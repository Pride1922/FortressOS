#include "common.h"

#define SED_LINE_CAP       65536
#define SED_MAX_TOKENS     64
#define SED_MAX_COMMANDS   64
#define SED_MAX_GROUPS     9
#define SED_MAX_FILES      64
#define SED_PATH_CAP       256

typedef enum {
    TOK_LITERAL,
    TOK_DOT,
    TOK_CLASS,
    TOK_NCLASS
} token_kind_t;

typedef struct {
    token_kind_t kind;
    char ch;
    uint8_t cls[32];           /* 256-bit set */
    bool star;
    uint16_t group_start_mask; /* bit 1..9 if group g opens before this token */
    uint16_t group_end_mask;   /* bit 1..9 if group g closes after this token */
} regex_token_t;

typedef struct {
    regex_token_t tokens[SED_MAX_TOKENS];
    int num_tokens;
    bool anchor_start;
    bool anchor_end;
    bool ignore_case;
    int num_groups;
} regex_t;

typedef struct {
    int32_t start[10]; /* group 0 is full match, 1-9 are capture groups */
    int32_t end[10];
} sed_capture_t;

typedef enum {
    ADDR_NONE,
    ADDR_LINE_NUM,
    ADDR_PATTERN,
    ADDR_LAST_LINE
} addr_type_t;

typedef struct {
    addr_type_t type;
    uint64_t line_num;
    regex_t pattern;
} sed_addr_spec_t;

typedef struct {
    sed_addr_spec_t start;
    sed_addr_spec_t end;
    bool is_range;
    bool negate;
    bool active;
} sed_addr_t;

typedef enum {
    CMD_SUBST,
    CMD_PRINT,
    CMD_DELETE
} sed_cmd_type_t;

typedef struct {
    sed_addr_t addr;
    sed_cmd_type_t type;
    /* For CMD_SUBST: */
    regex_t pattern;
    char replacement[2048];
    size_t repl_len;
    bool flag_g;
    bool flag_p;
    bool flag_i;
    uint32_t flag_n;
} sed_command_t;

typedef struct {
    bool quiet;              /* -n */
    bool in_place;           /* -i */
    const char *bak_suffix;  /* .bak or empty */
    const char *files[SED_MAX_FILES];
    int num_files;
} sed_opts_t;

typedef struct {
    int fd;
    unsigned char buf[TOOL_BUFFER_SIZE];
    size_t pos;
    size_t len;
    bool eof;
} line_reader_t;

/* Static BSS storage — bounded, zero dynamic allocation */
static char s_curr_line[SED_LINE_CAP];
static char s_next_line[SED_LINE_CAP];
static char s_subst_buf[SED_LINE_CAP];
static sed_command_t s_commands[SED_MAX_COMMANDS];
static int s_num_commands = 0;
static regex_t s_last_regex;
static bool s_has_last_regex = false;
static sed_capture_t s_state_caps[SED_MAX_TOKENS + 1];
static sed_capture_t s_next_caps[SED_MAX_TOKENS + 1];
static line_reader_t s_reader;
static sed_opts_t s_opts;
static char s_tmp_path[SED_PATH_CAP];
static char s_bak_path[SED_PATH_CAP];

/* --------------------------------------------------------------------------
 * Helpers & String Utilities
 * -------------------------------------------------------------------------- */

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

static int report_unsupported_cmd(char cmd_char) {
    char msg[32];
    char *m = msg;
    const char *pfx = "unsupported command '";
    while (*pfx) *m++ = *pfx++;
    *m++ = cmd_char;
    *m++ = '\'';
    *m++ = '\0';
    tool_error("sed", msg, NULL);
    return 2;
}

/* --------------------------------------------------------------------------
 * Regex Engine (Reusing grep bitmask NFA + submatch capture tracking)
 * -------------------------------------------------------------------------- */

static bool compile_pattern(regex_t *re, const char *pattern, size_t pat_len, bool icase) {
    for (size_t i = 0; i < sizeof(*re); i++) ((char *)re)[i] = 0;
    re->ignore_case = icase;
    re->num_tokens = 0;
    re->num_groups = 0;

    if (pat_len == 0) {
        return true; /* Empty pattern matches empty string */
    }

    const char *p = pattern;
    const char *end = pattern + pat_len;

    if (*p == '^') {
        re->anchor_start = true;
        p++;
    }

    int group_stack[16];
    int group_depth = 0;
    uint16_t pending_group_starts = 0;

    while (p < end) {
        if (*p == '$' && p + 1 == end) {
            re->anchor_end = true;
            p++;
            break;
        }

        if (re->num_tokens >= SED_MAX_TOKENS) return false;
        regex_token_t *tok = &re->tokens[re->num_tokens];

        if (*p == '\\' && p + 1 < end) {
            char next = p[1];
            if (next == '(') {
                if (re->num_groups < SED_MAX_GROUPS) {
                    re->num_groups++;
                    int g = re->num_groups;
                    if (group_depth < 16) {
                        group_stack[group_depth++] = g;
                    }
                    pending_group_starts |= (uint16_t)(1u << g);
                }
                p += 2;
                continue;
            } else if (next == ')') {
                if (group_depth > 0) {
                    int g = group_stack[--group_depth];
                    if (re->num_tokens > 0) {
                        re->tokens[re->num_tokens - 1].group_end_mask |= (uint16_t)(1u << g);
                    }
                }
                p += 2;
                continue;
            } else if (next == 'n') {
                tok->kind = TOK_LITERAL;
                tok->ch = '\n';
                tok->star = false;
                tok->group_start_mask = pending_group_starts;
                pending_group_starts = 0;
                re->num_tokens++;
                p += 2;
            } else if (next == 't') {
                tok->kind = TOK_LITERAL;
                tok->ch = '\t';
                tok->star = false;
                tok->group_start_mask = pending_group_starts;
                pending_group_starts = 0;
                re->num_tokens++;
                p += 2;
            } else {
                tok->kind = TOK_LITERAL;
                tok->ch = next;
                tok->star = false;
                tok->group_start_mask = pending_group_starts;
                pending_group_starts = 0;
                re->num_tokens++;
                p += 2;
            }
        } else if (*p == '.') {
            tok->kind = TOK_DOT;
            tok->star = false;
            tok->group_start_mask = pending_group_starts;
            pending_group_starts = 0;
            re->num_tokens++;
            p++;
        } else if (*p == '[') {
            p++;
            bool negated = false;
            if (p < end && *p == '^') {
                negated = true;
                p++;
            }
            tok->kind = negated ? TOK_NCLASS : TOK_CLASS;
            tok->star = false;
            tok->group_start_mask = pending_group_starts;
            pending_group_starts = 0;

            if (p < end && *p == ']') {
                set_bit_case(tok->cls, (unsigned char)*p, icase);
                p++;
            }

            while (p < end && *p != ']') {
                if (p + 2 < end && p[1] == '-' && p[2] != ']') {
                    unsigned char start = (unsigned char)p[0];
                    unsigned char stop = (unsigned char)p[2];
                    if (start > stop) {
                        unsigned char tmp = start; start = stop; stop = tmp;
                    }
                    for (unsigned i = start; i <= stop; i++) {
                        set_bit_case(tok->cls, (unsigned char)i, icase);
                    }
                    p += 3;
                } else {
                    set_bit_case(tok->cls, (unsigned char)*p, icase);
                    p++;
                }
            }
            if (p < end && *p == ']') p++;
            re->num_tokens++;
        } else if (*p == '*') {
            if (re->num_tokens > 0) {
                re->tokens[re->num_tokens - 1].star = true;
            }
            while (p < end && *p == '*') p++;
        } else {
            tok->kind = TOK_LITERAL;
            tok->ch = *p++;
            tok->star = false;
            tok->group_start_mask = pending_group_starts;
            pending_group_starts = 0;
            re->num_tokens++;
        }
    }

    return (group_depth == 0);
}

static uint64_t sed_nfa_closure(const regex_t *re, uint64_t states, sed_capture_t *caps, int32_t cur_pos) {
    for (int i = 0; i < re->num_tokens; i++) {
        if ((states & (1ULL << i)) && re->tokens[i].star) {
            uint64_t next_bit = (1ULL << (i + 1));
            if (!(states & next_bit)) {
                states |= next_bit;
                caps[i + 1] = caps[i];
                for (int g = 1; g <= 9; g++) {
                    if (re->tokens[i].group_start_mask & (1 << g)) {
                        if (caps[i + 1].start[g] < 0) {
                            caps[i + 1].start[g] = cur_pos;
                        }
                    }
                    if (re->tokens[i].group_end_mask & (1 << g)) {
                        if (caps[i + 1].end[g] < 0) {
                            caps[i + 1].end[g] = cur_pos;
                        }
                    }
                }
            }
        }
    }
    return states;
}

static uint64_t sed_nfa_step(const regex_t *re, uint64_t states, const sed_capture_t *cur_caps,
                             sed_capture_t *next_caps, unsigned char c, int32_t cur_pos) {
    uint64_t next_states = 0;
    for (int i = 0; i < re->num_tokens; i++) {
        if (states & (1ULL << i)) {
            if (match_token(&re->tokens[i], c, re->ignore_case)) {
                sed_capture_t updated = cur_caps[i];
                for (int g = 1; g <= 9; g++) {
                    if (re->tokens[i].group_start_mask & (1 << g)) {
                        if (updated.start[g] < 0) {
                            updated.start[g] = cur_pos;
                        }
                    }
                    if (re->tokens[i].group_end_mask & (1 << g)) {
                        updated.end[g] = cur_pos + 1;
                    }
                }

                if (re->tokens[i].star) {
                    next_states |= (1ULL << i);
                    next_caps[i] = updated;
                    if (!(next_states & (1ULL << (i + 1)))) {
                        next_states |= (1ULL << (i + 1));
                        next_caps[i + 1] = updated;
                    }
                } else {
                    if (!(next_states & (1ULL << (i + 1)))) {
                        next_states |= (1ULL << (i + 1));
                        next_caps[i + 1] = updated;
                    }
                }
            }
        }
    }
    return sed_nfa_closure(re, next_states, next_caps, cur_pos + 1);
}

static bool sed_find_match(const regex_t *re, const char *line, size_t line_len, size_t start_pos, sed_capture_t *out_cap) {
    if (re->num_tokens == 0) {
        if (re->anchor_start && re->anchor_end) {
            if (line_len == 0 && start_pos == 0) {
                out_cap->start[0] = 0;
                out_cap->end[0] = 0;
                for (int g = 1; g <= 9; g++) {
                    out_cap->start[g] = -1;
                    out_cap->end[g] = -1;
                }
                return true;
            }
            return false;
        }
        if (re->anchor_start) {
            if (start_pos == 0) {
                out_cap->start[0] = 0;
                out_cap->end[0] = 0;
                for (int g = 1; g <= 9; g++) {
                    out_cap->start[g] = -1;
                    out_cap->end[g] = -1;
                }
                return true;
            }
            return false;
        }
        if (re->anchor_end) {
            if (start_pos <= line_len) {
                out_cap->start[0] = (int32_t)line_len;
                out_cap->end[0] = (int32_t)line_len;
                for (int g = 1; g <= 9; g++) {
                    out_cap->start[g] = -1;
                    out_cap->end[g] = -1;
                }
                return true;
            }
            return false;
        }
        if (start_pos <= line_len) {
            out_cap->start[0] = (int32_t)start_pos;
            out_cap->end[0] = (int32_t)start_pos;
            for (int g = 1; g <= 9; g++) {
                out_cap->start[g] = -1;
                out_cap->end[g] = -1;
            }
            return true;
        }
        return false;
    }

    size_t min_pos = start_pos;
    size_t max_pos = re->anchor_start ? (start_pos == 0 ? 0 : 1) : line_len;
    if (re->anchor_start && start_pos > 0) return false;

    uint64_t accept_mask = (1ULL << re->num_tokens);

    for (size_t pos = min_pos; pos <= max_pos; pos++) {
        for (int i = 0; i <= re->num_tokens; i++) {
            for (int g = 0; g <= 9; g++) {
                s_state_caps[i].start[g] = -1;
                s_state_caps[i].end[g] = -1;
            }
        }
        s_state_caps[0].start[0] = (int32_t)pos;

        uint64_t states = sed_nfa_closure(re, 1ULL << 0, s_state_caps, (int32_t)pos);

        bool found_match_at_pos = false;
        sed_capture_t best_at_pos;

        if (states & accept_mask) {
            if (!re->anchor_end || pos == line_len) {
                found_match_at_pos = true;
                best_at_pos = s_state_caps[re->num_tokens];
                best_at_pos.end[0] = (int32_t)pos;
            }
        }

        for (size_t j = pos; j < line_len; j++) {
            for (int i = 0; i <= re->num_tokens; i++) {
                for (int g = 0; g <= 9; g++) {
                    s_next_caps[i].start[g] = -1;
                    s_next_caps[i].end[g] = -1;
                }
            }

            states = sed_nfa_step(re, states, s_state_caps, s_next_caps, (unsigned char)line[j], (int32_t)j);
            for (int i = 0; i <= re->num_tokens; i++) {
                s_state_caps[i] = s_next_caps[i];
            }

            if (states & accept_mask) {
                if (!re->anchor_end || (j + 1 == line_len)) {
                    found_match_at_pos = true;
                    best_at_pos = s_state_caps[re->num_tokens];
                    best_at_pos.end[0] = (int32_t)(j + 1);
                }
            }

            if (states == 0) break;
        }

        if (found_match_at_pos) {
            *out_cap = best_at_pos;
            return true;
        }
    }

    return false;
}

static bool sed_regex_matches(const regex_t *re, const char *line, size_t line_len) {
    sed_capture_t cap;
    return sed_find_match(re, line, line_len, 0, &cap);
}

/* --------------------------------------------------------------------------
 * Address Evaluation
 * -------------------------------------------------------------------------- */

static bool match_single_addr(const sed_addr_spec_t *spec, uint64_t line_no, bool is_last,
                              const char *line, size_t line_len) {
    switch (spec->type) {
    case ADDR_NONE:
        return true;
    case ADDR_LINE_NUM:
        return line_no == spec->line_num;
    case ADDR_LAST_LINE:
        return is_last;
    case ADDR_PATTERN:
        return sed_regex_matches(&spec->pattern, line, line_len);
    }
    return false;
}

static bool eval_address(sed_addr_t *addr, uint64_t line_no, bool is_last,
                         const char *line, size_t line_len) {
    if (addr->start.type == ADDR_NONE) {
        return !addr->negate;
    }

    bool matched = false;
    if (!addr->is_range) {
        matched = match_single_addr(&addr->start, line_no, is_last, line, line_len);
    } else {
        if (!addr->active) {
            if (match_single_addr(&addr->start, line_no, is_last, line, line_len)) {
                addr->active = true;
                matched = true;
                if (addr->end.type == ADDR_LINE_NUM && addr->end.line_num <= line_no) {
                    addr->active = false;
                }
            }
        } else {
            matched = true;
            if (match_single_addr(&addr->end, line_no, is_last, line, line_len)) {
                addr->active = false;
            }
        }
    }

    if (addr->negate) {
        matched = !matched;
    }
    return matched;
}

/* --------------------------------------------------------------------------
 * Substitution Command Implementation
 * -------------------------------------------------------------------------- */

static int do_substitution(const sed_command_t *cmd, const char *in, size_t in_len,
                           char *out, size_t out_cap, size_t *out_len, bool *out_substituted) {
    size_t src_pos = 0;
    size_t dst_pos = 0;
    uint32_t match_count = 0;
    bool substituted = false;

    while (src_pos <= in_len) {
        sed_capture_t match;
        if (!sed_find_match(&cmd->pattern, in, in_len, src_pos, &match)) {
            while (src_pos < in_len) {
                if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                out[dst_pos++] = in[src_pos++];
            }
            break;
        }

        match_count++;
        bool apply = true;
        if (cmd->flag_n > 0) {
            apply = (match_count == cmd->flag_n);
        }

        if (!apply) {
            size_t copy_end = (size_t)match.end[0];
            if (copy_end < (size_t)match.start[0]) copy_end = (size_t)match.start[0];
            while (src_pos < copy_end) {
                if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                out[dst_pos++] = in[src_pos++];
            }
            src_pos = copy_end;
            if (match.start[0] == match.end[0]) {
                if (src_pos < in_len) {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = in[src_pos++];
                } else {
                    break;
                }
            }
            continue;
        }

        /* Copy prefix before match */
        size_t m_start = (size_t)match.start[0];
        while (src_pos < m_start) {
            if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
            out[dst_pos++] = in[src_pos++];
        }

        /* Expand replacement */
        substituted = true;
        for (size_t k = 0; k < cmd->repl_len; k++) {
            char c = cmd->replacement[k];
            if (c == '&') {
                size_t ms = (size_t)match.start[0];
                size_t me = (size_t)match.end[0];
                for (size_t idx = ms; idx < me; idx++) {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = in[idx];
                }
            } else if (c == '\\' && k + 1 < cmd->repl_len) {
                k++;
                char next = cmd->replacement[k];
                if (next >= '1' && next <= '9') {
                    int g = next - '0';
                    if (match.start[g] >= 0 && match.end[g] >= match.start[g]) {
                        size_t gs = (size_t)match.start[g];
                        size_t ge = (size_t)match.end[g];
                        for (size_t idx = gs; idx < ge; idx++) {
                            if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                            out[dst_pos++] = in[idx];
                        }
                    }
                } else if (next == '&') {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = '&';
                } else if (next == '\\') {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = '\\';
                } else if (next == 'n') {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = '\n';
                } else if (next == 't') {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = '\t';
                } else {
                    if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                    out[dst_pos++] = next;
                }
            } else {
                if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                out[dst_pos++] = c;
            }
        }

        src_pos = (size_t)match.end[0];

        if (!cmd->flag_g) {
            /* Non-global: copy remainder and finish */
            while (src_pos < in_len) {
                if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                out[dst_pos++] = in[src_pos++];
            }
            break;
        }

        if (match.start[0] == match.end[0]) {
            if (src_pos < in_len) {
                if (dst_pos + 1 >= out_cap) return tool_error("sed", "line too long after substitution", NULL);
                out[dst_pos++] = in[src_pos++];
            } else {
                break;
            }
        }
    }

    out[dst_pos] = '\0';
    *out_len = dst_pos;
    *out_substituted = substituted;
    return 0;
}

/* --------------------------------------------------------------------------
 * Script Parser
 * -------------------------------------------------------------------------- */

static const char *skip_ws_and_comments(const char *p) {
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        } else {
            break;
        }
    }
    return p;
}

static int parse_addr_spec(const char **pp, sed_addr_spec_t *spec) {
    const char *p = *pp;
    if (*p >= '0' && *p <= '9') {
        uint64_t val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint64_t)(*p++ - '0');
        }
        spec->type = ADDR_LINE_NUM;
        spec->line_num = val;
        *pp = p;
        return 0;
    }
    if (*p == '$') {
        spec->type = ADDR_LAST_LINE;
        *pp = p + 1;
        return 0;
    }
    if (*p == '/') {
        p++;
        const char *start = p;
        while (*p && *p != '/') {
            if (*p == '\\' && p[1]) p += 2;
            else p++;
        }
        if (*p != '/') {
            tool_error("sed", "unterminated address regex", NULL);
            return 2;
        }
        size_t pat_len = (size_t)(p - start);
        if (pat_len == 0) {
            if (!s_has_last_regex) {
                tool_error("sed", "no previous regular expression", NULL);
                return 2;
            }
            spec->pattern = s_last_regex;
        } else {
            if (!compile_pattern(&spec->pattern, start, pat_len, false)) {
                tool_error("sed", "invalid address regular expression", NULL);
                return 2;
            }
            s_last_regex = spec->pattern;
            s_has_last_regex = true;
        }
        spec->type = ADDR_PATTERN;
        *pp = p + 1;
        return 0;
    }
    spec->type = ADDR_NONE;
    return 0;
}

static int parse_script(const char *script) {
    const char *p = script;

    for (;;) {
        p = skip_ws_and_comments(p);
        if (!*p) break;
        if (*p == ';' || *p == '\n') {
            p++;
            continue;
        }

        if (s_num_commands >= SED_MAX_COMMANDS) {
            tool_error("sed", "too many commands", NULL);
            return 2;
        }

        sed_command_t *cmd = &s_commands[s_num_commands];
        for (size_t i = 0; i < sizeof(*cmd); i++) ((char *)cmd)[i] = 0;

        /* 1. Parse Address */
        int r = parse_addr_spec(&p, &cmd->addr.start);
        if (r != 0) return r;

        if (cmd->addr.start.type != ADDR_NONE) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') {
                p++;
                cmd->addr.is_range = true;
                while (*p == ' ' || *p == '\t') p++;
                r = parse_addr_spec(&p, &cmd->addr.end);
                if (r != 0) return r;
                if (cmd->addr.end.type == ADDR_NONE) {
                    tool_error("sed", "invalid second address in range", NULL);
                    return 2;
                }
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '!') {
                cmd->addr.negate = true;
                p++;
            }
        }

        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {
            tool_error("sed", "missing command after address", NULL);
            return 2;
        }

        char c = *p;

        /* 2. Check for unsupported commands explicitly */
        if (c == 'a' || c == 'i' || c == 'c' || c == 'r' || c == 'w' ||
            c == 'y' || c == 'h' || c == 'H' || c == 'g' || c == 'G' ||
            c == 'x' || c == 'b' || c == 't' || c == ':' || c == '{' ||
            c == '}' || c == '=' || c == 'q' || c == 'Q') {
            return report_unsupported_cmd(c);
        }

        if (c == 'p') {
            cmd->type = CMD_PRINT;
            p++;
        } else if (c == 'd') {
            cmd->type = CMD_DELETE;
            p++;
        } else if (c == 's') {
            cmd->type = CMD_SUBST;
            p++;
            char delim = *p++;
            if (!delim || delim == '\n' || delim == '\\') {
                tool_error("sed", "bad delimiter in s command", NULL);
                return 2;
            }

            /* Parse Pattern */
            const char *pat_start = p;
            while (*p && *p != delim) {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p != delim) {
                tool_error("sed", "unterminated s command pattern", NULL);
                return 2;
            }
            size_t pat_len = (size_t)(p - pat_start);
            p++; /* skip delim */

            /* Parse Replacement */
            size_t repl_len = 0;
            while (*p && *p != delim) {
                if (*p == '\\' && p[1] == delim) {
                    if (repl_len + 1 >= sizeof(cmd->replacement)) {
                        tool_error("sed", "replacement string too long", NULL);
                        return 2;
                    }
                    cmd->replacement[repl_len++] = delim;
                    p += 2;
                } else if (*p == '\\' && p[1]) {
                    if (repl_len + 2 >= sizeof(cmd->replacement)) {
                        tool_error("sed", "replacement string too long", NULL);
                        return 2;
                    }
                    cmd->replacement[repl_len++] = *p++;
                    cmd->replacement[repl_len++] = *p++;
                } else {
                    if (repl_len + 1 >= sizeof(cmd->replacement)) {
                        tool_error("sed", "replacement string too long", NULL);
                        return 2;
                    }
                    cmd->replacement[repl_len++] = *p++;
                }
            }
            if (*p != delim) {
                tool_error("sed", "unterminated s command replacement", NULL);
                return 2;
            }
            cmd->replacement[repl_len] = '\0';
            cmd->repl_len = repl_len;
            p++; /* skip delim */

            /* Parse Flags */
            bool icase = false;
            while (*p && *p != ';' && *p != '\n' && *p != ' ' && *p != '\t') {
                if (*p == 'g') {
                    cmd->flag_g = true;
                    p++;
                } else if (*p == 'p') {
                    cmd->flag_p = true;
                    p++;
                } else if (*p == 'i' || *p == 'I') {
                    cmd->flag_i = true;
                    icase = true;
                    p++;
                } else if (*p >= '0' && *p <= '9') {
                    uint32_t val = 0;
                    while (*p >= '0' && *p <= '9') {
                        val = val * 10 + (uint32_t)(*p++ - '0');
                    }
                    cmd->flag_n = val;
                } else {
                    return report_unsupported_cmd(*p);
                }
            }

            /* Compile Pattern */
            if (pat_len == 0) {
                if (!s_has_last_regex) {
                    tool_error("sed", "no previous regular expression", NULL);
                    return 2;
                }
                cmd->pattern = s_last_regex;
                if (icase) cmd->pattern.ignore_case = true;
            } else {
                if (!compile_pattern(&cmd->pattern, pat_start, pat_len, icase)) {
                    tool_error("sed", "invalid regular expression in s command", NULL);
                    return 2;
                }
                s_last_regex = cmd->pattern;
                s_has_last_regex = true;
            }
        } else {
            return report_unsupported_cmd(c);
        }

        s_num_commands++;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '\n') p++;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Line Reader and Stream Processing
 * -------------------------------------------------------------------------- */

static int reader_getc(line_reader_t *r) {
    if (r->pos >= r->len) {
        if (r->eof) return -1;
        long n = tool_syscall(SYS_READ, (uintptr_t)r->fd, (uintptr_t)r->buf, sizeof(r->buf));
        if (n <= 0) {
            r->eof = true;
            return -1;
        }
        r->pos = 0;
        r->len = (size_t)n;
    }
    return r->buf[r->pos++];
}

static bool reader_read_line(line_reader_t *r, char *line_buf, size_t cap, size_t *out_len) {
    size_t len = 0;
    int c;
    while ((c = reader_getc(r)) >= 0) {
        if (c == '\n') {
            if (len > 0 && line_buf[len - 1] == '\r') {
                len--;
            }
            line_buf[len] = '\0';
            *out_len = len;
            return true;
        }
        if (len + 1 < cap) {
            line_buf[len++] = (char)c;
        } else {
            tool_error("sed", "line too long", NULL);
            return false;
        }
    }
    if (len > 0) {
        if (line_buf[len - 1] == '\r') {
            len--;
        }
        line_buf[len] = '\0';
        *out_len = len;
        return true;
    }
    return false;
}

static int write_output_line(int out_fd, const char *line, size_t line_len) {
    if (out_fd == 1) {
        if (line_len > 0) {
            int r = tool_write("sed", line, line_len);
            if (r != 0) return r;
        }
        return tool_write("sed", "\n", 1);
    } else {
        if (line_len > 0) {
            long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)line, line_len);
            if (w < 0 || (size_t)w != line_len) return tool_error("sed", "write error", NULL);
        }
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)"\n", 1);
        if (w < 0 || w != 1) return tool_error("sed", "write error", NULL);
        return 0;
    }
}

static int process_line(char *line, size_t line_len, uint64_t line_no, bool is_last,
                        int out_fd, const sed_opts_t *opts) {
    bool deleted = false;

    for (int i = 0; i < s_num_commands; i++) {
        sed_command_t *cmd = &s_commands[i];
        if (!eval_address(&cmd->addr, line_no, is_last, line, line_len)) {
            continue;
        }

        switch (cmd->type) {
        case CMD_PRINT:
            if (write_output_line(out_fd, line, line_len) != 0) return 1;
            break;

        case CMD_DELETE:
            deleted = true;
            goto done_line;

        case CMD_SUBST: {
            bool substituted = false;
            size_t new_len = 0;
            int r = do_substitution(cmd, line, line_len, s_subst_buf, sizeof(s_subst_buf), &new_len, &substituted);
            if (r != 0) return r;
            if (substituted) {
                for (size_t k = 0; k <= new_len; k++) {
                    line[k] = s_subst_buf[k];
                }
                line_len = new_len;
                if (cmd->flag_p) {
                    if (write_output_line(out_fd, line, line_len) != 0) return 1;
                }
            }
            break;
        }
        }
    }

done_line:
    if (!deleted && !opts->quiet) {
        if (write_output_line(out_fd, line, line_len) != 0) return 1;
    }

    return 0;
}

static int process_single_stream(int in_fd, int out_fd, uint64_t *p_line_no, const sed_opts_t *opts) {
    for (size_t i = 0; i < sizeof(s_reader); i++) ((char *)&s_reader)[i] = 0;
    s_reader.fd = in_fd;

    size_t curr_len = 0;
    size_t next_len = 0;

    if (!reader_read_line(&s_reader, s_curr_line, sizeof(s_curr_line), &curr_len)) {
        return 0;
    }

    for (;;) {
        bool has_next = reader_read_line(&s_reader, s_next_line, sizeof(s_next_line), &next_len);
        bool is_last = !has_next;
        (*p_line_no)++;

        int r = process_line(s_curr_line, curr_len, *p_line_no, is_last, out_fd, opts);
        if (r != 0) return r;

        if (is_last) break;

        for (size_t i = 0; i <= next_len; i++) {
            s_curr_line[i] = s_next_line[i];
        }
        curr_len = next_len;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * File Processing (Standard & In-Place)
 * -------------------------------------------------------------------------- */

static int process_file_inplace(const char *path, const sed_opts_t *opts) {
    if (tool_equal(path, "-")) {
        tool_error("sed", "-i cannot be used with standard input", NULL);
        return 2;
    }

    long in_fd = tool_syscall(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0);
    if (in_fd < 0) {
        tool_error("sed", "cannot open", path);
        return 2;
    }

    size_t plen = tool_length(path);
    if (plen + 5 >= sizeof(s_tmp_path)) {
        tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
        tool_error("sed", "path too long", path);
        return 2;
    }

    for (size_t i = 0; i < plen; i++) s_tmp_path[i] = path[i];
    s_tmp_path[plen] = '.';
    s_tmp_path[plen + 1] = 't';
    s_tmp_path[plen + 2] = 'm';
    s_tmp_path[plen + 3] = 'p';
    s_tmp_path[plen + 4] = '\0';

    long out_fd = tool_syscall(SYS_OPEN, (uintptr_t)s_tmp_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (out_fd < 0) {
        tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
        tool_error("sed", "cannot create temporary file", s_tmp_path);
        return 2;
    }

    /* Reset address states for independent file processing */
    for (int i = 0; i < s_num_commands; i++) {
        s_commands[i].addr.active = false;
    }

    uint64_t line_no = 0;
    int r = process_single_stream((int)in_fd, (int)out_fd, &line_no, opts);
    tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
    tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);

    if (r != 0) {
        tool_syscall(SYS_UNLINK, (uintptr_t)s_tmp_path, 0, 0);
        return r;
    }

    /* If backup suffix is requested, rename original to backup */
    if (opts->bak_suffix && opts->bak_suffix[0]) {
        size_t slen = tool_length(opts->bak_suffix);
        if (plen + slen + 1 < sizeof(s_bak_path)) {
            for (size_t i = 0; i < plen; i++) s_bak_path[i] = path[i];
            for (size_t i = 0; i < slen; i++) s_bak_path[plen + i] = opts->bak_suffix[i];
            s_bak_path[plen + slen] = '\0';

            long ren_bak = tool_syscall(SYS_RENAME, (uintptr_t)path, (uintptr_t)s_bak_path, 0);
            if (ren_bak != 0) {
                tool_syscall(SYS_UNLINK, (uintptr_t)s_bak_path, 0, 0);
                tool_syscall(SYS_RENAME, (uintptr_t)path, (uintptr_t)s_bak_path, 0);
            }
        }
    }

    /* Atomically rename temporary file to target path */
    long ren_r = tool_syscall(SYS_RENAME, (uintptr_t)s_tmp_path, (uintptr_t)path, 0);
    if (ren_r != 0) {
        tool_syscall(SYS_UNLINK, (uintptr_t)path, 0, 0);
        ren_r = tool_syscall(SYS_RENAME, (uintptr_t)s_tmp_path, (uintptr_t)path, 0);
        if (ren_r != 0) {
            tool_error("sed", "cannot replace target file", path);
            return 2;
        }
    }

    return 0;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: sed [OPTIONS] [SCRIPT] [FILE ...]\n"
        "Stream editor for filtering and transforming text.\n\n"
        "Options:\n"
        "  -n, --quiet, --silent  Suppress default printing of pattern space\n"
        "  -e SCRIPT              Add SCRIPT to the commands to be executed\n"
        "  -i[SUFFIX]             Edit files in place (optionally save backup with SUFFIX)\n"
        "  --help                 Display this help text and exit\n\n"
        "Supported commands:\n"
        "  s/pat/repl/flags       Substitute match of pat with repl (flags: g, p, i, N)\n"
        "  p                      Print the current line\n"
        "  d                      Delete the current line\n\n"
        "Addressing:\n"
        "  N                      Match line number N (1-based)\n"
        "  N,M                    Match lines from N to M inclusive\n"
        "  /pattern/              Match lines matching regular expression\n"
        "  /pat1/,/pat2/          Match lines from pat1 to pat2 inclusive\n"
        "  $                      Match the last line\n"
        "  !                      Invert address matching (e.g. 2!d)\n";
    return tool_write("sed", help_text, sizeof(help_text) - 1);
}

/* --------------------------------------------------------------------------
 * Entry Point
 * -------------------------------------------------------------------------- */

int sed_main(int argc, char **argv) {
    s_num_commands = 0;
    s_has_last_regex = false;

    for (size_t i = 0; i < sizeof(s_opts); i++) ((char *)&s_opts)[i] = 0;

    const char *first_script = NULL;
    bool had_script_arg = false;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            return print_help();
        }
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] == '-' && arg[1]) {
            if (tool_equal(arg, "-n") || tool_equal(arg, "--quiet") || tool_equal(arg, "--silent")) {
                s_opts.quiet = true;
            } else if (tool_equal(arg, "-e")) {
                if (++i >= argc) {
                    tool_error("sed", "option requires an argument -- 'e'", NULL);
                    return 2;
                }
                int r = parse_script(argv[i]);
                if (r != 0) return r;
                had_script_arg = true;
            } else if (arg[1] == 'e' && arg[2]) {
                int r = parse_script(arg + 2);
                if (r != 0) return r;
                had_script_arg = true;
            } else if (arg[1] == 'i') {
                s_opts.in_place = true;
                if (arg[2] != '\0') {
                    s_opts.bak_suffix = arg + 2;
                } else if (i + 1 < argc && argv[i + 1][0] == '.') {
                    s_opts.bak_suffix = argv[++i];
                } else {
                    s_opts.bak_suffix = "";
                }
            } else if (arg[1] == 'n' && arg[2] == '\0') {
                s_opts.quiet = true;
            } else {
                tool_error("sed", "invalid option; use --help", NULL);
                return 2;
            }
        } else {
            break;
        }
    }

    if (!had_script_arg) {
        if (i >= argc) {
            tool_error("sed", "missing script; use --help", NULL);
            return 2;
        }
        first_script = argv[i++];
        int r = parse_script(first_script);
        if (r != 0) return r;
    }

    for (; i < argc; i++) {
        if (s_opts.num_files < SED_MAX_FILES) {
            s_opts.files[s_opts.num_files++] = argv[i];
        }
    }

    if (s_opts.in_place) {
        if (s_opts.num_files == 0) {
            tool_error("sed", "-i requires at least one file", NULL);
            return 2;
        }
        for (int f = 0; f < s_opts.num_files; f++) {
            int r = process_file_inplace(s_opts.files[f], &s_opts);
            if (r != 0) return r;
        }
        return 0;
    }

    if (s_opts.num_files == 0) {
        uint64_t line_no = 0;
        return process_single_stream(0, 1, &line_no, &s_opts);
    }

    /* Process multiple files concatenated into a single stream */
    uint64_t line_no = 0;
    for (int f = 0; f < s_opts.num_files; f++) {
        const char *fpath = s_opts.files[f];
        int fd = 0;
        if (!tool_equal(fpath, "-")) {
            long open_r = tool_syscall(SYS_OPEN, (uintptr_t)fpath, VFS_O_RDONLY, 0);
            if (open_r < 0) {
                tool_error("sed", "cannot open", fpath);
                return 2;
            }
            fd = (int)open_r;
        }

        int r = process_single_stream(fd, 1, &line_no, &s_opts);
        if (fd != 0) {
            tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        }
        if (r != 0) return r;
    }

    return 0;
}
