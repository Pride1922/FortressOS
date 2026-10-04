#include "common.h"

#define GREP_LINE_CAP 4096
#define REGEX_MAX_TOKENS 64

typedef enum {
    TOK_LITERAL,
    TOK_DOT,
    TOK_CLASS,
    TOK_NCLASS
} token_kind_t;

typedef struct {
    token_kind_t kind;
    char ch;
    uint8_t cls[32]; /* 256-bit set */
    bool star;
} regex_token_t;

typedef struct {
    regex_token_t tokens[REGEX_MAX_TOKENS];
    int num_tokens;
    bool anchor_start;
    bool anchor_end;
    bool ignore_case;
} regex_t;

typedef struct {
    bool ignore_case;        /* -i */
    bool invert;             /* -v */
    bool count_only;         /* -c */
    bool line_numbers;       /* -n */
    bool files_with_matches; /* -l */
    bool quiet;              /* -q */
    bool no_filename;        /* -h */
    bool with_filename;      /* -H */
    bool fixed_strings;      /* -F */
    bool extended_regex;     /* -E */
    const char *pattern;
    int first_file;
} grep_options_t;

static char s_line_buf[GREP_LINE_CAP];
static char s_num_buf[32];
static regex_t s_regex;

static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static inline void set_bit(uint8_t *set, unsigned char b) {
    set[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static inline bool test_bit(const uint8_t *set, unsigned char b) {
    return (set[b >> 3] & (1u << (b & 7))) != 0;
}

static inline void set_bit_case(uint8_t *set, unsigned char b, bool icase) {
    set_bit(set, b);
    if (icase) {
        if (b >= 'a' && b <= 'z') set_bit(set, (unsigned char)(b - ('a' - 'A')));
        else if (b >= 'A' && b <= 'Z') set_bit(set, (unsigned char)(b + ('a' - 'A')));
    }
}

static size_t format_uint64(char *buf, size_t buf_size, uint64_t val) {
    char temp[32];
    size_t t_pos = 0;
    do {
        temp[t_pos++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val && t_pos < sizeof(temp));

    size_t out_len = 0;
    while (t_pos > 0 && out_len + 1 < buf_size) {
        buf[out_len++] = temp[--t_pos];
    }
    buf[out_len] = '\0';
    return out_len;
}

/* --------------------------------------------------------------------------
 * Iterative Bitmask NFA Engine (Pike VM) - O(1) Stack, O(N) Time
 * -------------------------------------------------------------------------- */

static inline uint64_t nfa_closure(const regex_t *re, uint64_t states) {
    for (int i = 0; i < re->num_tokens; i++) {
        if ((states & (1ULL << i)) && re->tokens[i].star) {
            states |= (1ULL << (i + 1));
        }
    }
    return states;
}

static inline bool match_token(const regex_token_t *tok, unsigned char c, bool icase) {
    if (c == '\0' || c == '\n' || c == '\r') return false;
    switch (tok->kind) {
    case TOK_DOT:
        return true;
    case TOK_LITERAL: {
        char c1 = tok->ch;
        char c2 = (char)c;
        if (icase) {
            c1 = to_lower(c1);
            c2 = to_lower(c2);
        }
        return c1 == c2;
    }
    case TOK_CLASS:
        return test_bit(tok->cls, c);
    case TOK_NCLASS:
        return !test_bit(tok->cls, c);
    }
    return false;
}

static inline uint64_t nfa_step(const regex_t *re, uint64_t states, unsigned char c) {
    uint64_t next_states = 0;
    for (int i = 0; i < re->num_tokens; i++) {
        if (states & (1ULL << i)) {
            if (match_token(&re->tokens[i], c, re->ignore_case)) {
                if (re->tokens[i].star) {
                    /* Repetition: can loop on state i and also advance to state i+1 */
                    next_states |= (1ULL << i) | (1ULL << (i + 1));
                } else {
                    next_states |= (1ULL << (i + 1));
                }
            }
        }
    }
    return nfa_closure(re, next_states);
}

static bool compile_pattern(regex_t *re, const char *pattern, bool icase, bool fixed) {
    for (size_t i = 0; i < sizeof(*re); i++) ((char *)re)[i] = 0;
    re->ignore_case = icase;

    if (fixed) {
        /* Fixed strings (-F): every character is a literal token, no metacharacters */
        for (const char *p = pattern; *p; p++) {
            if (re->num_tokens >= REGEX_MAX_TOKENS) return false;
            regex_token_t *tok = &re->tokens[re->num_tokens++];
            tok->kind = TOK_LITERAL;
            tok->ch = *p;
            tok->star = false;
        }
        return true;
    }

    const char *p = pattern;
    if (*p == '^') {
        re->anchor_start = true;
        p++;
    }

    while (*p) {
        if (*p == '$' && p[1] == '\0') {
            re->anchor_end = true;
            p++;
            break;
        }

        if (re->num_tokens >= REGEX_MAX_TOKENS) return false;
        regex_token_t *tok = &re->tokens[re->num_tokens];

        if (*p == '\\' && p[1]) {
            p++;
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            re->num_tokens++;
        } else if (*p == '.') {
            tok->kind = TOK_DOT;
            tok->star = false;
            p++;
            re->num_tokens++;
        } else if (*p == '[') {
            p++;
            bool negated = false;
            if (*p == '^') {
                negated = true;
                p++;
            }
            tok->kind = negated ? TOK_NCLASS : TOK_CLASS;
            tok->star = false;

            /* If ']' is immediately first, it is literal */
            if (*p == ']') {
                set_bit_case(tok->cls, (unsigned char)*p, icase);
                p++;
            }

            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    unsigned char start = (unsigned char)p[0];
                    unsigned char end = (unsigned char)p[2];
                    if (start > end) {
                        unsigned char tmp = start; start = end; end = tmp;
                    }
                    for (unsigned i = start; i <= end; i++) {
                        set_bit_case(tok->cls, (unsigned char)i, icase);
                    }
                    p += 3;
                } else {
                    set_bit_case(tok->cls, (unsigned char)*p, icase);
                    p++;
                }
            }
            if (*p == ']') p++;
            re->num_tokens++;
        } else if (*p == '*') {
            if (re->num_tokens > 0) {
                re->tokens[re->num_tokens - 1].star = true;
            }
            while (*p == '*') p++;
            continue;
        } else {
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            re->num_tokens++;
        }
    }

    return true;
}

static int parse_grep_options(int argc, char **argv, grep_options_t *opts) {
    for (size_t i = 0; i < sizeof(*opts); i++) ((char *)opts)[i] = 0;
    opts->first_file = argc;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) return 1; /* help */
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] != '-' || !arg[1] || tool_equal(arg, "-")) {
            break;
        }

        for (size_t j = 1; arg[j]; j++) {
            switch (arg[j]) {
            case 'i': opts->ignore_case = true; break;
            case 'v': opts->invert = true; break;
            case 'c': opts->count_only = true; break;
            case 'n': opts->line_numbers = true; break;
            case 'l': opts->files_with_matches = true; break;
            case 'q': opts->quiet = true; break;
            case 'h': opts->no_filename = true; break;
            case 'H': opts->with_filename = true; break;
            case 'F': opts->fixed_strings = true; break;
            case 'E': opts->extended_regex = true; break;
            default:
                tool_error("grep", "invalid option; use --help", NULL);
                return 2;
            }
        }
    }

    if (i >= argc) {
        tool_error("grep", "missing pattern; use --help", NULL);
        return 2;
    }

    opts->pattern = argv[i++];
    opts->first_file = i;
    return 0;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: grep [OPTIONS] PATTERN [FILE ...]\n"
        "Search for PATTERN in each FILE or standard input.\n\n"
        "Options:\n"
        "  -i        Ignore case distinctions\n"
        "  -v        Invert the sense of matching\n"
        "  -c        Print only a count of selected lines\n"
        "  -n        Prefix each line with 1-based line number\n"
        "  -l        Print only names of files with matching lines\n"
        "  -q        Quiet; exit 0 if any match found, 1 otherwise\n"
        "  -h        Suppress file name prefix on output\n"
        "  -H        Print file name prefix for each match\n"
        "  -F        Interpret PATTERN as a fixed string\n"
        "  -E        Interpret PATTERN as an extended regular expression\n"
        "  --help    Display this help text and exit\n";
    return tool_write("grep", help_text, sizeof(help_text) - 1);
}

/* --------------------------------------------------------------------------
 * Streaming Line Processor with Rolling Window Across Arbitrary Line Lengths
 * -------------------------------------------------------------------------- */

static int process_stream(int fd, const char *label, const grep_options_t *opts,
                          bool show_filename, bool *out_matched) {
    size_t line_len = 0;
    uint64_t line_no = 1;
    uint64_t match_count = 0;
    bool matched_any_in_file = false;

    uint64_t accept_mask = (1ULL << s_regex.num_tokens);
    uint64_t nfa_states = nfa_closure(&s_regex, 1ULL << 0);
    bool line_matched = (!s_regex.anchor_end && (nfa_states & accept_mask));
    bool line_overflow = false;
    bool line_prefix_printed = false;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) {
            return tool_error("grep", "read error", label);
        }
        if (n == 0) {
            /* EOF reached */
            if (line_len > 0 || line_overflow) {
                if (s_regex.anchor_end && (nfa_states & accept_mask)) {
                    line_matched = true;
                }
                bool select = opts->invert ? !line_matched : line_matched;
                if (select) {
                    matched_any_in_file = true;
                    *out_matched = true;
                    if (opts->quiet) return 0;
                    if (opts->files_with_matches) goto done_file;
                    if (opts->count_only) {
                        match_count++;
                    } else {
                        if (!line_prefix_printed) {
                            if (show_filename && label) {
                                (void)tool_write("grep", label, tool_length(label));
                                (void)tool_write("grep", ":", 1);
                            }
                            if (opts->line_numbers) {
                                size_t nlen = format_uint64(s_num_buf, sizeof(s_num_buf), line_no);
                                (void)tool_write("grep", s_num_buf, nlen);
                                (void)tool_write("grep", ":", 1);
                            }
                        }
                        if (line_len > 0) {
                            (void)tool_write("grep", s_line_buf, line_len);
                        }
                        (void)tool_write("grep", "\n", 1);
                    }
                }
            }
            break;
        }

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == '\n') {
                /* End of line reached */
                if (line_len > 0 && s_line_buf[line_len - 1] == '\r') {
                    line_len--;
                }
                if (s_regex.anchor_end && (nfa_states & accept_mask)) {
                    line_matched = true;
                }

                bool select = opts->invert ? !line_matched : line_matched;
                if (select) {
                    matched_any_in_file = true;
                    *out_matched = true;
                    if (opts->quiet) return 0;
                    if (opts->files_with_matches) goto done_file;
                    if (opts->count_only) {
                        match_count++;
                    } else {
                        if (!line_prefix_printed) {
                            if (show_filename && label) {
                                (void)tool_write("grep", label, tool_length(label));
                                (void)tool_write("grep", ":", 1);
                            }
                            if (opts->line_numbers) {
                                size_t nlen = format_uint64(s_num_buf, sizeof(s_num_buf), line_no);
                                (void)tool_write("grep", s_num_buf, nlen);
                                (void)tool_write("grep", ":", 1);
                            }
                        }
                        if (line_len > 0) {
                            (void)tool_write("grep", s_line_buf, line_len);
                        }
                        (void)tool_write("grep", "\n", 1);
                    }
                }

                /* Reset line state for next line */
                line_no++;
                line_len = 0;
                line_overflow = false;
                line_prefix_printed = false;
                nfa_states = nfa_closure(&s_regex, 1ULL << 0);
                line_matched = (!s_regex.anchor_end && (nfa_states & accept_mask));
            } else {
                /* Feed character to NFA */
                if (!s_regex.anchor_start) {
                    nfa_states |= (1ULL << 0);
                    nfa_states = nfa_closure(&s_regex, nfa_states);
                }
                nfa_states = nfa_step(&s_regex, nfa_states, (unsigned char)c);
                if (!s_regex.anchor_end && (nfa_states & accept_mask)) {
                    line_matched = true;
                }

                /* Rolling line buffer management */
                if (line_len + 1 < sizeof(s_line_buf)) {
                    s_line_buf[line_len++] = c;
                } else {
                    /* Overlong line: buffer full before newline */
#define GREP_OVERLAP 256
                    line_overflow = true;
                    if (line_matched) {
                        /* Line already matched: flush and continue streaming */
                        if (!line_prefix_printed) {
                            if (show_filename && label) {
                                (void)tool_write("grep", label, tool_length(label));
                                (void)tool_write("grep", ":", 1);
                            }
                            if (opts->line_numbers) {
                                size_t nlen = format_uint64(s_num_buf, sizeof(s_num_buf), line_no);
                                (void)tool_write("grep", s_num_buf, nlen);
                                (void)tool_write("grep", ":", 1);
                            }
                            line_prefix_printed = true;
                        }
                        (void)tool_write("grep", s_line_buf, line_len);
                        line_len = 0;
                        s_line_buf[line_len++] = c;
                    } else {
                        /* Match not found yet: slide the last GREP_OVERLAP bytes to start of buffer */
                        size_t keep = line_len > GREP_OVERLAP ? GREP_OVERLAP : line_len;
                        for (size_t k = 0; k < keep; k++) {
                            s_line_buf[k] = s_line_buf[line_len - keep + k];
                        }
                        line_len = keep;
                        s_line_buf[line_len++] = c;
                    }
                }
            }
        }
    }

done_file:
    if (opts->quiet) return 0;

    if (opts->files_with_matches) {
        if (matched_any_in_file) {
            const char *print_name = label ? label : "(standard input)";
            (void)tool_write("grep", print_name, tool_length(print_name));
            (void)tool_write("grep", "\n", 1);
        }
        return 0;
    }

    if (opts->count_only) {
        if (show_filename && label) {
            (void)tool_write("grep", label, tool_length(label));
            (void)tool_write("grep", ":", 1);
        }
        size_t clen = format_uint64(s_num_buf, sizeof(s_num_buf), match_count);
        (void)tool_write("grep", s_num_buf, clen);
        (void)tool_write("grep", "\n", 1);
    }

    return 0;
}

int grep_main(int argc, char **argv) {
    grep_options_t opts;
    int opt_res = parse_grep_options(argc, argv, &opts);
    if (opt_res == 1) return print_help();
    if (opt_res != 0) return opt_res;

    if (!compile_pattern(&s_regex, opts.pattern, opts.ignore_case, opts.fixed_strings)) {
        return tool_error("grep", "invalid regular expression", opts.pattern);
    }

    int file_count = argc - opts.first_file;
    bool multiple_files = file_count > 1;
    bool show_filename = opts.with_filename || (multiple_files && !opts.no_filename);

    bool any_match = false;
    bool any_error = false;

    if (file_count == 0) {
        /* Read from standard input */
        int r = process_stream(0, NULL, &opts, show_filename, &any_match);
        if (r != 0) any_error = true;
    } else {
        for (int i = opts.first_file; i < argc; i++) {
            const char *file_arg = argv[i];
            if (tool_equal(file_arg, "-")) {
                int r = process_stream(0, "(standard input)", &opts, show_filename, &any_match);
                if (r != 0) any_error = true;
                if (opts.quiet && any_match) return 0;
                continue;
            }

            vfs_stat_t st;
            if (tool_syscall(SYS_STAT, (uintptr_t)file_arg, (uintptr_t)&st, 0) < 0) {
                tool_error("grep", "cannot open", file_arg);
                any_error = true;
                continue;
            }
            if (st.type == VFS_DIRECTORY) {
                tool_error("grep", "is a directory", file_arg);
                any_error = true;
                continue;
            }

            long fd = tool_syscall(SYS_OPEN, (uintptr_t)file_arg, VFS_O_RDONLY, 0);
            if (fd < 0) {
                tool_error("grep", "cannot open", file_arg);
                any_error = true;
                continue;
            }

            int r = process_stream((int)fd, file_arg, &opts, show_filename, &any_match);
            (void)tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            if (r != 0) any_error = true;
            if (opts.quiet && any_match) return 0;
        }
    }

    if (opts.quiet) {
        return any_match ? 0 : 1;
    }
    if (any_error) return 2;
    return any_match ? 0 : 1;
}
