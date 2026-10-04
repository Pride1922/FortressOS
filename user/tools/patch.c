#include "common.h"

#define PATCH_MAX_ORIG_LINES 4096
#define PATCH_ORIG_POOL_SIZE 262144   /* 256 KiB pool for original file */
#define PATCH_MAX_DIFF_LINES 8192
#define PATCH_DIFF_POOL_SIZE 262144   /* 256 KiB pool for diff file */
#define PATCH_MAX_OPS        8192
#define PATCH_MAX_HUNKS      512
#define PATCH_MAX_PATH       256

typedef struct {
    uint32_t offset;
    uint16_t length;
    bool has_newline;
} patch_line_t;

typedef enum {
    OP_CONTEXT,
    OP_DELETE,
    OP_ADD
} hunk_op_type_t;

typedef struct {
    hunk_op_type_t type;
    uint32_t text_offset;  /* Offset in diff pool of line text */
    uint16_t text_length;  /* Length of line text */
    bool has_newline;
} hunk_op_t;

typedef enum {
    HUNK_UNIFIED,
    HUNK_NORMAL_CHANGE,
    HUNK_NORMAL_APPEND,
    HUNK_NORMAL_DELETE
} hunk_type_t;

typedef struct {
    uint32_t target_line;       /* 1-based original line number */
    uint32_t op_start;          /* Index into s_ops */
    uint32_t op_count;          /* Number of operations */
    int32_t applied_at_line;    /* 1-based line where hunk actually matched */
    int32_t offset;             /* applied_at_line - target_line */
    bool applied;
} patch_hunk_t;

typedef struct {
    int strip_count;          /* -p NUM (default -1 means auto/unspecified) */
    const char *input_file;   /* -i PATCHFILE */
    const char *output_file;  /* -o OUTFILE */
    bool reverse;             /* -R */
    bool unified;             /* -u */
    bool quiet;               /* -s / -q */
    bool dry_run;             /* --dry-run */
    const char *target_file;  /* positional operand 1 */
    const char *patch_file;   /* positional operand 2 */
} patch_options_t;

/* Static BSS storage — bounded, zero dynamic allocation */
static char s_orig_pool[PATCH_ORIG_POOL_SIZE];
static patch_line_t s_orig_lines[PATCH_MAX_ORIG_LINES];
static size_t s_orig_pool_used = 0;
static size_t s_orig_line_count = 0;

static char s_diff_pool[PATCH_DIFF_POOL_SIZE];
static patch_line_t s_diff_lines[PATCH_MAX_DIFF_LINES];
static size_t s_diff_pool_used = 0;
static size_t s_diff_line_count = 0;

static hunk_op_t s_ops[PATCH_MAX_OPS];
static size_t s_op_count = 0;

static patch_hunk_t s_hunks[PATCH_MAX_HUNKS];
static size_t s_hunk_count = 0;

static char s_out_buf[TOOL_BUFFER_SIZE];
static size_t s_out_pos = 0;

static char s_detected_target[PATCH_MAX_PATH];
static char s_detected_old[PATCH_MAX_PATH];
static char s_detected_new[PATCH_MAX_PATH];

static inline int tool_memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static inline void *tool_memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

static inline void *tool_memset(void *dest, int c, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dest;
}

static int write_out(int out_fd, const void *data, size_t n) {
    if (out_fd == 1) return tool_write("patch", data, n);
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)p, n);
        if (w <= 0) return tool_error("patch", "write error", NULL);
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

static void print_msg(const char *msg) {
    tool_write("patch", msg, tool_length(msg));
}

static void print_hunk_success(uint32_t hunk_num, uint32_t line, int32_t offset) {
    char num_buf[32];
    print_msg("Hunk #");
    tool_format_u64(num_buf, hunk_num);
    print_msg(num_buf);
    print_msg(" succeeded at ");
    tool_format_u64(num_buf, line);
    print_msg(num_buf);
    if (offset != 0) {
        print_msg(" (offset ");
        tool_format_i64(num_buf, offset);
        print_msg(num_buf);
        print_msg(" lines)");
    }
    print_msg(".\n");
}

static void print_hunk_failed(uint32_t hunk_num, uint32_t line) {
    char num_buf[32];
    print_msg("Hunk #");
    tool_format_u64(num_buf, hunk_num);
    print_msg(num_buf);
    print_msg(" FAILED at line ");
    tool_format_u64(num_buf, line);
    print_msg(num_buf);
    print_msg(".\n");
}

static bool parse_u32(const char *s, size_t *pos, size_t len, uint32_t *out) {
    if (*pos >= len || s[*pos] < '0' || s[*pos] > '9') return false;
    uint32_t val = 0;
    while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
        int d = s[*pos] - '0';
        if (val > (UINT32_MAX - (uint32_t)d) / 10) return false;
        val = val * 10 + (uint32_t)d;
        (*pos)++;
    }
    *out = val;
    return true;
}

static bool decimal_u32_str(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    size_t pos = 0, len = tool_length(s);
    if (!parse_u32(s, &pos, len, out)) return false;
    return pos == len;
}

static bool parse_unified_header(const char *s, size_t len,
                                 uint32_t *old_start, uint32_t *old_count,
                                 uint32_t *new_start, uint32_t *new_count) {
    if (len < 7 || s[0] != '@' || s[1] != '@' || s[2] != ' ' || s[3] != '-') return false;
    size_t pos = 4;
    if (!parse_u32(s, &pos, len, old_start)) return false;
    if (pos < len && s[pos] == ',') {
        pos++;
        if (!parse_u32(s, &pos, len, old_count)) return false;
    } else {
        *old_count = (*old_start == 0) ? 0 : 1;
    }
    if (pos >= len || s[pos] != ' ') return false;
    pos++;
    if (pos >= len || s[pos] != '+') return false;
    pos++;
    if (!parse_u32(s, &pos, len, new_start)) return false;
    if (pos < len && s[pos] == ',') {
        pos++;
        if (!parse_u32(s, &pos, len, new_count)) return false;
    } else {
        *new_count = (*new_start == 0) ? 0 : 1;
    }
    while (pos + 2 <= len) {
        if (s[pos] == ' ' && s[pos + 1] == '@' && s[pos + 2] == '@') {
            return true;
        }
        pos++;
    }
    return false;
}

static bool parse_normal_header(const char *s, size_t len,
                                hunk_type_t *type,
                                uint32_t *old_start, uint32_t *old_count,
                                uint32_t *new_start, uint32_t *new_count) {
    size_t pos = 0;
    uint32_t o_start = 0, o_end = 0;
    if (!parse_u32(s, &pos, len, &o_start)) return false;
    if (pos < len && s[pos] == ',') {
        pos++;
        if (!parse_u32(s, &pos, len, &o_end)) return false;
    } else {
        o_end = o_start;
    }
    if (pos >= len) return false;
    char act = s[pos++];
    if (act == 'a') *type = HUNK_NORMAL_APPEND;
    else if (act == 'd') *type = HUNK_NORMAL_DELETE;
    else if (act == 'c') *type = HUNK_NORMAL_CHANGE;
    else return false;

    uint32_t n_start = 0, n_end = 0;
    if (!parse_u32(s, &pos, len, &n_start)) return false;
    if (pos < len && s[pos] == ',') {
        pos++;
        if (!parse_u32(s, &pos, len, &n_end)) return false;
    } else {
        n_end = n_start;
    }
    while (pos < len && (s[pos] == ' ' || s[pos] == '\r' || s[pos] == '\t')) pos++;
    if (pos < len) return false;

    *old_start = o_start;
    *old_count = (o_end >= o_start) ? (o_end - o_start + 1) : 1;
    *new_start = n_start;
    *new_count = (n_end >= n_start) ? (n_end - n_start + 1) : 1;
    return true;
}

static bool extract_diff_header_path(const char *line, size_t len, int strip_count, char *out, size_t out_max) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return false;

    size_t start = i;
    while (i < len && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
        if (line[i] == ' ' && i + 5 < len && line[i+1] >= '0' && line[i+1] <= '9' &&
            line[i+2] >= '0' && line[i+2] <= '9' && line[i+3] >= '0' && line[i+3] <= '9' &&
            line[i+4] >= '0' && line[i+4] <= '9' && line[i+5] == '-') {
            break;
        }
        i++;
    }
    size_t path_len = i - start;
    if (path_len == 0) return false;

    if (path_len == 9 && !tool_memcmp(line + start, "/dev/null", 9)) {
        out[0] = '\0';
        return true;
    }

    size_t p = start;
    if (strip_count > 0) {
        int stripped = 0;
        while (p < start + path_len && stripped < strip_count) {
            if (line[p] == '/') {
                stripped++;
                p++;
                while (p < start + path_len && line[p] == '/') p++;
            } else {
                p++;
            }
        }
    } else if (strip_count < 0) {
        if (path_len > 2 && (line[start] == 'a' || line[start] == 'b') && line[start + 1] == '/') {
            p = start + 2;
        }
    }

    size_t final_len = (start + path_len) - p;
    if (final_len == 0 || final_len >= out_max) return false;
    for (size_t k = 0; k < final_len; k++) {
        out[k] = line[p + k];
    }
    out[final_len] = '\0';
    return true;
}

static int read_stream_into_pool(int fd, const char *label,
                                 char *pool, size_t pool_max, size_t *pool_used,
                                 patch_line_t *lines, size_t lines_max, size_t *line_count) {
    size_t line_start = *pool_used;
    bool has_content = false;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("patch", "read error", label);
        if (n == 0) break;

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            has_content = true;
            if (c == '\n') {
                size_t len = *pool_used - line_start;
                if (len > 0 && pool[*pool_used - 1] == '\r') {
                    len--;
                    (*pool_used)--;
                }
                if (*line_count >= lines_max) {
                    return tool_error("patch", "maximum line count exceeded", label);
                }
                lines[*line_count].offset = (uint32_t)line_start;
                lines[*line_count].length = (uint16_t)len;
                lines[*line_count].has_newline = true;
                (*line_count)++;
                line_start = *pool_used;
            } else {
                if (*pool_used >= pool_max) {
                    return tool_error("patch", "file too large for memory buffer", label);
                }
                pool[(*pool_used)++] = c;
            }
        }
    }

    if (has_content && *pool_used > line_start) {
        size_t len = *pool_used - line_start;
        if (len > 0 && pool[*pool_used - 1] == '\r') {
            len--;
            (*pool_used)--;
        }
        if (*line_count >= lines_max) {
            return tool_error("patch", "maximum line count exceeded", label);
        }
        lines[*line_count].offset = (uint32_t)line_start;
        lines[*line_count].length = (uint16_t)len;
        lines[*line_count].has_newline = false;
        (*line_count)++;
    }
    return 0;
}

static int parse_diff(const patch_options_t *opts) {
    size_t cur = 0;
    while (cur < s_diff_line_count) {
        const char *line = s_diff_pool + s_diff_lines[cur].offset;
        size_t len = s_diff_lines[cur].length;

        /* Check for "--- " header */
        if (len >= 4 && line[0] == '-' && line[1] == '-' && line[2] == '-' && line[3] == ' ') {
            if (!opts->target_file && s_detected_target[0] == '\0') {
                extract_diff_header_path(line + 4, len - 4, opts->strip_count, s_detected_old, sizeof(s_detected_old));
            }
            cur++;
            continue;
        }

        /* Check for "+++ " header */
        if (len >= 4 && line[0] == '+' && line[1] == '+' && line[2] == '+' && line[3] == ' ') {
            if (!opts->target_file && s_detected_target[0] == '\0') {
                extract_diff_header_path(line + 4, len - 4, opts->strip_count, s_detected_new, sizeof(s_detected_new));
                if (s_detected_new[0] != '\0') {
                    tool_memcpy(s_detected_target, s_detected_new, tool_length(s_detected_new) + 1);
                } else if (s_detected_old[0] != '\0') {
                    tool_memcpy(s_detected_target, s_detected_old, tool_length(s_detected_old) + 1);
                }
            }
            cur++;
            continue;
        }

        /* Check for Unified Hunk "@@ " */
        if (len >= 4 && line[0] == '@' && line[1] == '@' && line[2] == ' ') {
            uint32_t old_start = 0, old_count = 0, new_start = 0, new_count = 0;
            if (!parse_unified_header(line, len, &old_start, &old_count, &new_start, &new_count)) {
                return tool_error("patch", "malformed unified hunk header", NULL);
            }
            if (s_hunk_count >= PATCH_MAX_HUNKS) {
                return tool_error("patch", "too many hunks in patch", NULL);
            }

            patch_hunk_t *hunk = &s_hunks[s_hunk_count++];
            hunk->target_line = opts->reverse ? new_start : old_start;
            if (hunk->target_line == 0) hunk->target_line = 1;
            hunk->op_start = (uint32_t)s_op_count;
            hunk->op_count = 0;
            hunk->applied_at_line = 0;
            hunk->offset = 0;
            hunk->applied = false;

            cur++;
            while (cur < s_diff_line_count) {
                const char *hl = s_diff_pool + s_diff_lines[cur].offset;
                size_t hlen = s_diff_lines[cur].length;
                bool hnl = s_diff_lines[cur].has_newline;

                if (hlen >= 4 && ((hl[0] == '@' && hl[1] == '@' && hl[2] == ' ') ||
                                 (hl[0] == '-' && hl[1] == '-' && hl[2] == '-' && hl[3] == ' ') ||
                                 (hl[0] == '+' && hl[1] == '+' && hl[2] == '+' && hl[3] == ' '))) {
                    break;
                }
                if (hlen > 0 && hl[0] >= '0' && hl[0] <= '9') {
                    hunk_type_t dummy_t;
                    uint32_t d1, d2, d3, d4;
                    if (parse_normal_header(hl, hlen, &dummy_t, &d1, &d2, &d3, &d4)) {
                        break;
                    }
                }

                if (hlen > 0 && hl[0] == '\\') {
                    cur++;
                    continue;
                }

                char marker = (hlen > 0) ? hl[0] : ' ';
                size_t text_len = (hlen > 0) ? (hlen - 1) : 0;

                if (marker != ' ' && marker != '+' && marker != '-') {
                    break;
                }

                if (s_op_count >= PATCH_MAX_OPS) {
                    return tool_error("patch", "too many operations in patch", NULL);
                }

                hunk_op_t *op = &s_ops[s_op_count++];
                hunk->op_count++;
                op->text_offset = (uint32_t)(s_diff_lines[cur].offset + ((hlen > 0) ? 1 : 0));
                op->text_length = (uint16_t)text_len;
                op->has_newline = hnl;

                if (marker == ' ') {
                    op->type = OP_CONTEXT;
                } else if (marker == '-') {
                    op->type = opts->reverse ? OP_ADD : OP_DELETE;
                } else if (marker == '+') {
                    op->type = opts->reverse ? OP_DELETE : OP_ADD;
                }

                cur++;
            }
            continue;
        }

        /* Check for Normal Diff Hunk: e.g. "1,3c1,4" or "4a5,6" or "2d1" */
        if (len > 0 && line[0] >= '0' && line[0] <= '9') {
            hunk_type_t ntype;
            uint32_t old_start = 0, old_count = 0, new_start = 0, new_count = 0;
            if (parse_normal_header(line, len, &ntype, &old_start, &old_count, &new_start, &new_count)) {
                if (s_hunk_count >= PATCH_MAX_HUNKS) {
                    return tool_error("patch", "too many hunks in patch", NULL);
                }

                patch_hunk_t *hunk = &s_hunks[s_hunk_count++];
                hunk->op_start = (uint32_t)s_op_count;
                hunk->op_count = 0;
                hunk->applied_at_line = 0;
                hunk->offset = 0;
                hunk->applied = false;

                if (!opts->reverse) {
                    hunk->target_line = (ntype == HUNK_NORMAL_APPEND) ? (old_start + 1) : old_start;
                } else {
                    hunk->target_line = (ntype == HUNK_NORMAL_DELETE) ? (new_start + 1) : new_start;
                }
                if (hunk->target_line == 0) hunk->target_line = 1;

                cur++;
                while (cur < s_diff_line_count) {
                    const char *nl = s_diff_pool + s_diff_lines[cur].offset;
                    size_t nlen = s_diff_lines[cur].length;
                    bool nnl = s_diff_lines[cur].has_newline;

                    if (nlen == 0) break;
                    if (nlen == 3 && nl[0] == '-' && nl[1] == '-' && nl[2] == '-') {
                        cur++;
                        continue;
                    }
                    if (nl[0] >= '0' && nl[0] <= '9') {
                        hunk_type_t dummy_t;
                        uint32_t d1, d2, d3, d4;
                        if (parse_normal_header(nl, nlen, &dummy_t, &d1, &d2, &d3, &d4)) {
                            break;
                        }
                    }
                    if (nlen >= 4 && ((nl[0] == '@' && nl[1] == '@' && nl[2] == ' ') ||
                                     (nl[0] == '-' && nl[1] == '-' && nl[2] == '-' && nl[3] == ' ') ||
                                     (nl[0] == '+' && nl[1] == '+' && nl[2] == '+' && nl[3] == ' '))) {
                        break;
                    }

                    char marker = nl[0];
                    if (marker != '<' && marker != '>') {
                        break;
                    }
                    size_t skip = 1;
                    if (nlen > 1 && nl[1] == ' ') skip = 2;

                    if (s_op_count >= PATCH_MAX_OPS) {
                        return tool_error("patch", "too many operations in patch", NULL);
                    }

                    hunk_op_t *op = &s_ops[s_op_count++];
                    hunk->op_count++;
                    op->text_offset = (uint32_t)(s_diff_lines[cur].offset + skip);
                    op->text_length = (uint16_t)(nlen >= skip ? (nlen - skip) : 0);
                    op->has_newline = nnl;

                    if (marker == '<') {
                        op->type = opts->reverse ? OP_ADD : OP_DELETE;
                    } else {
                        op->type = opts->reverse ? OP_DELETE : OP_ADD;
                    }

                    cur++;
                }
                continue;
            }
        }

        cur++;
    }
    return 0;
}

static uint32_t hunk_orig_consumed(const patch_hunk_t *hunk) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < hunk->op_count; i++) {
        const hunk_op_t *op = &s_ops[hunk->op_start + i];
        if (op->type == OP_CONTEXT || op->type == OP_DELETE) {
            count++;
        }
    }
    return count;
}

static bool match_hunk_at(const patch_hunk_t *hunk, uint32_t line_idx) {
    uint32_t orig_cur = line_idx;

    for (uint32_t i = 0; i < hunk->op_count; i++) {
        const hunk_op_t *op = &s_ops[hunk->op_start + i];
        if (op->type == OP_CONTEXT || op->type == OP_DELETE) {
            if (orig_cur > s_orig_line_count) {
                return false;
            }
            const patch_line_t *orig_line = &s_orig_lines[orig_cur - 1];
            if (orig_line->length != op->text_length) {
                return false;
            }
            const char *t_orig = s_orig_pool + orig_line->offset;
            const char *t_op = s_diff_pool + op->text_offset;
            if (tool_memcmp(t_orig, t_op, op->text_length) != 0) {
                return false;
            }
            orig_cur++;
        }
    }
    return true;
}

static bool locate_hunk(patch_hunk_t *hunk, uint32_t min_line) {
    uint32_t target = hunk->target_line;
    if (target < min_line) target = min_line;

    if (target >= min_line && match_hunk_at(hunk, target)) {
        hunk->applied_at_line = (int32_t)target;
        hunk->offset = 0;
        hunk->applied = true;
        return true;
    }

    for (int32_t delta = 1; delta <= 100; delta++) {
        int64_t cand_pos = (int64_t)target + delta;
        if (cand_pos >= (int64_t)min_line && cand_pos <= (int64_t)(s_orig_line_count + 1)) {
            if (match_hunk_at(hunk, (uint32_t)cand_pos)) {
                hunk->applied_at_line = (int32_t)cand_pos;
                hunk->offset = delta;
                hunk->applied = true;
                return true;
            }
        }
        int64_t cand_neg = (int64_t)target - delta;
        if (cand_neg >= (int64_t)min_line && cand_neg <= (int64_t)(s_orig_line_count + 1)) {
            if (match_hunk_at(hunk, (uint32_t)cand_neg)) {
                hunk->applied_at_line = (int32_t)cand_neg;
                hunk->offset = -delta;
                hunk->applied = true;
                return true;
            }
        }
    }
    return false;
}

static int emit_orig_line(int out_fd, uint32_t line_num) {
    const patch_line_t *l = &s_orig_lines[line_num - 1];
    const char *text = s_orig_pool + l->offset;
    int r = emit_out(out_fd, text, l->length);
    if (r) return r;
    if (l->has_newline) {
        r = emit_out(out_fd, "\n", 1);
    }
    return r;
}

static int emit_diff_op(int out_fd, const hunk_op_t *op) {
    const char *text = s_diff_pool + op->text_offset;
    int r = emit_out(out_fd, text, op->text_length);
    if (r) return r;
    if (op->has_newline) {
        r = emit_out(out_fd, "\n", 1);
    }
    return r;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: patch [OPTIONS] [ORIGFILE [PATCHFILE]]\n"
        "Apply a diff file to an original.\n\n"
        "Options:\n"
        "  -p, --strip=NUM      Strip NUM leading components from file names\n"
        "  -i, --input=FILE     Read patch from FILE instead of stdin\n"
        "  -o, --output=FILE    Write output to FILE instead of in-place or stdout\n"
        "  -R, --reverse        Assume patch was created with reversed diff\n"
        "  -u, --unified        Interpret patch as unified diff\n"
        "  -s, -q, --quiet      Suppress non-error diagnostics\n"
        "  --dry-run            Test patch without modifying any files\n"
        "  --help               Display this help and exit\n";
    print_msg(help_text);
    return 0;
}

int patch_main(int argc, char **argv) {
    /* Reset state for testability and re-entry */
    s_orig_pool_used = 0;
    s_orig_line_count = 0;
    s_diff_pool_used = 0;
    s_diff_line_count = 0;
    s_op_count = 0;
    s_hunk_count = 0;
    s_out_pos = 0;
    s_detected_target[0] = '\0';
    s_detected_old[0] = '\0';
    s_detected_new[0] = '\0';

    patch_options_t opts;
    tool_memset(&opts, 0, sizeof(opts));
    opts.strip_count = -1;

    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            return print_help();
        }
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (tool_equal(arg, "--dry-run")) {
            opts.dry_run = true;
            i++;
            continue;
        }
        if (tool_equal(arg, "--reverse")) {
            opts.reverse = true;
            i++;
            continue;
        }
        if (tool_equal(arg, "--unified")) {
            opts.unified = true;
            i++;
            continue;
        }
        if (tool_equal(arg, "--quiet") || tool_equal(arg, "--silent")) {
            opts.quiet = true;
            i++;
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            if (!tool_memcmp(arg, "--strip=", 8)) {
                uint32_t val = 0;
                if (!decimal_u32_str(arg + 8, &val)) {
                    tool_error("patch", "invalid strip count", arg + 8);
                    return 2;
                }
                opts.strip_count = (int)val;
                i++;
                continue;
            }
            if (!tool_memcmp(arg, "--input=", 8)) {
                opts.input_file = arg + 8;
                i++;
                continue;
            }
            if (!tool_memcmp(arg, "--output=", 9)) {
                opts.output_file = arg + 9;
                i++;
                continue;
            }
            tool_error("patch", "unrecognized option", arg);
            return 2;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            size_t j = 1;
            bool advance_i = true;
            while (arg[j] != '\0') {
                char opt = arg[j];
                if (opt == 'R') {
                    opts.reverse = true;
                } else if (opt == 'u') {
                    opts.unified = true;
                } else if (opt == 's' || opt == 'q') {
                    opts.quiet = true;
                } else if (opt == 'p') {
                    const char *val_str = NULL;
                    if (arg[j + 1] != '\0') {
                        val_str = &arg[j + 1];
                    } else if (i + 1 < argc) {
                        val_str = argv[++i];
                    } else {
                        tool_error("patch", "option requires an argument -- 'p'", NULL);
                        return 2;
                    }
                    uint32_t val = 0;
                    if (!decimal_u32_str(val_str, &val)) {
                        tool_error("patch", "invalid strip count", val_str);
                        return 2;
                    }
                    opts.strip_count = (int)val;
                    advance_i = false;
                    break;
                } else if (opt == 'i') {
                    if (arg[j + 1] != '\0') {
                        opts.input_file = &arg[j + 1];
                    } else if (i + 1 < argc) {
                        opts.input_file = argv[++i];
                    } else {
                        tool_error("patch", "option requires an argument -- 'i'", NULL);
                        return 2;
                    }
                    advance_i = false;
                    break;
                } else if (opt == 'o') {
                    if (arg[j + 1] != '\0') {
                        opts.output_file = &arg[j + 1];
                    } else if (i + 1 < argc) {
                        opts.output_file = argv[++i];
                    } else {
                        tool_error("patch", "option requires an argument -- 'o'", NULL);
                        return 2;
                    }
                    advance_i = false;
                    break;
                } else {
                    tool_error("patch", "invalid option", NULL);
                    return 2;
                }
                j++;
            }
            if (advance_i) i++;
            else i++;
            continue;
        }

        /* Positional operands */
        if (!opts.target_file) {
            opts.target_file = arg;
        } else if (!opts.patch_file) {
            opts.patch_file = arg;
        } else {
            tool_error("patch", "extra operand", arg);
            return 2;
        }
        i++;
    }

    /* Determine patch input source */
    const char *patch_src = opts.patch_file ? opts.patch_file : opts.input_file;
    int patch_fd = 0;
    if (patch_src && !tool_equal(patch_src, "-")) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)patch_src, VFS_O_RDONLY, 0);
        if (fd < 0) {
            tool_error("patch", "cannot open patch file", patch_src);
            return 2;
        }
        patch_fd = (int)fd;
    }

    /* Read the patch file */
    int r = read_stream_into_pool(patch_fd, patch_src ? patch_src : "standard input",
                                  s_diff_pool, PATCH_DIFF_POOL_SIZE, &s_diff_pool_used,
                                  s_diff_lines, PATCH_MAX_DIFF_LINES, &s_diff_line_count);
    if (patch_fd != 0) {
        tool_syscall(SYS_CLOSE, (uintptr_t)patch_fd, 0, 0);
    }
    if (r) return 2;

    /* Parse diff headers and hunks */
    r = parse_diff(&opts);
    if (r) return 2;

    if (s_hunk_count == 0) {
        if (!opts.quiet) {
            print_msg("patch: no hunks found in patch file\n");
        }
        return 0;
    }

    /* Determine target file to patch */
    const char *target = opts.target_file;
    if (!target) {
        if (s_detected_target[0] != '\0') {
            target = s_detected_target;
        } else {
            tool_error("patch", "missing target file", NULL);
            return 2;
        }
    }

    if (!opts.quiet) {
        print_msg("patching file ");
        print_msg(target);
        print_msg("\n");
    }

    /* Open and read target file (if it exists) */
    long target_fd = tool_syscall(SYS_OPEN, (uintptr_t)target, VFS_O_RDONLY, 0);
    if (target_fd >= 0) {
        r = read_stream_into_pool((int)target_fd, target,
                                  s_orig_pool, PATCH_ORIG_POOL_SIZE, &s_orig_pool_used,
                                  s_orig_lines, PATCH_MAX_ORIG_LINES, &s_orig_line_count);
        tool_syscall(SYS_CLOSE, (uintptr_t)target_fd, 0, 0);
        if (r) return 2;
    } else {
        /* File does not exist yet (could be file creation) */
        s_orig_line_count = 0;
        s_orig_pool_used = 0;
    }

    /* Validate and locate all hunks */
    uint32_t next_min_line = 1;
    bool all_ok = true;
    uint32_t failed_count = 0;

    for (size_t h = 0; h < s_hunk_count; h++) {
        patch_hunk_t *hunk = &s_hunks[h];
        if (locate_hunk(hunk, next_min_line)) {
            if (!opts.quiet) {
                print_hunk_success((uint32_t)(h + 1), (uint32_t)hunk->applied_at_line, hunk->offset);
            }
            next_min_line = (uint32_t)hunk->applied_at_line + hunk_orig_consumed(hunk);
        } else {
            all_ok = false;
            failed_count++;
            print_hunk_failed((uint32_t)(h + 1), hunk->target_line);
        }
    }

    if (!all_ok) {
        char buf[32];
        print_msg("patch: ");
        tool_format_u64(buf, failed_count);
        print_msg(buf);
        print_msg(" out of ");
        tool_format_u64(buf, s_hunk_count);
        print_msg(buf);
        print_msg(" hunks FAILED -- file left unchanged\n");
        return 1;
    }

    if (opts.dry_run) {
        return 0;
    }

    /* All hunks verified — emit patched output */
    int out_fd = -1;
    if (opts.output_file) {
        if (tool_equal(opts.output_file, "-")) {
            out_fd = 1;
        } else {
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.output_file,
                                   VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
            if (fd < 0) {
                tool_error("patch", "cannot create output file", opts.output_file);
                return 2;
            }
            out_fd = (int)fd;
        }
    } else {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)target,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
        if (fd < 0) {
            tool_error("patch", "cannot write to target file", target);
            return 2;
        }
        out_fd = (int)fd;
    }

    uint32_t orig_cur = 1;
    for (size_t h = 0; h < s_hunk_count; h++) {
        const patch_hunk_t *hunk = &s_hunks[h];
        while (orig_cur < (uint32_t)hunk->applied_at_line && orig_cur <= s_orig_line_count) {
            emit_orig_line(out_fd, orig_cur);
            orig_cur++;
        }

        for (uint32_t k = 0; k < hunk->op_count; k++) {
            const hunk_op_t *op = &s_ops[hunk->op_start + k];
            if (op->type == OP_CONTEXT) {
                emit_orig_line(out_fd, orig_cur);
                orig_cur++;
            } else if (op->type == OP_DELETE) {
                orig_cur++;
            } else if (op->type == OP_ADD) {
                emit_diff_op(out_fd, op);
            }
        }
    }

    while (orig_cur <= s_orig_line_count) {
        emit_orig_line(out_fd, orig_cur);
        orig_cur++;
    }

    flush_out_buf(out_fd);
    if (out_fd != 1) {
        tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);
    }

    return 0;
}
