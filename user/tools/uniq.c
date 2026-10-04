#include "common.h"

#define UNIQ_LINE_CAP 4096

typedef struct {
    bool count;        /* -c */
    bool duplicates;   /* -d */
    bool unique_only;  /* -u */
    bool ignore_case;  /* -i */
    const char *input_file;
    const char *output_file;
} uniq_options_t;

static char s_prev_line[UNIQ_LINE_CAP];
static size_t s_prev_len = 0;
static char s_curr_line[UNIQ_LINE_CAP];
static size_t s_curr_len = 0;
static char s_cnt_buf[32];
static uint64_t s_repeat_count = 0;
static bool s_has_prev = false;

static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool lines_equal(const char *a, size_t a_len, const char *b, size_t b_len, bool icase) {
    if (a_len != b_len) return false;
    for (size_t i = 0; i < a_len; i++) {
        char c1 = a[i];
        char c2 = b[i];
        if (icase) {
            c1 = to_lower(c1);
            c2 = to_lower(c2);
        }
        if (c1 != c2) return false;
    }
    return true;
}

static int write_out(int out_fd, const void *data, size_t n) {
    if (out_fd == 1) return tool_write("uniq", data, n);
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)p, n);
        if (w <= 0) return tool_error("uniq", "write error", NULL);
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static size_t format_count(char *buf, uint64_t count) {
    char num[32];
    size_t npos = 0;
    do {
        num[npos++] = (char)('0' + (count % 10));
        count /= 10;
    } while (count && npos < sizeof(num));

    size_t width = npos < 7 ? 7 : npos;
    size_t spaces = width - npos;
    size_t bpos = 0;
    for (size_t i = 0; i < spaces; i++) buf[bpos++] = ' ';
    while (npos > 0) buf[bpos++] = num[--npos];
    buf[bpos++] = ' ';
    buf[bpos] = '\0';
    return bpos;
}

static int flush_run(int out_fd, const uniq_options_t *opts) {
    if (!s_has_prev) return 0;
    if (opts->duplicates && s_repeat_count < 2) return 0;
    if (opts->unique_only && s_repeat_count > 1) return 0;

    if (opts->count) {
        size_t clen = format_count(s_cnt_buf, s_repeat_count);
        int r = write_out(out_fd, s_cnt_buf, clen);
        if (r) return r;
    }
    int r = write_out(out_fd, s_prev_line, s_prev_len);
    if (!r) r = write_out(out_fd, "\n", 1);
    return r;
}

static int parse_uniq_options(int argc, char **argv, uniq_options_t *opts) {
    for (size_t i = 0; i < sizeof(*opts); i++) ((char *)opts)[i] = 0;

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
            case 'c': opts->count = true; break;
            case 'd': opts->duplicates = true; break;
            case 'u': opts->unique_only = true; break;
            case 'i': opts->ignore_case = true; break;
            default:
                tool_error("uniq", "invalid option; use --help", NULL);
                return 2;
            }
        }
    }

    if (i < argc) {
        opts->input_file = argv[i++];
    }
    if (i < argc) {
        opts->output_file = argv[i++];
    }
    if (i < argc) {
        tool_error("uniq", "extra operand; use --help", argv[i]);
        return 2;
    }
    return 0;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: uniq [OPTIONS] [INPUT [OUTPUT]]\n"
        "Filter adjacent matching lines from INPUT (or standard input) to OUTPUT (or standard output).\n\n"
        "Options:\n"
        "  -c        Prefix lines by the number of occurrences\n"
        "  -d        Only print duplicate lines\n"
        "  -u        Only print unique lines\n"
        "  -i        Ignore differences in case when comparing\n"
        "  --help    Display this help text and exit\n";
    return tool_write("uniq", help_text, sizeof(help_text) - 1);
}

static int process_uniq_stream(int in_fd, int out_fd, const uniq_options_t *opts) {
    s_prev_len = 0;
    s_curr_len = 0;
    s_repeat_count = 0;
    s_has_prev = false;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) {
            return tool_error("uniq", "read error", opts->input_file);
        }
        if (n == 0) {
            /* EOF reached */
            if (s_curr_len > 0) {
                if (s_has_prev) {
                    if (lines_equal(s_curr_line, s_curr_len, s_prev_line, s_prev_len, opts->ignore_case)) {
                        s_repeat_count++;
                    } else {
                        int r = flush_run(out_fd, opts);
                        if (r) return r;
                        for (size_t k = 0; k < s_curr_len; k++) s_prev_line[k] = s_curr_line[k];
                        s_prev_len = s_curr_len;
                        s_repeat_count = 1;
                    }
                } else {
                    for (size_t k = 0; k < s_curr_len; k++) s_prev_line[k] = s_curr_line[k];
                    s_prev_len = s_curr_len;
                    s_repeat_count = 1;
                    s_has_prev = true;
                }
                s_curr_len = 0;
            }
            int r = flush_run(out_fd, opts);
            if (r) return r;
            break;
        }

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == '\n') {
                if (s_curr_len > 0 && s_curr_line[s_curr_len - 1] == '\r') {
                    s_curr_len--;
                }
                if (s_has_prev) {
                    if (lines_equal(s_curr_line, s_curr_len, s_prev_line, s_prev_len, opts->ignore_case)) {
                        s_repeat_count++;
                    } else {
                        int r = flush_run(out_fd, opts);
                        if (r) return r;
                        for (size_t k = 0; k < s_curr_len; k++) s_prev_line[k] = s_curr_line[k];
                        s_prev_len = s_curr_len;
                        s_repeat_count = 1;
                    }
                } else {
                    for (size_t k = 0; k < s_curr_len; k++) s_prev_line[k] = s_curr_line[k];
                    s_prev_len = s_curr_len;
                    s_repeat_count = 1;
                    s_has_prev = true;
                }
                s_curr_len = 0;
            } else {
                if (s_curr_len + 1 < sizeof(s_curr_line)) {
                    s_curr_line[s_curr_len++] = c;
                }
            }
        }
    }
    return 0;
}

int uniq_main(int argc, char **argv) {
    uniq_options_t opts;
    int opt_res = parse_uniq_options(argc, argv, &opts);
    if (opt_res == 1) return print_help();
    if (opt_res != 0) return opt_res;

    int in_fd = 0;
    bool close_in = false;
    if (opts.input_file && !tool_equal(opts.input_file, "-")) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)opts.input_file, (uintptr_t)&st, 0) < 0) {
            return tool_error("uniq", "cannot open", opts.input_file);
        }
        if (st.type == VFS_DIRECTORY) {
            return tool_error("uniq", "is a directory", opts.input_file);
        }
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.input_file, VFS_O_RDONLY, 0);
        if (fd < 0) {
            return tool_error("uniq", "cannot open", opts.input_file);
        }
        in_fd = (int)fd;
        close_in = true;
    }

    int out_fd = 1;
    bool close_out = false;
    if (opts.output_file && !tool_equal(opts.output_file, "-")) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.output_file,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
        if (fd < 0) {
            if (close_in) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
            return tool_error("uniq", "cannot create", opts.output_file);
        }
        out_fd = (int)fd;
        close_out = true;
    }

    int ret = process_uniq_stream(in_fd, out_fd, &opts);

    if (close_in) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
    if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);

    return ret;
}
