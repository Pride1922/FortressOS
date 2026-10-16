#include "types.h"
#include "syscall_abi.h"

/* Static BSS storage to strictly respect the 512-byte Ring 3 stack budget. */
static proc_info_t s_proc_info;
static char s_out_buf[256];

#ifndef PS_HOST_TEST
static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_procinfo(uint64_t index, proc_info_t *buf) {
    long nr = SYS_PROCINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(index), "S"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#else
long sys_write(int fd, const void *buf, size_t count);
long sys_procinfo(uint64_t index, proc_info_t *buf);
#endif

static int write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        long w = sys_write(fd, buf, count);
        if (w <= 0) {
            return -1;
        }
        buf += w;
        count -= (size_t)w;
    }
    return 0;
}

static size_t str_len(const char *s, size_t max) {
    size_t len = 0;
    while (len < max && s[len] != '\0') {
        len++;
    }
    return len;
}

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void format_i64_right(char *dst, size_t width, int64_t val) {
    uint64_t u = (val < 0) ? (uint64_t)(-(val + 1)) + 1 : (uint64_t)val;
    size_t pos = width;
    if (u == 0) {
        if (pos > 0) dst[--pos] = '0';
    } else {
        while (u > 0 && pos > 0) {
            dst[--pos] = (char)('0' + (u % 10));
            u /= 10;
        }
    }
    if (val < 0 && pos > 0) {
        dst[--pos] = '-';
    }
    while (pos > 0) {
        dst[--pos] = ' ';
    }
}

static const char *state_to_str(uint32_t state) {
    switch (state) {
        case PROC_STATE_RUNNING: return "RUNNING";
        case PROC_STATE_STOPPED: return "STOPPED";
        case PROC_STATE_ZOMBIE:  return "ZOMBIE";
        case PROC_STATE_DONE:    return "DONE";
        default:                 return "UNKNOWN";
    }
}

enum ps_col {
    COL_PID = 0,
    COL_PPID,
    COL_PGID,
    COL_SID,
    COL_STATE,
    COL_NAME,
    COL_MAX
};

#define MAX_COLS 16
static uint8_t s_cols[MAX_COLS];
static size_t s_num_cols = 0;

static const char s_hdr_pid[]   = "  PID";
static const char s_hdr_ppid[]  = "  PPID";
static const char s_hdr_pgid[]  = "  PGID";
static const char s_hdr_sid[]   = "   SID";
static const char s_hdr_state[] = "STATE   ";
static const char s_hdr_name[]  = "NAME";

static int parse_column(const char *name, size_t nlen) {
    char token[32];
    if (nlen >= sizeof(token)) return -1;
    for (size_t i = 0; i < nlen; i++) token[i] = name[i];
    token[nlen] = '\0';

    if (str_eq(token, "pid")) return COL_PID;
    if (str_eq(token, "ppid")) return COL_PPID;
    if (str_eq(token, "pgid")) return COL_PGID;
    if (str_eq(token, "sid")) return COL_SID;
    if (str_eq(token, "state") || str_eq(token, "stat")) return COL_STATE;
    if (str_eq(token, "name") || str_eq(token, "comm") || str_eq(token, "cmd") || str_eq(token, "args")) return COL_NAME;
    return -1;
}

static int add_columns_from_arg(const char *arg) {
    const char *p = arg;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        int col = parse_column(start, len);
        if (col < 0) {
            static const char err_pre[] = "ps: unknown column '";
            write_all(2, err_pre, sizeof(err_pre) - 1);
            write_all(2, start, len);
            write_all(2, "'\n", 2);
            return -1;
        }
        if (s_num_cols < MAX_COLS) {
            s_cols[s_num_cols++] = (uint8_t)col;
        }
    }
    return 0;
}

static void print_help(void) {
    static const char help_msg[] =
        "Usage: ps [options]\n"
        "Snapshot active process table.\n\n"
        "Options:\n"
        "  -o, --format, --output <cols>  Comma-separated columns to display\n"
        "  -h, --help                     Display this help and exit\n\n"
        "Available columns:\n"
        "  pid, ppid, pgid, sid, state (stat), name (comm, cmd)\n";
    write_all(1, help_msg, sizeof(help_msg) - 1);
}

int ps_main(int argc, char **argv) {
    s_num_cols = 0;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (str_eq(arg, "-h") || str_eq(arg, "--help")) {
            print_help();
            return 0;
        } else if (str_eq(arg, "-o") || str_eq(arg, "--output") || str_eq(arg, "--format")) {
            if (i + 1 >= argc) {
                static const char msg[] = "ps: option -o requires an argument\n";
                write_all(2, msg, sizeof(msg) - 1);
                return 1;
            }
            if (add_columns_from_arg(argv[++i]) < 0) {
                return 1;
            }
        } else if (arg[0] == '-' && arg[1] == 'o' && arg[2] != '\0') {
            /* Handle attached argument, e.g. -opid,name */
            if (add_columns_from_arg(arg + 2) < 0) {
                return 1;
            }
        } else {
            static const char msg[] = "ps: unrecognized option '";
            write_all(2, msg, sizeof(msg) - 1);
            write_all(2, arg, str_len(arg, 64));
            write_all(2, "'\n", 2);
            return 1;
        }
    }

    /* If no columns specified, use default POSIX-style layout */
    if (s_num_cols == 0) {
        s_cols[0] = COL_PID;
        s_cols[1] = COL_PPID;
        s_cols[2] = COL_PGID;
        s_cols[3] = COL_SID;
        s_cols[4] = COL_STATE;
        s_cols[5] = COL_NAME;
        s_num_cols = 6;
    }

    /* Print header row */
    size_t pos = 0;
    for (size_t c = 0; c < s_num_cols; c++) {
        if (c > 0 && pos + 1 < sizeof(s_out_buf)) s_out_buf[pos++] = ' ';
        const char *h = NULL;
        switch (s_cols[c]) {
            case COL_PID:   h = s_hdr_pid; break;
            case COL_PPID:  h = s_hdr_ppid; break;
            case COL_PGID:  h = s_hdr_pgid; break;
            case COL_SID:   h = s_hdr_sid; break;
            case COL_STATE: h = s_hdr_state; break;
            case COL_NAME:  h = s_hdr_name; break;
            default: break;
        }
        if (h) {
            size_t hlen = str_len(h, 32);
            for (size_t j = 0; j < hlen && pos + 1 < sizeof(s_out_buf); j++) {
                s_out_buf[pos++] = h[j];
            }
        }
    }
    if (pos + 1 < sizeof(s_out_buf)) s_out_buf[pos++] = '\n';
    if (write_all(1, s_out_buf, pos) != 0) {
        return 1;
    }

    /* Print process records */
    for (uint64_t idx = 0; idx < PROC_INFO_MAX; ++idx) {
        long ret = sys_procinfo(idx, &s_proc_info);
        if (ret == 0) {
            break;
        }
        if (ret < 0) {
            static const char err_msg[] = "ps: error reading process info\n";
            write_all(2, err_msg, sizeof(err_msg) - 1);
            return 1;
        }

        pos = 0;
        for (size_t c = 0; c < s_num_cols; c++) {
            if (c > 0 && pos + 1 < sizeof(s_out_buf)) s_out_buf[pos++] = ' ';

            switch (s_cols[c]) {
                case COL_PID:
                    if (pos + 5 < sizeof(s_out_buf)) {
                        format_i64_right(&s_out_buf[pos], 5, s_proc_info.pid);
                        pos += 5;
                    }
                    break;
                case COL_PPID:
                    if (pos + 6 < sizeof(s_out_buf)) {
                        format_i64_right(&s_out_buf[pos], 6, s_proc_info.ppid);
                        pos += 6;
                    }
                    break;
                case COL_PGID:
                    if (pos + 6 < sizeof(s_out_buf)) {
                        format_i64_right(&s_out_buf[pos], 6, s_proc_info.pgid);
                        pos += 6;
                    }
                    break;
                case COL_SID:
                    if (pos + 6 < sizeof(s_out_buf)) {
                        format_i64_right(&s_out_buf[pos], 6, s_proc_info.sid);
                        pos += 6;
                    }
                    break;
                case COL_STATE: {
                    const char *st = state_to_str(s_proc_info.state);
                    size_t slen = str_len(st, 8);
                    for (size_t i = 0; i < slen && pos + 1 < sizeof(s_out_buf); ++i) {
                        s_out_buf[pos++] = st[i];
                    }
                    for (size_t i = slen; i < 8 && pos + 1 < sizeof(s_out_buf); ++i) {
                        s_out_buf[pos++] = ' ';
                    }
                    break;
                }
                case COL_NAME: {
                    size_t nlen = str_len(s_proc_info.name, sizeof(s_proc_info.name));
                    for (size_t i = 0; i < nlen && pos + 1 < sizeof(s_out_buf); ++i) {
                        s_out_buf[pos++] = s_proc_info.name[i];
                    }
                    break;
                }
                default: break;
            }
        }
        if (pos + 1 < sizeof(s_out_buf)) s_out_buf[pos++] = '\n';

        if (write_all(1, s_out_buf, pos) != 0) {
            return 1;
        }
    }

    return 0;
}
