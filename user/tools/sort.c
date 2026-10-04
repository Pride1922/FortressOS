#include "common.h"

#define SORT_POOL_SIZE 262144  /* 256 KiB pool for line text */
#define SORT_MAX_LINES 8192    /* Up to 8192 lines */

typedef struct {
    uint32_t offset;
    uint32_t length;
} sort_line_t;

typedef struct {
    bool reverse;       /* -r */
    bool numeric;       /* -n */
    bool unique;        /* -u */
    bool ignore_case;   /* -f */
    bool check;         /* -c or -C */
    bool check_silent;  /* -C */
    uint32_t key_field; /* -k */
    const char *output_file; /* -o */
    int input_start;    /* argv index of first input file */
} sort_options_t;

static char s_text_pool[SORT_POOL_SIZE];
static size_t s_pool_used = 0;

static sort_line_t s_lines[SORT_MAX_LINES];
static sort_line_t s_aux_lines[SORT_MAX_LINES];
static size_t s_line_count = 0;

static char s_out_buf[TOOL_BUFFER_SIZE];
static size_t s_out_pos = 0;

static int write_out(int out_fd, const void *data, size_t n) {
    if (out_fd == 1) return tool_write("sort", data, n);
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)p, n);
        if (w <= 0) return tool_error("sort", "write error", NULL);
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int flush_out_buf(int out_fd) {
    if (s_out_pos == 0) return 0;
    int r = write_out(out_fd, s_out_buf, s_out_pos);
    s_out_pos = 0;
    return r;
}

static int emit_out(int out_fd, const char *data, size_t n) {
    while (n > 0) {
        size_t space = sizeof(s_out_buf) - s_out_pos;
        if (space == 0) {
            int r = flush_out_buf(out_fd);
            if (r) return r;
            space = sizeof(s_out_buf);
        }
        size_t take = n < space ? n : space;
        for (size_t i = 0; i < take; i++) {
            s_out_buf[s_out_pos++] = data[i];
        }
        data += take;
        n -= take;
    }
    return 0;
}

static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static const char *get_field(const char *line, size_t len, uint32_t key_field, size_t *out_len) {
    if (key_field <= 1) {
        *out_len = len;
        return line;
    }

    size_t i = 0;
    uint32_t current_field = 0;
    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len) break;

        current_field++;
        size_t field_start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t') i++;

        if (current_field == key_field) {
            *out_len = len - field_start;
            return line + field_start;
        }
    }

    *out_len = 0;
    return line + len;
}

static int parse_number(const char *str, size_t len, int64_t *out) {
    size_t i = 0;
    while (i < len && (str[i] == ' ' || str[i] == '\t')) i++;
    if (i >= len) {
        *out = 0;
        return 0;
    }

    bool neg = false;
    if (str[i] == '-') {
        neg = true;
        i++;
    } else if (str[i] == '+') {
        i++;
    }

    int64_t val = 0;
    bool has_digits = false;
    while (i < len && str[i] >= '0' && str[i] <= '9') {
        has_digits = true;
        int d = str[i++] - '0';
        if (val > (INT64_MAX - d) / 10) {
            val = INT64_MAX;
            break;
        }
        val = val * 10 + d;
    }

    if (!has_digits) {
        *out = 0;
        return 0;
    }

    *out = neg ? -val : val;
    return 1;
}

static int compare_lines(const sort_line_t *la, const sort_line_t *lb, const sort_options_t *opts, bool key_only) {
    const char *a_full = s_text_pool + la->offset;
    size_t a_full_len = la->length;
    const char *b_full = s_text_pool + lb->offset;
    size_t b_full_len = lb->length;

    const char *a_key = a_full;
    size_t a_key_len = a_full_len;
    const char *b_key = b_full;
    size_t b_key_len = b_full_len;

    if (opts->key_field > 0) {
        a_key = get_field(a_full, a_full_len, opts->key_field, &a_key_len);
        b_key = get_field(b_full, b_full_len, opts->key_field, &b_key_len);
    }

    int cmp = 0;
    if (opts->numeric) {
        int64_t num_a = 0, num_b = 0;
        parse_number(a_key, a_key_len, &num_a);
        parse_number(b_key, b_key_len, &num_b);
        if (num_a < num_b) cmp = -1;
        else if (num_a > num_b) cmp = 1;
        else cmp = 0;
    } else {
        size_t min_len = a_key_len < b_key_len ? a_key_len : b_key_len;
        for (size_t i = 0; i < min_len; i++) {
            unsigned char ca = (unsigned char)a_key[i];
            unsigned char cb = (unsigned char)b_key[i];
            if (opts->ignore_case) {
                ca = (unsigned char)to_lower((char)ca);
                cb = (unsigned char)to_lower((char)cb);
            }
            if (ca < cb) { cmp = -1; break; }
            if (ca > cb) { cmp = 1; break; }
        }
        if (cmp == 0) {
            if (a_key_len < b_key_len) cmp = -1;
            else if (a_key_len > b_key_len) cmp = 1;
        }
    }

    if (cmp == 0 && !key_only && (opts->numeric || opts->key_field > 0)) {
        size_t min_len = a_full_len < b_full_len ? a_full_len : b_full_len;
        for (size_t i = 0; i < min_len; i++) {
            unsigned char ca = (unsigned char)a_full[i];
            unsigned char cb = (unsigned char)b_full[i];
            if (opts->ignore_case) {
                ca = (unsigned char)to_lower((char)ca);
                cb = (unsigned char)to_lower((char)cb);
            }
            if (ca < cb) { cmp = -1; break; }
            if (ca > cb) { cmp = 1; break; }
        }
        if (cmp == 0) {
            if (a_full_len < b_full_len) cmp = -1;
            else if (a_full_len > b_full_len) cmp = 1;
        }
    }

    return cmp;
}

static void merge(sort_line_t *arr, sort_line_t *aux, size_t left, size_t mid, size_t right, const sort_options_t *opts) {
    size_t i = left;
    size_t j = mid;
    size_t k = left;

    while (i < mid && j < right) {
        int cmp = compare_lines(&arr[i], &arr[j], opts, opts->unique);
        if (opts->reverse) {
            if (cmp >= 0) {
                aux[k++] = arr[i++];
            } else {
                aux[k++] = arr[j++];
            }
        } else {
            if (cmp <= 0) {
                aux[k++] = arr[i++];
            } else {
                aux[k++] = arr[j++];
            }
        }
    }

    while (i < mid) aux[k++] = arr[i++];
    while (j < right) aux[k++] = arr[j++];

    for (size_t x = left; x < right; x++) {
        arr[x] = aux[x];
    }
}

static void bottom_up_mergesort(sort_line_t *arr, sort_line_t *aux, size_t n, const sort_options_t *opts) {
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t i = 0; i < n; i += 2 * width) {
            size_t left = i;
            size_t mid = i + width < n ? i + width : n;
            size_t right = i + 2 * width < n ? i + 2 * width : n;
            if (mid < right) {
                merge(arr, aux, left, mid, right, opts);
            }
        }
    }
}

static bool decimal_u32(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t n = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        unsigned d = (unsigned)(*s++ - '0');
        if (n > (UINT32_MAX - d) / 10) return false;
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: sort [OPTIONS] [FILE...]\n"
        "Sort lines of text files.\n\n"
        "Options:\n"
        "  -r, --reverse       Reverse the result of comparisons\n"
        "  -n, --numeric-sort  Compare according to string numerical value\n"
        "  -u, --unique        Output only the first of an equal run\n"
        "  -f, --ignore-case   Fold lower case to upper case characters\n"
        "  -c, --check         Check for sorted order and report disorder; do not sort\n"
        "  -C                  Check for sorted order silently; do not sort\n"
        "  -k POS              Sort by key field starting at POS (1-indexed)\n"
        "  -o FILE             Write result to FILE instead of standard output\n"
        "  --help              Display this help text and exit\n";
    return tool_write("sort", help_text, sizeof(help_text) - 1);
}

static int parse_sort_options(int argc, char **argv, sort_options_t *opts) {
    for (size_t i = 0; i < sizeof(*opts); i++) ((char *)opts)[i] = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) return 1;
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] != '-' || !arg[1] || tool_equal(arg, "-")) {
            break;
        }

        if (tool_equal(arg, "--reverse")) { opts->reverse = true; continue; }
        if (tool_equal(arg, "--numeric-sort")) { opts->numeric = true; continue; }
        if (tool_equal(arg, "--unique")) { opts->unique = true; continue; }
        if (tool_equal(arg, "--ignore-case")) { opts->ignore_case = true; continue; }
        if (tool_equal(arg, "--check")) { opts->check = true; continue; }

        for (size_t j = 1; arg[j]; j++) {
            char opt = arg[j];
            if (opt == 'r') {
                opts->reverse = true;
            } else if (opt == 'n') {
                opts->numeric = true;
            } else if (opt == 'u') {
                opts->unique = true;
            } else if (opt == 'f') {
                opts->ignore_case = true;
            } else if (opt == 'c') {
                opts->check = true;
                opts->check_silent = false;
            } else if (opt == 'C') {
                opts->check = true;
                opts->check_silent = true;
            } else if (opt == 'k') {
                const char *val_str = NULL;
                if (arg[j + 1] != '\0') {
                    val_str = &arg[j + 1];
                } else if (i + 1 < argc) {
                    val_str = argv[++i];
                } else {
                    tool_error("sort", "option requires an argument -- 'k'", NULL);
                    return 2;
                }
                uint32_t f = 0;
                if (!decimal_u32(val_str, &f) || f == 0) {
                    tool_error("sort", "invalid key field; use --help", val_str);
                    return 2;
                }
                opts->key_field = f;
                break;
            } else if (opt == 'o') {
                if (arg[j + 1] != '\0') {
                    opts->output_file = &arg[j + 1];
                } else if (i + 1 < argc) {
                    opts->output_file = argv[++i];
                } else {
                    tool_error("sort", "option requires an argument -- 'o'", NULL);
                    return 2;
                }
                break;
            } else {
                tool_error("sort", "invalid option; use --help", NULL);
                return 2;
            }
        }
    }

    opts->input_start = i;
    return 0;
}

static int read_input_stream(int fd, const char *label) {
    size_t line_start = s_pool_used;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("sort", "read error", label);
        if (n == 0) break;

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == '\n') {
                size_t len = s_pool_used - line_start;
                if (len > 0 && s_text_pool[s_pool_used - 1] == '\r') {
                    len--;
                    s_pool_used--;
                }
                if (s_line_count >= SORT_MAX_LINES) {
                    return tool_error("sort", "maximum line count exceeded", label);
                }
                s_lines[s_line_count].offset = (uint32_t)line_start;
                s_lines[s_line_count].length = (uint32_t)len;
                s_line_count++;
                line_start = s_pool_used;
            } else {
                if (s_pool_used >= SORT_POOL_SIZE) {
                    return tool_error("sort", "file too large for memory buffer", label);
                }
                s_text_pool[s_pool_used++] = c;
            }
        }
    }

    if (s_pool_used > line_start) {
        size_t len = s_pool_used - line_start;
        if (len > 0 && s_text_pool[s_pool_used - 1] == '\r') {
            len--;
            s_pool_used--;
        }
        if (s_line_count >= SORT_MAX_LINES) {
            return tool_error("sort", "maximum line count exceeded", label);
        }
        s_lines[s_line_count].offset = (uint32_t)line_start;
        s_lines[s_line_count].length = (uint32_t)len;
        s_line_count++;
    }

    return 0;
}

int sort_main(int argc, char **argv) {
    sort_options_t opts;
    int opt_res = parse_sort_options(argc, argv, &opts);
    if (opt_res == 1) return print_help();
    if (opt_res != 0) return opt_res;

    s_pool_used = 0;
    s_line_count = 0;
    s_out_pos = 0;

    if (opts.input_start >= argc) {
        int r = read_input_stream(0, "-");
        if (r) return r;
    } else {
        for (int i = opts.input_start; i < argc; i++) {
            const char *file = argv[i];
            if (tool_equal(file, "-")) {
                int r = read_input_stream(0, "-");
                if (r) return r;
            } else {
                vfs_stat_t st;
                if (tool_syscall(SYS_STAT, (uintptr_t)file, (uintptr_t)&st, 0) < 0) {
                    return tool_error("sort", "cannot open", file);
                }
                if (st.type == VFS_DIRECTORY) {
                    return tool_error("sort", "is a directory", file);
                }
                long fd = tool_syscall(SYS_OPEN, (uintptr_t)file, VFS_O_RDONLY, 0);
                if (fd < 0) {
                    return tool_error("sort", "cannot open", file);
                }
                int r = read_input_stream((int)fd, file);
                (void)tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                if (r) return r;
            }
        }
    }

    if (opts.check) {
        for (size_t i = 1; i < s_line_count; i++) {
            int cmp = compare_lines(&s_lines[i - 1], &s_lines[i], &opts, false);
            bool disorder = false;
            if (opts.reverse) {
                if (cmp < 0) disorder = true;
                else if (cmp == 0 && opts.unique) disorder = true;
            } else {
                if (cmp > 0) disorder = true;
                else if (cmp == 0 && opts.unique) disorder = true;
            }
            if (disorder) {
                if (opts.check_silent) return 1;
                char line_num_str[32];
                tool_format_u64(line_num_str, (uint64_t)(i + 1));
                char diag[64] = "disorder on line ";
                size_t dpos = 17;
                for (size_t k = 0; line_num_str[k]; k++) diag[dpos++] = line_num_str[k];
                diag[dpos] = '\0';
                return tool_error("sort", diag, NULL);
            }
        }
        return 0;
    }

    if (s_line_count > 1) {
        bottom_up_mergesort(s_lines, s_aux_lines, s_line_count, &opts);
    }

    int out_fd = 1;
    bool close_out = false;
    if (opts.output_file && !tool_equal(opts.output_file, "-")) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.output_file,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
        if (fd < 0) {
            return tool_error("sort", "cannot create", opts.output_file);
        }
        out_fd = (int)fd;
        close_out = true;
    }

    const sort_line_t *prev = NULL;
    for (size_t i = 0; i < s_line_count; i++) {
        const sort_line_t *curr = &s_lines[i];
        if (opts.unique && prev != NULL) {
            int cmp = compare_lines(prev, curr, &opts, true);
            if (cmp == 0) continue;
        }

        int r = emit_out(out_fd, s_text_pool + curr->offset, curr->length);
        if (r) {
            if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);
            return r;
        }
        r = emit_out(out_fd, "\n", 1);
        if (r) {
            if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);
            return r;
        }
        prev = curr;
    }

    int ret = flush_out_buf(out_fd);
    if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);
    return ret;
}
