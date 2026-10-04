#include "common.h"

#define DIFF_MAX_LINES 2048
#define DIFF_POOL_SIZE 131072
#define DIFF_MAX_SNAKE 4096

typedef struct {
    uint32_t offset;
    uint16_t length;
} diff_line_t;

typedef struct {
    bool unified;
    uint32_t context_lines;
    bool brief;
    bool ignore_case;
    bool ignore_all_space;
    bool ignore_space_change;
    const char *file_a;
    const char *file_b;
} diff_options_t;

typedef struct {
    int x1, y1, x2, y2;
} diff_box_t;

static char s_pool_a[DIFF_POOL_SIZE];
static size_t s_pool_a_used = 0;
static diff_line_t s_lines_a[DIFF_MAX_LINES];
static size_t s_line_count_a = 0;
static int16_t s_matched_a[DIFF_MAX_LINES];

static char s_pool_b[DIFF_POOL_SIZE];
static size_t s_pool_b_used = 0;
static diff_line_t s_lines_b[DIFF_MAX_LINES];
static size_t s_line_count_b = 0;
static int16_t s_matched_b[DIFF_MAX_LINES];

static int s_vf[2 * DIFF_MAX_SNAKE + 1];
static int s_vb[2 * DIFF_MAX_SNAKE + 1];

static diff_box_t s_box_stack[64];
static int s_box_sp = 0;

static char s_out_buf[TOOL_BUFFER_SIZE];
static size_t s_out_pos = 0;

static int write_out(int out_fd, const void *data, size_t n) {
    if (out_fd == 1) return tool_write("diff", data, n);
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)p, n);
        if (w <= 0) return tool_error("diff", "write error", NULL);
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

static inline int emit_char(int out_fd, char c) {
    return emit_out(out_fd, &c, 1);
}

static inline int emit_str(int out_fd, const char *s) {
    return emit_out(out_fd, s, tool_length(s));
}

static int emit_u32(int out_fd, uint32_t val) {
    char num[32];
    size_t len = tool_format_u64(num, val);
    return emit_out(out_fd, num, len);
}

static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool lines_match(const char *a, size_t a_len, const char *b, size_t b_len, const diff_options_t *opts) {
    if (!opts->ignore_all_space && !opts->ignore_space_change && !opts->ignore_case) {
        if (a_len != b_len) return false;
        for (size_t i = 0; i < a_len; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    size_t i = 0, j = 0;
    while (i < a_len || j < b_len) {
        if (opts->ignore_all_space) {
            while (i < a_len && (a[i] == ' ' || a[i] == '\t')) i++;
            while (j < b_len && (b[j] == ' ' || b[j] == '\t')) j++;
            if (i >= a_len && j >= b_len) return true;
            if (i >= a_len || j >= b_len) return false;
            char ca = a[i++];
            char cb = b[j++];
            if (opts->ignore_case) {
                ca = to_lower(ca);
                cb = to_lower(cb);
            }
            if (ca != cb) return false;
            continue;
        }

        if (opts->ignore_space_change) {
            bool a_space = (i < a_len && (a[i] == ' ' || a[i] == '\t'));
            bool b_space = (j < b_len && (b[j] == ' ' || b[j] == '\t'));
            if (a_space || b_space) {
                if (!a_space || !b_space) return false;
                while (i < a_len && (a[i] == ' ' || a[i] == '\t')) i++;
                while (j < b_len && (b[j] == ' ' || b[j] == '\t')) j++;
                continue;
            }
            if (i >= a_len && j >= b_len) return true;
            if (i >= a_len || j >= b_len) return false;
            char ca = a[i++];
            char cb = b[j++];
            if (opts->ignore_case) {
                ca = to_lower(ca);
                cb = to_lower(cb);
            }
            if (ca != cb) return false;
            continue;
        }

        char ca = a[i++];
        char cb = b[j++];
        if (opts->ignore_case) {
            ca = to_lower(ca);
            cb = to_lower(cb);
        }
        if (ca != cb) return false;
    }
    return true;
}

static int read_file_lines(int fd, const char *label, char *pool, size_t *pool_used,
                           diff_line_t *lines, size_t *line_count) {
    size_t line_start = *pool_used;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("diff", "read error", label);
        if (n == 0) break;

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == '\n') {
                size_t len = *pool_used - line_start;
                if (len > 0 && pool[*pool_used - 1] == '\r') {
                    len--;
                    (*pool_used)--;
                }
                if (*line_count >= DIFF_MAX_LINES) {
                    return tool_error("diff", "maximum line count exceeded", label);
                }
                lines[*line_count].offset = (uint32_t)line_start;
                lines[*line_count].length = (uint16_t)len;
                (*line_count)++;
                line_start = *pool_used;
            } else {
                if (*pool_used >= DIFF_POOL_SIZE) {
                    return tool_error("diff", "file too large for memory buffer", label);
                }
                pool[(*pool_used)++] = c;
            }
        }
    }

    if (*pool_used > line_start) {
        size_t len = *pool_used - line_start;
        if (len > 0 && pool[*pool_used - 1] == '\r') {
            len--;
            (*pool_used)--;
        }
        if (*line_count >= DIFF_MAX_LINES) {
            return tool_error("diff", "maximum line count exceeded", label);
        }
        lines[*line_count].offset = (uint32_t)line_start;
        lines[*line_count].length = (uint16_t)len;
        (*line_count)++;
    }

    return 0;
}

static bool find_middle_snake(int x1, int y1, int x2, int y2, const diff_options_t *opts,
                              int *out_u, int *out_v, int *out_x, int *out_y) {
    int N = x2 - x1;
    int M = y2 - y1;
    int delta = N - M;
    bool odd = (delta & 1) != 0;
    int MAX = N + M;
    if (MAX > DIFF_MAX_SNAKE) MAX = DIFF_MAX_SNAKE;
    int offset = MAX;

    for (int i = 0; i <= 2 * MAX; i++) {
        s_vf[i] = -1;
        s_vb[i] = -1;
    }

    s_vf[offset + 1] = 0;
    s_vb[offset + delta - 1] = N;

    int max_d = (MAX + 1) / 2;
    for (int d = 0; d <= max_d; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && s_vf[offset + k - 1] < s_vf[offset + k + 1])) {
                x = s_vf[offset + k + 1];
            } else {
                x = s_vf[offset + k - 1] + 1;
            }
            int y = x - k;
            int x0 = x, y0 = y;
            while (x < N && y < M) {
                const char *la = s_pool_a + s_lines_a[x1 + x].offset;
                size_t la_len = s_lines_a[x1 + x].length;
                const char *lb = s_pool_b + s_lines_b[y1 + y].offset;
                size_t lb_len = s_lines_b[y1 + y].length;
                if (!lines_match(la, la_len, lb, lb_len, opts)) break;
                x++;
                y++;
            }
            s_vf[offset + k] = x;
            if (odd && (k >= delta - (d - 1) && k <= delta + (d - 1))) {
                if (s_vf[offset + k] >= s_vb[offset + k]) {
                    *out_u = x1 + x0;
                    *out_v = y1 + y0;
                    *out_x = x1 + x;
                    *out_y = y1 + y;
                    return true;
                }
            }
        }

        for (int c = -d + delta; c <= d + delta; c += 2) {
            int u;
            if (c == d + delta || (c != -d + delta && s_vb[offset + c - 1] < s_vb[offset + c + 1])) {
                u = s_vb[offset + c - 1];
            } else {
                u = s_vb[offset + c + 1] - 1;
            }
            int v = u - c;
            int u0 = u, v0 = v;
            while (u > 0 && v > 0) {
                const char *la = s_pool_a + s_lines_a[x1 + u - 1].offset;
                size_t la_len = s_lines_a[x1 + u - 1].length;
                const char *lb = s_pool_b + s_lines_b[y1 + v - 1].offset;
                size_t lb_len = s_lines_b[y1 + v - 1].length;
                if (!lines_match(la, la_len, lb, lb_len, opts)) break;
                u--;
                v--;
            }
            s_vb[offset + c] = u;
            if (!odd && (c >= -d && c <= d)) {
                if (s_vb[offset + c] <= s_vf[offset + c]) {
                    *out_u = x1 + u;
                    *out_v = y1 + v;
                    *out_x = x1 + u0;
                    *out_y = y1 + v0;
                    return true;
                }
            }
        }
    }

    return false;
}

static void compute_lcs(const diff_options_t *opts) {
    for (size_t i = 0; i < s_line_count_a; i++) s_matched_a[i] = -1;
    for (size_t j = 0; j < s_line_count_b; j++) s_matched_b[j] = -1;

    s_box_sp = 0;
    s_box_stack[s_box_sp++] = (diff_box_t){0, 0, (int)s_line_count_a, (int)s_line_count_b};

    while (s_box_sp > 0) {
        diff_box_t box = s_box_stack[--s_box_sp];
        int x1 = box.x1, y1 = box.y1, x2 = box.x2, y2 = box.y2;

        /* Common prefix */
        while (x1 < x2 && y1 < y2) {
            const char *la = s_pool_a + s_lines_a[x1].offset;
            size_t la_len = s_lines_a[x1].length;
            const char *lb = s_pool_b + s_lines_b[y1].offset;
            size_t lb_len = s_lines_b[y1].length;
            if (!lines_match(la, la_len, lb, lb_len, opts)) break;
            s_matched_a[x1] = (int16_t)y1;
            s_matched_b[y1] = (int16_t)x1;
            x1++;
            y1++;
        }

        /* Common suffix */
        while (x2 > x1 && y2 > y1) {
            const char *la = s_pool_a + s_lines_a[x2 - 1].offset;
            size_t la_len = s_lines_a[x2 - 1].length;
            const char *lb = s_pool_b + s_lines_b[y2 - 1].offset;
            size_t lb_len = s_lines_b[y2 - 1].length;
            if (!lines_match(la, la_len, lb, lb_len, opts)) break;
            s_matched_a[x2 - 1] = (int16_t)(y2 - 1);
            s_matched_b[y2 - 1] = (int16_t)(x2 - 1);
            x2--;
            y2--;
        }

        if (x1 >= x2 || y1 >= y2) continue;

        int u = 0, v = 0, x = 0, y = 0;
        if (!find_middle_snake(x1, y1, x2, y2, opts, &u, &v, &x, &y)) {
            continue;
        }

        int cx = u, cy = v;
        while (cx < x && cy < y) {
            s_matched_a[cx] = (int16_t)cy;
            s_matched_b[cy] = (int16_t)cx;
            cx++;
            cy++;
        }

        if (s_box_sp + 2 < (int)(sizeof(s_box_stack) / sizeof(s_box_stack[0]))) {
            s_box_stack[s_box_sp++] = (diff_box_t){x, y, x2, y2};
            s_box_stack[s_box_sp++] = (diff_box_t){x1, y1, u, v};
        }
    }
}

static bool files_differ(void) {
    if (s_line_count_a != s_line_count_b) return true;
    for (size_t i = 0; i < s_line_count_a; i++) {
        if (s_matched_a[i] != (int16_t)i) return true;
    }
    return false;
}

static int print_normal_diff(void) {
    size_t i = 0, j = 0;
    while (i < s_line_count_a || j < s_line_count_b) {
        if (i < s_line_count_a && s_matched_a[i] != -1 && s_matched_a[i] == (int16_t)j) {
            i++;
            j++;
            continue;
        }

        size_t a_start = i;
        while (i < s_line_count_a && s_matched_a[i] == -1) i++;
        size_t a_end = i;

        size_t b_start = j;
        while (j < s_line_count_b && s_matched_b[j] == -1) j++;
        size_t b_end = j;

        size_t del_count = a_end - a_start;
        size_t ins_count = b_end - b_start;

        if (del_count > 0 && ins_count > 0) {
            /* Change: a1,a2cb1,b2 */
            if (del_count == 1) {
                emit_u32(1, (uint32_t)(a_start + 1));
            } else {
                emit_u32(1, (uint32_t)(a_start + 1));
                emit_char(1, ',');
                emit_u32(1, (uint32_t)a_end);
            }
            emit_char(1, 'c');
            if (ins_count == 1) {
                emit_u32(1, (uint32_t)(b_start + 1));
            } else {
                emit_u32(1, (uint32_t)(b_start + 1));
                emit_char(1, ',');
                emit_u32(1, (uint32_t)b_end);
            }
            emit_char(1, '\n');

            for (size_t k = a_start; k < a_end; k++) {
                emit_str(1, "< ");
                emit_out(1, s_pool_a + s_lines_a[k].offset, s_lines_a[k].length);
                emit_char(1, '\n');
            }
            emit_str(1, "---\n");
            for (size_t k = b_start; k < b_end; k++) {
                emit_str(1, "> ");
                emit_out(1, s_pool_b + s_lines_b[k].offset, s_lines_b[k].length);
                emit_char(1, '\n');
            }
        } else if (del_count > 0) {
            /* Delete: a1,a2db1 */
            if (del_count == 1) {
                emit_u32(1, (uint32_t)(a_start + 1));
            } else {
                emit_u32(1, (uint32_t)(a_start + 1));
                emit_char(1, ',');
                emit_u32(1, (uint32_t)a_end);
            }
            emit_char(1, 'd');
            emit_u32(1, (uint32_t)b_start);
            emit_char(1, '\n');

            for (size_t k = a_start; k < a_end; k++) {
                emit_str(1, "< ");
                emit_out(1, s_pool_a + s_lines_a[k].offset, s_lines_a[k].length);
                emit_char(1, '\n');
            }
        } else if (ins_count > 0) {
            /* Add: a1ab1,b2 */
            emit_u32(1, (uint32_t)a_start);
            emit_char(1, 'a');
            if (ins_count == 1) {
                emit_u32(1, (uint32_t)(b_start + 1));
            } else {
                emit_u32(1, (uint32_t)(b_start + 1));
                emit_char(1, ',');
                emit_u32(1, (uint32_t)b_end);
            }
            emit_char(1, '\n');

            for (size_t k = b_start; k < b_end; k++) {
                emit_str(1, "> ");
                emit_out(1, s_pool_b + s_lines_b[k].offset, s_lines_b[k].length);
                emit_char(1, '\n');
            }
        }
    }
    return flush_out_buf(1);
}

static int print_unified_diff(const diff_options_t *opts) {
    emit_str(1, "--- ");
    emit_str(1, opts->file_a);
    emit_char(1, '\n');
    emit_str(1, "+++ ");
    emit_str(1, opts->file_b);
    emit_char(1, '\n');

    size_t ctx = opts->context_lines;
    size_t i = 0, j = 0;

    while (i < s_line_count_a || j < s_line_count_b) {
        if (i < s_line_count_a && s_matched_a[i] != -1 && s_matched_a[i] == (int16_t)j) {
            i++;
            j++;
            continue;
        }

        /* Found edit region */
        size_t a_start = i;
        while (i < s_line_count_a && s_matched_a[i] == -1) i++;
        size_t a_end = i;

        size_t b_start = j;
        while (j < s_line_count_b && s_matched_b[j] == -1) j++;
        size_t b_end = j;

        /* Lookahead and group close edits within 2 * ctx lines */
        while (i < s_line_count_a || j < s_line_count_b) {
            size_t next_a = i;
            size_t next_b = j;
            while (next_a < s_line_count_a && next_b < s_line_count_b &&
                   s_matched_a[next_a] != -1 && s_matched_a[next_a] == (int16_t)next_b) {
                next_a++;
                next_b++;
            }
            size_t common_between = next_a - i;
            if (common_between > 2 * ctx || (next_a >= s_line_count_a && next_b >= s_line_count_b)) {
                break;
            }
            /* Merge with next edit */
            i = next_a;
            j = next_b;
            while (i < s_line_count_a && s_matched_a[i] == -1) i++;
            while (j < s_line_count_b && s_matched_b[j] == -1) j++;
            a_end = i;
            b_end = j;
        }

        size_t hunk_a_start = a_start > ctx ? a_start - ctx : 0;
        size_t hunk_a_end = (a_end + ctx <= s_line_count_a) ? a_end + ctx : s_line_count_a;
        size_t hunk_b_start = b_start > ctx ? b_start - ctx : 0;
        size_t hunk_b_end = (b_end + ctx <= s_line_count_b) ? b_end + ctx : s_line_count_b;

        size_t hunk_a_count = hunk_a_end - hunk_a_start;
        size_t hunk_b_count = hunk_b_end - hunk_b_start;

        emit_str(1, "@@ -");
        emit_u32(1, (uint32_t)(hunk_a_count == 0 ? 0 : hunk_a_start + 1));
        if (hunk_a_count != 1) {
            emit_char(1, ',');
            emit_u32(1, (uint32_t)hunk_a_count);
        }
        emit_str(1, " +");
        emit_u32(1, (uint32_t)(hunk_b_count == 0 ? 0 : hunk_b_start + 1));
        if (hunk_b_count != 1) {
            emit_char(1, ',');
            emit_u32(1, (uint32_t)hunk_b_count);
        }
        emit_str(1, " @@\n");

        size_t cur_a = hunk_a_start;
        size_t cur_b = hunk_b_start;

        while (cur_a < hunk_a_end || cur_b < hunk_b_end) {
            if (cur_a < hunk_a_end && cur_b < hunk_b_end &&
                s_matched_a[cur_a] != -1 && s_matched_a[cur_a] == (int16_t)cur_b) {
                emit_char(1, ' ');
                emit_out(1, s_pool_a + s_lines_a[cur_a].offset, s_lines_a[cur_a].length);
                emit_char(1, '\n');
                cur_a++;
                cur_b++;
            } else if (cur_a < hunk_a_end && s_matched_a[cur_a] == -1) {
                emit_char(1, '-');
                emit_out(1, s_pool_a + s_lines_a[cur_a].offset, s_lines_a[cur_a].length);
                emit_char(1, '\n');
                cur_a++;
            } else if (cur_b < hunk_b_end && s_matched_b[cur_b] == -1) {
                emit_char(1, '+');
                emit_out(1, s_pool_b + s_lines_b[cur_b].offset, s_lines_b[cur_b].length);
                emit_char(1, '\n');
                cur_b++;
            } else {
                /* Common line outside current sub-range */
                emit_char(1, ' ');
                emit_out(1, s_pool_a + s_lines_a[cur_a].offset, s_lines_a[cur_a].length);
                emit_char(1, '\n');
                cur_a++;
                cur_b++;
            }
        }
    }
    return flush_out_buf(1);
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: diff [OPTIONS] FILE1 FILE2\n"
        "Compare FILE1 and FILE2 line by line.\n\n"
        "Options:\n"
        "  -u, -U NUM      Output NUM (default 3) lines of unified context\n"
        "  -q, --brief     Report only whether files differ\n"
        "  -i, --ignore-case  Ignore case differences in line contents\n"
        "  -w, --ignore-all-space  Ignore all white space\n"
        "  -b, --ignore-space-change  Ignore changes in the amount of white space\n"
        "  --help          Display this help text and exit\n";
    return tool_write("diff", help_text, sizeof(help_text) - 1);
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

static int parse_diff_options(int argc, char **argv, diff_options_t *opts) {
    for (size_t i = 0; i < sizeof(*opts); i++) ((char *)opts)[i] = 0;
    opts->context_lines = 3;

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

        if (tool_equal(arg, "-u") || tool_equal(arg, "--unified")) {
            opts->unified = true;
            continue;
        }
        if (tool_equal(arg, "-q") || tool_equal(arg, "--brief")) {
            opts->brief = true;
            continue;
        }
        if (tool_equal(arg, "-i") || tool_equal(arg, "--ignore-case")) {
            opts->ignore_case = true;
            continue;
        }
        if (tool_equal(arg, "-w") || tool_equal(arg, "--ignore-all-space")) {
            opts->ignore_all_space = true;
            continue;
        }
        if (tool_equal(arg, "-b") || tool_equal(arg, "--ignore-space-change")) {
            opts->ignore_space_change = true;
            continue;
        }

        for (size_t j = 1; arg[j]; j++) {
            char opt = arg[j];
            if (opt == 'u') {
                opts->unified = true;
            } else if (opt == 'q') {
                opts->brief = true;
            } else if (opt == 'i') {
                opts->ignore_case = true;
            } else if (opt == 'w') {
                opts->ignore_all_space = true;
            } else if (opt == 'b') {
                opts->ignore_space_change = true;
            } else if (opt == 'U') {
                opts->unified = true;
                const char *val_str = NULL;
                if (arg[j + 1] != '\0') {
                    val_str = &arg[j + 1];
                } else if (i + 1 < argc) {
                    val_str = argv[++i];
                } else {
                    tool_error("diff", "option requires an argument -- 'U'", NULL);
                    return 2;
                }
                uint32_t num = 0;
                if (!decimal_u32(val_str, &num)) {
                    tool_error("diff", "invalid context lines for -U", val_str);
                    return 2;
                }
                opts->context_lines = num;
                break;
            } else {
                tool_error("diff", "invalid option; use --help", NULL);
                return 2;
            }
        }
    }

    if (i < argc) {
        opts->file_a = argv[i++];
    }
    if (i < argc) {
        opts->file_b = argv[i++];
    }

    if (!opts->file_a || !opts->file_b) {
        tool_error("diff", "missing operand; use --help", NULL);
        return 2;
    }
    if (i < argc) {
        tool_error("diff", "extra operand; use --help", argv[i]);
        return 2;
    }

    return 0;
}

int diff_main(int argc, char **argv) {
    diff_options_t opts;
    int opt_res = parse_diff_options(argc, argv, &opts);
    if (opt_res == 1) return print_help();
    if (opt_res != 0) return opt_res;

    s_pool_a_used = 0;
    s_line_count_a = 0;
    s_pool_b_used = 0;
    s_line_count_b = 0;
    s_out_pos = 0;

    /* Read File A */
    int in_a_fd = 0;
    bool close_a = false;
    if (!tool_equal(opts.file_a, "-")) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)opts.file_a, (uintptr_t)&st, 0) < 0) {
            tool_error("diff", "cannot open", opts.file_a);
            return 2;
        }
        if (st.type == VFS_DIRECTORY) {
            tool_error("diff", "is a directory", opts.file_a);
            return 2;
        }
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.file_a, VFS_O_RDONLY, 0);
        if (fd < 0) {
            tool_error("diff", "cannot open", opts.file_a);
            return 2;
        }
        in_a_fd = (int)fd;
        close_a = true;
    }
    int ra = read_file_lines(in_a_fd, opts.file_a, s_pool_a, &s_pool_a_used, s_lines_a, &s_line_count_a);
    if (close_a) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_a_fd, 0, 0);
    if (ra != 0) return 2;

    /* Read File B */
    int in_b_fd = 0;
    bool close_b = false;
    if (!tool_equal(opts.file_b, "-")) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)opts.file_b, (uintptr_t)&st, 0) < 0) {
            tool_error("diff", "cannot open", opts.file_b);
            return 2;
        }
        if (st.type == VFS_DIRECTORY) {
            tool_error("diff", "is a directory", opts.file_b);
            return 2;
        }
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.file_b, VFS_O_RDONLY, 0);
        if (fd < 0) {
            tool_error("diff", "cannot open", opts.file_b);
            return 2;
        }
        in_b_fd = (int)fd;
        close_b = true;
    }
    int rb = read_file_lines(in_b_fd, opts.file_b, s_pool_b, &s_pool_b_used, s_lines_b, &s_line_count_b);
    if (close_b) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_b_fd, 0, 0);
    if (rb != 0) return 2;

    compute_lcs(&opts);

    if (!files_differ()) {
        return 0;
    }

    if (opts.brief) {
        emit_str(1, "Files ");
        emit_str(1, opts.file_a);
        emit_str(1, " and ");
        emit_str(1, opts.file_b);
        emit_str(1, " differ\n");
        flush_out_buf(1);
        return 1;
    }

    if (opts.unified) {
        print_unified_diff(&opts);
    } else {
        print_normal_diff();
    }

    return 1;
}
