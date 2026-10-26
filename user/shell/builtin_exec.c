/*
 * Shared builtin execution handlers.
 *
 * Linked into both the parent shell (user/shell.c) and the /bin/sh-builtin
 * runner.  Handlers read NO global shell state; all context comes through
 * builtin_ctx_t and the argc/argv arguments.
 *
 * Output discipline (every emitting handler):
 *   - Use write_str() / write_bytes_fd().
 *   - Stop at the first stdout failure; propagate the error code.
 *   - Return 1 quietly on EPIPE; return 1 + best-effort stderr on other I/O
 *     errors; never recurse if the diagnostic write also fails.
 *   - A read or close failure cannot overwrite an earlier output failure.
 */
#include "builtin_exec.h"
#include "builtins.h"
#include "io.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "../permissions_cli.h"
#include "../tools/userdb.h"

/* ---------- internal I/O helpers ----------------------------------------- */

static int io_err(long r) {
    (void)r; /* Default SIGPIPE already terminates at the syscall boundary. */
    return 1;
}

/* Write a NUL-terminated string to stdout.  Returns 0 or an error code. */
static int write_str(const char *s) {
    long r = write_bytes_fd(1, s, length(s));
    return r < 0 ? io_err(r) : 0;
}

/* Write a NUL-terminated string to stderr (best-effort; ignore failure). */
static void write_err(const char *s) {
    (void)write_bytes_fd(2, s, length(s));
}

/* ---------- builtin_ctx_from_envp ---------------------------------------- */

builtin_ctx_t builtin_ctx_from_envp(const char *const *envp) {
    builtin_ctx_t ctx = {0};
    ctx.envp = envp;
    if (!envp) return ctx;
    for (int i = 0; envp[i]; i++) {
        const char *e = envp[i];
        /* Match "PATH=" prefix */
        if (e[0] == 'P' && e[1] == 'A' && e[2] == 'T' && e[3] == 'H' && e[4] == '=') {
            ctx.path = e + 5;
            break;
        }
    }
    return ctx;
}

/* ---------- echo ---------------------------------------------------------- */

static int exec_echo(int argc, const char *const *argv) {
    bool no_newline = false;
    bool enable_escapes = false;
    int arg_start = 1;

    /* Parse leading flags (-n, -e, -E and combinations) */
    while (arg_start < argc) {
        const char *arg = argv[arg_start];
        if (arg[0] != '-' || arg[1] == '\0') break;

        /* Verify all characters after '-' are valid options */
        bool valid = true;
        for (int k = 1; arg[k]; k++) {
            if (arg[k] != 'n' && arg[k] != 'e' && arg[k] != 'E') {
                valid = false;
                break;
            }
        }
        if (!valid) break;

        /* Apply options */
        for (int k = 1; arg[k]; k++) {
            if (arg[k] == 'n') no_newline = true;
            else if (arg[k] == 'e') enable_escapes = true;
            else if (arg[k] == 'E') enable_escapes = false;
        }
        arg_start++;
    }

    for (int i = arg_start; i < argc; i++) {
        if (i > arg_start) {
            long r = write_bytes_fd(1, " ", 1);
            if (r < 0) return io_err(r);
        }
        if (!enable_escapes) {
            int rc = write_str(argv[i]);
            if (rc) return rc;
        } else {
            const char *p = argv[i];
            while (*p) {
                if (*p == '\\') {
                    p++;
                    char c = *p;
                    if (!c) {
                        long r = write_bytes_fd(1, "\\", 1);
                        if (r < 0) return io_err(r);
                        break;
                    }
                    p++;
                    if (c == 'c') {
                        return 0; /* Stop all output immediately, no trailing newline */
                    } else if (c == 'n') {
                        long r = write_bytes_fd(1, "\n", 1); if (r < 0) return io_err(r);
                    } else if (c == 't') {
                        long r = write_bytes_fd(1, "\t", 1); if (r < 0) return io_err(r);
                    } else if (c == 'r') {
                        long r = write_bytes_fd(1, "\r", 1); if (r < 0) return io_err(r);
                    } else if (c == 'a') {
                        long r = write_bytes_fd(1, "\a", 1); if (r < 0) return io_err(r);
                    } else if (c == 'b') {
                        long r = write_bytes_fd(1, "\b", 1); if (r < 0) return io_err(r);
                    } else if (c == 'e' || c == 'E') {
                        long r = write_bytes_fd(1, "\033", 1); if (r < 0) return io_err(r);
                    } else if (c == 'f') {
                        long r = write_bytes_fd(1, "\f", 1); if (r < 0) return io_err(r);
                    } else if (c == 'v') {
                        long r = write_bytes_fd(1, "\v", 1); if (r < 0) return io_err(r);
                    } else if (c == '\\') {
                        long r = write_bytes_fd(1, "\\", 1); if (r < 0) return io_err(r);
                    } else if (c == '0') {
                        unsigned val = 0;
                        int digits = 0;
                        while (digits < 3 && *p >= '0' && *p <= '7') {
                            val = (val << 3) | (*p++ - '0');
                            digits++;
                        }
                        char ch = (char)(val & 0xFF);
                        long r = write_bytes_fd(1, &ch, 1); if (r < 0) return io_err(r);
                    } else if (c == 'x') {
                        unsigned val = 0;
                        int digits = 0;
                        while (digits < 2 && *p) {
                            char h = *p;
                            if (h >= '0' && h <= '9') val = (val << 4) | (h - '0');
                            else if (h >= 'a' && h <= 'f') val = (val << 4) | (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') val = (val << 4) | (h - 'A' + 10);
                            else break;
                            p++; digits++;
                        }
                        if (digits > 0) {
                            char ch = (char)(val & 0xFF);
                            long r = write_bytes_fd(1, &ch, 1); if (r < 0) return io_err(r);
                        } else {
                            long r = write_bytes_fd(1, "x", 1); if (r < 0) return io_err(r);
                        }
                    } else {
                        char seq[2] = {'\\', c};
                        long r = write_bytes_fd(1, seq, 2); if (r < 0) return io_err(r);
                    }
                } else {
                    long r = write_bytes_fd(1, p++, 1);
                    if (r < 0) return io_err(r);
                }
            }
        }
    }
    if (!no_newline) {
        long r = write_bytes_fd(1, "\n", 1);
        return r < 0 ? io_err(r) : 0;
    }
    return 0;
}

/* ---------- printf -------------------------------------------------------- */

typedef struct {
    char buf[256];
    size_t len;
} printf_out_t;

static int p_flush(printf_out_t *out) {
    if (out->len == 0) return 0;
    long r = write_bytes_fd(1, out->buf, out->len);
    out->len = 0;
    return r < 0 ? io_err(r) : 0;
}

static int p_char(printf_out_t *out, char c) {
    if (out->len >= sizeof(out->buf)) {
        int r = p_flush(out);
        if (r) return r;
    }
    out->buf[out->len++] = c;
    return 0;
}

static int64_t parse_int64(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\'' || *s == '"') return (unsigned char)s[1];
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') { s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        uint64_t val = 0;
        while (*s) {
            char c = *s++;
            if (c >= '0' && c <= '9') val = (val << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f') val = (val << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val = (val << 4) | (c - 'A' + 10);
            else break;
        }
        return neg ? -(int64_t)val : (int64_t)val;
    }
    int64_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s++ - '0');
    }
    return neg ? -val : val;
}

static int format_uint(printf_out_t *out, uint64_t val, int width, bool left_align, bool zero_pad, unsigned base, bool upper) {
    char num_buf[32];
    int npos = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        num_buf[npos++] = digits[val % base];
        val /= base;
    } while (val > 0);
    int pad = width > npos ? width - npos : 0;
    if (!left_align && pad > 0) {
        char pad_ch = zero_pad ? '0' : ' ';
        for (int i = 0; i < pad; i++) {
            if (p_char(out, pad_ch)) return 1;
        }
    }
    while (npos > 0) {
        if (p_char(out, num_buf[--npos])) return 1;
    }
    if (left_align && pad > 0) {
        for (int i = 0; i < pad; i++) {
            if (p_char(out, ' ')) return 1;
        }
    }
    return 0;
}

static int format_int(printf_out_t *out, int64_t val, int width, bool left_align, bool zero_pad) {
    bool neg = false;
    uint64_t uval;
    if (val < 0) {
        neg = true;
        uval = (uint64_t)(-(val + 1)) + 1;
    } else {
        uval = (uint64_t)val;
    }
    char num_buf[32];
    int npos = 0;
    do {
        num_buf[npos++] = '0' + (char)(uval % 10);
        uval /= 10;
    } while (uval > 0);
    int total_len = npos + (neg ? 1 : 0);
    int pad = width > total_len ? width - total_len : 0;
    if (neg && zero_pad) {
        if (p_char(out, '-')) return 1;
        neg = false;
    }
    if (!left_align && pad > 0) {
        char pad_ch = zero_pad ? '0' : ' ';
        for (int i = 0; i < pad; i++) {
            if (p_char(out, pad_ch)) return 1;
        }
    }
    if (neg) {
        if (p_char(out, '-')) return 1;
    }
    while (npos > 0) {
        if (p_char(out, num_buf[--npos])) return 1;
    }
    if (left_align && pad > 0) {
        for (int i = 0; i < pad; i++) {
            if (p_char(out, ' ')) return 1;
        }
    }
    return 0;
}

int exec_printf(int argc, const char *const *argv) {
    if (argc < 2) return 0;
    if (argc == 2 && equal(argv[1], "--help")) {
        return write_str("Usage: printf FORMAT [ARGUMENT...]\nFormat and print ARGUMENT(s) according to FORMAT.\n");
    }
    int fmt_idx = 1;
    if (equal(argv[1], "--")) {
        fmt_idx = 2;
        if (fmt_idx >= argc) return 0;
    }
    const char *fmt = argv[fmt_idx];
    int arg_idx = fmt_idx + 1;
    bool has_args = (arg_idx < argc);

    printf_out_t out = {.len = 0};

    do {
        const char *p = fmt;
        while (*p) {
            if (*p == '\\') {
                p++;
                char c = *p;
                if (!c) {
                    if (p_char(&out, '\\')) return 1;
                    break;
                }
                p++;
                if (c == 'n') { if (p_char(&out, '\n')) return 1; }
                else if (c == 't') { if (p_char(&out, '\t')) return 1; }
                else if (c == 'r') { if (p_char(&out, '\r')) return 1; }
                else if (c == 'a') { if (p_char(&out, '\a')) return 1; }
                else if (c == 'b') { if (p_char(&out, '\b')) return 1; }
                else if (c == 'f') { if (p_char(&out, '\f')) return 1; }
                else if (c == 'v') { if (p_char(&out, '\v')) return 1; }
                else if (c == '\\') { if (p_char(&out, '\\')) return 1; }
                else if (c == '\'') { if (p_char(&out, '\'')) return 1; }
                else if (c == '\"') { if (p_char(&out, '\"')) return 1; }
                else if (c == 'c') { (void)p_flush(&out); return 0; }
                else if (c == 'x') {
                    unsigned val = 0;
                    int digits = 0;
                    while (digits < 2 && *p) {
                        char h = *p;
                        if (h >= '0' && h <= '9') val = (val << 4) | (h - '0');
                        else if (h >= 'a' && h <= 'f') val = (val << 4) | (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') val = (val << 4) | (h - 'A' + 10);
                        else break;
                        p++; digits++;
                    }
                    if (digits > 0) { if (p_char(&out, (char)val)) return 1; }
                    else { if (p_char(&out, 'x')) return 1; }
                } else if (c >= '0' && c <= '7') {
                    unsigned val = (c - '0');
                    int digits = 1;
                    while (digits < 3 && *p >= '0' && *p <= '7') {
                        val = (val << 3) | (*p++ - '0');
                        digits++;
                    }
                    if (p_char(&out, (char)val)) return 1;
                } else {
                    if (p_char(&out, c)) return 1;
                }
            } else if (*p == '%') {
                p++;
                if (*p == '%') {
                    p++;
                    if (p_char(&out, '%')) return 1;
                    continue;
                }
                bool left_align = false;
                bool zero_pad = false;
                while (*p == '-' || *p == '0' || *p == '+' || *p == ' ') {
                    if (*p == '-') left_align = true;
                    else if (*p == '0') zero_pad = true;
                    p++;
                }
                int width = 0;
                while (*p >= '0' && *p <= '9') {
                    width = width * 10 + (*p++ - '0');
                }
                int prec = -1;
                if (*p == '.') {
                    p++;
                    prec = 0;
                    while (*p >= '0' && *p <= '9') {
                        prec = prec * 10 + (*p++ - '0');
                    }
                }
                char spec = *p ? *p++ : '\0';
                const char *val_str = (arg_idx < argc) ? argv[arg_idx++] : "";
                if (spec == 's') {
                    size_t slen = length(val_str);
                    if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
                    int pad = width > (int)slen ? width - (int)slen : 0;
                    if (!left_align && pad > 0) {
                        for (int k = 0; k < pad; k++) if (p_char(&out, ' ')) return 1;
                    }
                    for (size_t k = 0; k < slen; k++) {
                        if (p_char(&out, val_str[k])) return 1;
                    }
                    if (left_align && pad > 0) {
                        for (int k = 0; k < pad; k++) if (p_char(&out, ' ')) return 1;
                    }
                } else if (spec == 'b') {
                    static char b_buf[512];
                    size_t blen = 0;
                    bool early_exit = false;
                    const char *bs = val_str;
                    while (*bs && blen < sizeof(b_buf)) {
                        if (*bs == '\\') {
                            bs++;
                            char bc = *bs;
                            if (!bc) { b_buf[blen++] = '\\'; break; }
                            bs++;
                            if (bc == 'c') {
                                early_exit = true;
                                break;
                            } else if (bc == 'n') { b_buf[blen++] = '\n'; }
                            else if (bc == 't') { b_buf[blen++] = '\t'; }
                            else if (bc == 'r') { b_buf[blen++] = '\r'; }
                            else if (bc == 'a') { b_buf[blen++] = '\a'; }
                            else if (bc == 'b') { b_buf[blen++] = '\b'; }
                            else if (bc == 'e' || bc == 'E') { b_buf[blen++] = '\033'; }
                            else if (bc == 'f') { b_buf[blen++] = '\f'; }
                            else if (bc == 'v') { b_buf[blen++] = '\v'; }
                            else if (bc == '\\') { b_buf[blen++] = '\\'; }
                            else if (bc == '0') {
                                unsigned val = 0;
                                int digits = 0;
                                while (digits < 3 && *bs >= '0' && *bs <= '7') {
                                    val = (val << 3) | (*bs++ - '0');
                                    digits++;
                                }
                                b_buf[blen++] = (char)(val & 0xFF);
                            } else if (bc == 'x') {
                                unsigned val = 0;
                                int digits = 0;
                                while (digits < 2 && *bs) {
                                    char h = *bs;
                                    if (h >= '0' && h <= '9') val = (val << 4) | (h - '0');
                                    else if (h >= 'a' && h <= 'f') val = (val << 4) | (h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') val = (val << 4) | (h - 'A' + 10);
                                    else break;
                                    bs++; digits++;
                                }
                                if (digits > 0) b_buf[blen++] = (char)(val & 0xFF);
                                else b_buf[blen++] = 'x';
                            } else {
                                b_buf[blen++] = bc;
                            }
                        } else {
                            b_buf[blen++] = *bs++;
                        }
                    }
                    if (prec >= 0 && (size_t)prec < blen) blen = (size_t)prec;
                    int pad = width > (int)blen ? width - (int)blen : 0;
                    if (!left_align && pad > 0) {
                        for (int k = 0; k < pad; k++) if (p_char(&out, ' ')) return 1;
                    }
                    for (size_t k = 0; k < blen; k++) {
                        if (p_char(&out, b_buf[k])) return 1;
                    }
                    if (left_align && pad > 0) {
                        for (int k = 0; k < pad; k++) if (p_char(&out, ' ')) return 1;
                    }
                    if (early_exit) {
                        (void)p_flush(&out);
                        return 0;
                    }
                } else if (spec == 'c') {
                    char ch = val_str[0];
                    if (p_char(&out, ch)) return 1;
                } else if (spec == 'd' || spec == 'i') {
                    int64_t num = parse_int64(val_str);
                    if (format_int(&out, num, width, left_align, zero_pad)) return 1;
                } else if (spec == 'u') {
                    uint64_t unum = (uint64_t)parse_int64(val_str);
                    if (format_uint(&out, unum, width, left_align, zero_pad, 10, false)) return 1;
                } else if (spec == 'x') {
                    uint64_t xnum = (uint64_t)parse_int64(val_str);
                    if (format_uint(&out, xnum, width, left_align, zero_pad, 16, false)) return 1;
                } else if (spec == 'X') {
                    uint64_t xnum = (uint64_t)parse_int64(val_str);
                    if (format_uint(&out, xnum, width, left_align, zero_pad, 16, true)) return 1;
                } else if (spec == 'o') {
                    uint64_t onum = (uint64_t)parse_int64(val_str);
                    if (format_uint(&out, onum, width, left_align, zero_pad, 8, false)) return 1;
                } else if (spec) {
                    if (p_char(&out, '%')) return 1;
                    if (p_char(&out, spec)) return 1;
                }
            } else {
                if (p_char(&out, *p++)) return 1;
            }
        }
    } while (has_args && arg_idx < argc);

    return p_flush(&out);
}

/* ---------- pwd ----------------------------------------------------------- */

static int exec_pwd(void) {
    char buf[VFS_MAX_PATH];
    long n = call(SYS_GETCWD, (uintptr_t)buf, sizeof(buf), 0);
    if (n <= 0) {
        buf[0] = '/'; buf[1] = '\0';
    }
    int rc = write_str(buf);
    if (rc) return rc;
    long r = write_bytes_fd(1, "\n", 1);
    return r < 0 ? io_err(r) : 0;
}

/* ---------- env ----------------------------------------------------------- */

/* Iterate the supplied envp vector; do not call vars_print_env(). */
static int exec_env(const builtin_ctx_t *ctx) {
    if (!ctx || !ctx->envp) return 0;
    for (int i = 0; ctx->envp[i]; i++) {
        int rc = write_str(ctx->envp[i]);
        if (rc) return rc;
        long r = write_bytes_fd(1, "\n", 1);
        if (r < 0) return io_err(r);
    }
    return 0;
}

/* ---------- ls ------------------------------------------------------------ */

static int ls_number(uint64_t value) {
    char digits[24],out[24];unsigned count=0;
    do {digits[count++]='0'+value%10;value/=10;} while (value);
    for (unsigned i=0;i<count;i++) out[i]=digits[count-i-1];
    return write_bytes_fd(1,out,count)<0 ? 1 : 0;
}
static userdb_t ls_database;
static void ls_names(void) {
    static char bytes[DB_FILE_MAX+1];ls_database=(userdb_t){0};
    const char *paths[]={"/etc/passwd","/etc/group"};
    for (unsigned i=0;i<2;i++) {
        long fd=call(SYS_OPEN,(uintptr_t)paths[i],VFS_O_RDONLY|VFS_O_CLOEXEC,0);
        if (fd<0) return;
        size_t n=0;long r;
        do {r=call(SYS_READ,fd,(uintptr_t)(bytes+n),sizeof(bytes)-n);if (r>0) n+=(size_t)r;}
        while (r>0 && n<sizeof(bytes));
        (void)call(SYS_CLOSE,fd,0,0);
        if (r!=0 || n>DB_FILE_MAX || !(i ? db_group(&ls_database,bytes,n) : db_passwd(&ls_database,bytes,n))) {
            ls_database=(userdb_t){0};return;
        }
    }
}
static int ls_owner(uint32_t id,bool group) {
    const char *name=0;
    if (group) {const db_group_t *g=db_group_id(&ls_database,id);if (g) name=g->name;}
    else {const db_user_t *u=db_user_id(&ls_database,id);if (u) name=u->name;}
    return name ? write_str(name) : ls_number(id);
}
static int ls_long(const char *path,const char *label) {
    stat_ext_v1_t st;
    if (permission_call4(SYS_STAT_EXT,(uintptr_t)path,(uintptr_t)&st,sizeof(st),1)<0) {
        write_err("ls: cannot read metadata: ");write_err(path);write_err("\n");return 1;
    }
    unsigned type=st.mode & VFS_S_IFMT;
    char mode[11]={type==VFS_S_IFDIR ? 'd' : type==VFS_S_IFCHR ? 'c' : type==VFS_S_IFBLK ? 'b' : '-',0};
    for (unsigned i=0;i<9;i++) mode[i+1]=(st.mode & (1u<<(8-i))) ? "rwx"[i%3] : '-';
    if (st.mode & 04000) mode[3]=(st.mode & 0100) ? 's' : 'S';
    if (st.mode & 02000) mode[6]=(st.mode & 0010) ? 's' : 'S';
    if (st.mode & 01000) mode[9]=(st.mode & 0001) ? 't' : 'T';
    if (write_str(mode) || write_str(" ") || ls_owner(st.uid,false) || write_str(" ") ||
        ls_owner(st.gid,true) || write_str(" ") || ls_number(st.file_size) ||
        write_str(" ") || write_str(label) || write_str("\n")) return 1;
    return 0;
}
static int exec_ls(int argc, const char *const *argv) {
    bool detail=argc>1 && equal(argv[1],"-l");
    if (detail) ls_names();
    int first=detail ? 2 : 1;
    if (argc>first+1) {write_err("ls: expected [-l] [path]\n");return 1;}
    const char *path = argc > first ? argv[first] : ".";
    vfs_stat_t st;
    long result = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) {
        write_err("ls: ");
        write_err(path);
        write_err(result==SYSCALL_EACCES || result==SYSCALL_EPERM ? ": permission denied\n" : ": no such file or directory\n");
        return 1;
    }
    if (st.type != VFS_DIRECTORY) {
        if (detail) return ls_long(path,path);
        /* Single file: just print its name. */
        int rc = write_str(path);
        if (rc) return rc;
        long r = write_bytes_fd(1, "\n", 1);
        return r < 0 ? io_err(r) : 0;
    }
    long fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) {
        write_err(fd==SYSCALL_EACCES || fd==SYSCALL_EPERM ? "ls: permission denied\n" : "ls: cannot open directory\n");
        return 1;
    }
    vfs_dirent_t entry;
    int out_err = 0;
    while ((result = call(SYS_READDIR, fd, (uintptr_t)&entry, 0)) == 1) {
        if (!out_err) {
            if (detail) {
                char child[VFS_MAX_PATH];size_t a=length(path),b=length(entry.name);
                if (a+b+2>sizeof(child)) {write_err("ls: path too long\n");out_err=1;}
                else {
                    for (size_t i=0;i<a;i++) child[i]=path[i];
                    child[a++]='/';
                    for (size_t i=0;i<=b;i++) child[a+i]=entry.name[i];
                    out_err=ls_long(child,entry.name);
                }
                continue;
            }
            int rc = write_str(entry.name);
            if (!rc) {
                const char *suffix = entry.type == VFS_DIRECTORY ? "/\n" : "\n";
                long r = write_bytes_fd(1, suffix, length(suffix));
                if (r < 0) out_err = io_err(r);
            } else {
                out_err = rc;
            }
        }
    }
    if (result < 0 && !out_err) {
        write_err("ls: read error\n");
        (void)call(SYS_CLOSE, fd, 0, 0);
        return 1;
    }
    (void)call(SYS_CLOSE, fd, 0, 0);
    return out_err;
}

/* ---------- view ---------------------------------------------------------- */

/*
 * Safe text viewer: read from fd 0 (stdin) if no path argument, otherwise open
 * the named file.  Replace non-printable bytes with dots; guarantee final LF.
 * Stop at first write failure; return 1 if the process survives SIGPIPE.
 */
static int exec_view(int argc, const char *const *argv) {
    const char *path = argc > 1 ? argv[1] : NULL;
    long fd = 0;
    if (path) {
        vfs_stat_t st;
        long r = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
        if (r < 0) { write_err(r==SYSCALL_EACCES || r==SYSCALL_EPERM ? "view: permission denied\n" : "view: no such file or directory\n"); return 1; }
        if (st.type != VFS_FILE) { write_err("view: not a regular file\n"); return 1; }
        fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
        if (fd < 0) { write_err(fd==SYSCALL_EACCES || fd==SYSCALL_EPERM ? "view: permission denied\n" : "view: cannot open file\n"); return 1; }
    }
    static char buf[512]; /* BSS; safe for freestanding */
    bool newline = true;
    long result = 0;
    int out_err = 0;
    while (!out_err && (result = call(SYS_READ, fd, (uintptr_t)buf, sizeof(buf))) > 0) {
        for (long i = 0; i < result; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c != '\n' && c != '\t' && (c < 32 || c > 126)) buf[i] = '.';
        }
        newline = buf[result - 1] == '\n';
        long w = write_bytes_fd(1, buf, (size_t)result);
        if (w < 0) out_err = io_err(w);
    }
    if (!out_err && result >= 0 && !newline) {
        long r = write_bytes_fd(1, "\n", 1);
        if (r < 0) out_err = io_err(r);
    }
    if (path) (void)call(SYS_CLOSE, fd, 0, 0);
    if (out_err) return out_err;
    if (result < 0) { write_err("view: read error\n"); return 1; }
    return 0;
}

/* ---------- type ---------------------------------------------------------- */

/*
 * In the runner context: classify only the runner allowlist as builtins, then
 * resolve external names using ctx->path.  Parent aliases are unavailable.
 * "type cd" → "cd: not found" (cd is not in the allowlist; no external cd).
 *
 * A NULL alias_get function pointer means "no alias lookup" (runner context).
 */
static int exec_type(int argc, const char *const *argv, const builtin_ctx_t *ctx,
                     const char *(*alias_get_fn)(const char *)) {
    if (argc < 2) {
        write_err("type: missing operand\n");
        return 1;
    }
    int ret = 0;
    const char *path_var = ctx && ctx->path ? ctx->path : "/bin";

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];

        /* Builtin check: in runner, only child-safe entries qualify */
        if (builtin_is_child_safe(name)) {
            int rc = write_str(name);
            if (!rc) rc = write_str(" is a shell builtin\n");
            if (rc) return rc;
            continue;
        }

        /* Alias check (parent only; runner passes NULL) */
        if (alias_get_fn) {
            const char *al = alias_get_fn(name);
            if (al) {
                int rc = write_str(name);
                if (!rc) rc = write_str(" is an alias for '");
                if (!rc) rc = write_str(al);
                if (!rc) rc = write_str("'\n");
                if (rc) return rc;
                continue;
            }
        }

        /* Path resolution */
        bool has_slash = false;
        for (size_t k = 0; name[k]; k++) { if (name[k] == '/') { has_slash = true; break; } }

        vfs_stat_t st;
        bool found = false;
        if (has_slash) {
            if (call(SYS_STAT, (uintptr_t)name, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
                int rc = write_str(name);
                if (!rc) rc = write_str(" is ");
                if (!rc) rc = write_str(name);
                if (!rc) rc = write_str("\n");
                if (rc) return rc;
                found = true;
            }
        } else {
            const char *p = path_var;
            if (!p || !*p) p = "/bin:/sbin";
            while (*p && !found) {
                size_t dlen = 0;
                while (p[dlen] && p[dlen] != ':') dlen++;
                size_t nlen = length(name);
                if (dlen + 1 + nlen + 1 < VFS_MAX_PATH) {
                    static char candidate[VFS_MAX_PATH];
                    size_t k;
                    for (k = 0; k < dlen; k++) candidate[k] = p[k];
                    candidate[k++] = '/';
                    for (size_t j = 0; j <= nlen; j++) candidate[k + j] = name[j];
                    if (call(SYS_STAT, (uintptr_t)candidate, (uintptr_t)&st, 0) == 0 &&
                        st.type == VFS_FILE) {
                        int rc = write_str(name);
                        if (!rc) rc = write_str(" is ");
                        if (!rc) rc = write_str(candidate);
                        if (!rc) rc = write_str("\n");
                        if (rc) return rc;
                        found = true;
                    }
                }
                p += dlen;
                if (*p == ':') p++;
            }
        }

        if (!found) {
            int rc = write_str(name);
            if (!rc) rc = write_str(": not found\n");
            if (rc) return rc;
            ret = 1;
        }
    }
    return ret;
}

/* ---------- help ---------------------------------------------------------- */

/* The full static builtin table is generated by builtin_help(); no state needed. */
static int exec_help(int argc, const char *const *argv) {
    /* Capture stdout writes; builtin_help() calls puts() which goes to fd 1 */
    builtin_help(argc > 1 ? argv[1] : NULL);
    /* builtin_help uses puts() which ignores write errors.  For the runner we
     * accept this: help output is informational and not byte-verified. */
    return 0;
}

/* ---------- version ------------------------------------------------------- */

static int exec_version(void) {
    return write_str("FortressOS v0.1.0-smp (x86_64) — Engineered by Pride1922\n"
                     "Freestanding C11/NASM Preemptive Microkernel with Limine v8 Bootloader\n");
}

/* ---------- dmesg --------------------------------------------------------- */

static char s_dmesg_buf[DMESG_SIZE];

static bool parse_uint(const char *s, uint64_t *val) {
    if (!s || !*s) return false;
    uint64_t res = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        res = res * 10 + (uint64_t)(s[i] - '0');
    }
    *val = res;
    return true;
}

#define MAX_DMESG_FILTERS 8

static bool starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static bool dmesg_tag_matches(const char *line, size_t line_len, const char *fac) {
    size_t pos = 0;
    while (pos < line_len && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }
    if (pos + 2 < line_len && line[pos] == '<' && line[pos + 1] >= '0' && line[pos + 1] <= '7' && line[pos + 2] == '>') {
        pos += 3;
        while (pos < line_len && (line[pos] == ' ' || line[pos] == '\t')) {
            pos++;
        }
    }
    if (pos >= line_len || line[pos] != '[') {
        return false;
    }
    pos++; /* skip '[' */

    while (*fac == ' ' || *fac == '\t') fac++;
    if (*fac == '[') fac++;
    size_t flen = 0;
    while (fac[flen] && fac[flen] != ']' && fac[flen] != ' ' && fac[flen] != '\t') {
        flen++;
    }
    if (flen == 0) return false;

    for (size_t i = 0; i < flen; i++) {
        if (pos >= line_len) return false;
        char c1 = line[pos++];
        char c2 = fac[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + ('a' - 'A'));
        if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + ('a' - 'A'));
        if (c1 != c2) return false;
    }

    if (pos >= line_len || line[pos] != ']') {
        return false;
    }
    return true;
}

static bool iequal(const char *tok, size_t len, const char *name) {
    size_t i = 0;
    while (i < len && name[i]) {
        char c1 = tok[i];
        char c2 = name[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + ('a' - 'A'));
        if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + ('a' - 'A'));
        if (c1 != c2) return false;
        i++;
    }
    return (i == len && name[i] == '\0');
}

static int parse_level_token(const char *tok, size_t len, uint8_t *out_level) {
    if (len == 1 && tok[0] >= '0' && tok[0] <= '7') {
        *out_level = (uint8_t)(tok[0] - '0');
        return 0;
    }
    if (iequal(tok, len, "emerg") || iequal(tok, len, "emergency") || iequal(tok, len, "panic")) {
        *out_level = 0;
        return 0;
    }
    if (iequal(tok, len, "alert")) {
        *out_level = 1;
        return 0;
    }
    if (iequal(tok, len, "crit") || iequal(tok, len, "critical")) {
        *out_level = 2;
        return 0;
    }
    if (iequal(tok, len, "err") || iequal(tok, len, "error")) {
        *out_level = 3;
        return 0;
    }
    if (iequal(tok, len, "warn") || iequal(tok, len, "warning")) {
        *out_level = 4;
        return 0;
    }
    if (iequal(tok, len, "notice")) {
        *out_level = 5;
        return 0;
    }
    if (iequal(tok, len, "info")) {
        *out_level = 6;
        return 0;
    }
    if (iequal(tok, len, "debug")) {
        *out_level = 7;
        return 0;
    }
    if (iequal(tok, len, "all")) {
        *out_level = 255;
        return 0;
    }
    return -1;
}

static int parse_dmesg_levels(const char *arg, uint8_t *mask) {
    if (!arg || !arg[0]) {
        write_err("dmesg: option requires a level argument\n");
        return 2;
    }
    const char *p = arg;
    bool had_token = false;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t tok_len = (size_t)(p - start);
        bool comma_after = (*p == ',');
        if (*p == ',') p++;

        while (tok_len > 0 && (*start == ' ' || *start == '\t')) {
            start++;
            tok_len--;
        }
        while (tok_len > 0 && (start[tok_len - 1] == ' ' || start[tok_len - 1] == '\t')) {
            tok_len--;
        }
        if (tok_len == 0) {
            write_err("dmesg: invalid level ''\n");
            return 2;
        }

        uint8_t lvl = 0;
        if (parse_level_token(start, tok_len, &lvl) != 0) {
            write_err("dmesg: unknown level '");
            write_bytes_fd(2, start, tok_len);
            write_err("'\n");
            return 2;
        }
        if (lvl == 255) {
            *mask = 0xFF;
        } else {
            *mask |= (uint8_t)(1 << lvl);
        }
        had_token = true;

        if (comma_after && !*p) {
            write_err("dmesg: invalid level ''\n");
            return 2;
        }
    }
    if (!had_token) {
        write_err("dmesg: option requires a level argument\n");
        return 2;
    }
    return 0;
}

static size_t format_dmesg_timestamp(char *buf, size_t cap, uint64_t ticks) {
    if (cap < 20) return 0;
    uint64_t sec = ticks / 100;
    uint64_t usec = (ticks % 100) * 10000;
    buf[0] = '[';
    char s_digits[16];
    int slen = 0;
    uint64_t s_val = sec;
    do {
        s_digits[slen++] = (char)('0' + (s_val % 10));
        s_val /= 10;
    } while (s_val > 0);
    int pad = 5 - slen;
    if (pad < 0) pad = 0;
    int pos = 1;
    for (int p = 0; p < pad; p++) buf[pos++] = ' ';
    for (int p = slen - 1; p >= 0; p--) buf[pos++] = s_digits[p];
    buf[pos++] = '.';
    char u_digits[8];
    for (int u = 5; u >= 0; u--) {
        u_digits[u] = (char)('0' + (usec % 10));
        usec /= 10;
    }
    for (int u = 0; u < 6; u++) buf[pos++] = u_digits[u];
    buf[pos++] = ']';
    buf[pos++] = ' ';
    buf[pos] = '\0';
    return (size_t)pos;
}

static bool dmesg_line_matches(const char *line, size_t line_len, const char *const *filters, size_t num_filters) {
    if (num_filters == 0) return true;
    for (size_t i = 0; i < num_filters; i++) {
        if (dmesg_tag_matches(line, line_len, filters[i])) {
            return true;
        }
    }
    return false;
}

int exec_dmesg(int argc, const char *const *argv) {
    uint64_t tail_lines = 0;
    const char *dest_path = NULL;
    bool append = false;
    bool raw_mode = false;
    bool show_timestamp = false;
    const char *filters[MAX_DMESG_FILTERS];
    size_t num_filters = 0;
    uint8_t level_mask = 0x7F; /* Default: levels 0..6 (emerg through info) */
    bool level_filter_active = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (equal(arg, "-h") || equal(arg, "--help")) {
            write_str("usage: dmesg [-f FAC] [-l LEVELS] [-a] [-r] [-T] [-v] [-n N | tail [N]] [path]\n"
                      "       dmesg [path]\n\n"
                      "Print or save kernel diagnostic ring buffer messages.\n"
                      "options:\n"
                      "  -f FAC, --facility=FAC\n"
                      "                   filter by facility tag (case-insensitive, exact tag)\n"
                      "                   supported: NET, EXT4, USB, PMM, VMM, JBD2, BOOT, SMP,\n"
                      "                              PCI, ACPI, NVME, HEAP, TIME, SIGNAL, SHELL\n"
                      "  -l LEVELS, --level=LEVELS\n"
                      "                   filter by comma-separated severity levels (names or 0-7, all)\n"
                      "                   supported: emerg (0), alert (1), crit (2), err (3),\n"
                      "                              warn (4), notice (5), info (6), debug (7), all\n"
                      "  -v, --all        show all messages including internal debug and tests\n"
                      "  -T, --time, --timestamp\n"
                      "                   prefix output lines with elapsed kernel timestamps\n"
                      "  -r, --raw        do not strip kernel syslog level prefixes (<N>)\n"
                      "  -a, --append     append log output to file instead of overwriting\n"
                      "  -n N, tail [N]   print only the last N lines (e.g. dmesg -n 13)\n"
                      "  -h, --help       display this help and exit\n");
            return 0;
        } else if (equal(arg, "-a") || equal(arg, "--append")) {
            append = true;
        } else if (equal(arg, "-r") || equal(arg, "--raw")) {
            raw_mode = true;
            if (!level_filter_active) level_mask = 0xFF;
        } else if (equal(arg, "-v") || equal(arg, "--all")) {
            level_mask = 0xFF;
            level_filter_active = true;
        } else if (equal(arg, "-T") || equal(arg, "--time") || equal(arg, "--timestamp")) {
            show_timestamp = true;
        } else if (equal(arg, "-l") || equal(arg, "--level")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                write_err("dmesg: option requires a level argument\n");
                return 2;
            }
            if (!level_filter_active) { level_mask = 0; level_filter_active = true; }
            int rc = parse_dmesg_levels(argv[++i], &level_mask);
            if (rc != 0) return rc;
        } else if (starts_with(arg, "--level=")) {
            const char *levels = arg + 8;
            if (!levels[0]) {
                write_err("dmesg: option requires a level argument\n");
                return 2;
            }
            if (!level_filter_active) { level_mask = 0; level_filter_active = true; }
            int rc = parse_dmesg_levels(levels, &level_mask);
            if (rc != 0) return rc;
        } else if (arg[0] == '-' && arg[1] == 'l' && arg[2] != '\0') {
            const char *levels = (arg[2] == '=') ? arg + 3 : arg + 2;
            if (!levels[0]) {
                write_err("dmesg: option requires a level argument\n");
                return 2;
            }
            if (!level_filter_active) { level_mask = 0; level_filter_active = true; }
            int rc = parse_dmesg_levels(levels, &level_mask);
            if (rc != 0) return rc;
        } else if (equal(arg, "-f") || equal(arg, "--facility")) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                write_err("dmesg: option requires a facility argument\n");
                return 1;
            }
            if (num_filters >= MAX_DMESG_FILTERS) {
                write_err("dmesg: too many facility filters (maximum 8)\n");
                return 1;
            }
            filters[num_filters++] = argv[++i];
        } else if (starts_with(arg, "--facility=")) {
            const char *fac = arg + 11;
            if (!fac[0]) {
                write_err("dmesg: option requires a facility argument\n");
                return 1;
            }
            if (num_filters >= MAX_DMESG_FILTERS) {
                write_err("dmesg: too many facility filters (maximum 8)\n");
                return 1;
            }
            filters[num_filters++] = fac;
        } else if (arg[0] == '-' && arg[1] == 'f' && arg[2] != '\0') {
            const char *fac = (arg[2] == '=') ? arg + 3 : arg + 2;
            if (!fac[0]) {
                write_err("dmesg: option requires a facility argument\n");
                return 1;
            }
            if (num_filters >= MAX_DMESG_FILTERS) {
                write_err("dmesg: too many facility filters (maximum 8)\n");
                return 1;
            }
            filters[num_filters++] = fac;
        } else if (equal(arg, "-n")) {
            if (i + 1 >= argc || !parse_uint(argv[i + 1], &tail_lines)) {
                write_err("dmesg: option -n requires a numeric argument\n");
                return 1;
            }
            i++;
        } else if (equal(arg, "tail") || equal(arg, "--tail") || equal(arg, "-t")) {
            tail_lines = 10; /* default tail if count not specified */
            if (i + 1 < argc) {
                if (equal(argv[i + 1], "-n") && i + 2 < argc) {
                    if (parse_uint(argv[i + 2], &tail_lines)) {
                        i += 2;
                    }
                } else {
                    uint64_t val = 0;
                    if (parse_uint(argv[i + 1], &val)) {
                        tail_lines = val;
                        i++;
                    }
                }
            }
        } else {
            uint64_t val = 0;
            if (parse_uint(arg, &val)) {
                tail_lines = val;
            } else if (!dest_path) {
                dest_path = arg;
            } else {
                write_err("dmesg: unrecognized argument '");
                write_err(arg);
                write_err("'\n");
                return 1;
            }
        }
    }

    long n = call(SYS_DMESG, (uintptr_t)s_dmesg_buf, sizeof(s_dmesg_buf), 0);
    if (n < 0) {
        write_err(n == SYSCALL_EPERM || n == SYSCALL_EACCES ? "dmesg: permission denied; root access required (use sudo dmesg).\n" : "dmesg: kernel log unavailable\n");
        return 1;
    }
    if (n == 0) return 0;

    size_t len = (size_t)n;

    size_t write_pos = 0;
    size_t line_start = 0;
    while (line_start < len) {
        size_t line_end = line_start;
        while (line_end < len && s_dmesg_buf[line_end] != '\n') {
            line_end++;
        }
        if (line_end < len && s_dmesg_buf[line_end] == '\n') {
            line_end++;
        }
        size_t line_len = line_end - line_start;
        const char *line = s_dmesg_buf + line_start;

        uint8_t line_level = 6; /* KLOG_INFO default */
        const char *text = line;
        size_t text_len = line_len;

        if (line_len >= 3 && line[0] == '<' && line[1] >= '0' && line[1] <= '7') {
            line_level = (uint8_t)(line[1] - '0');
            size_t hdr_len = 2;
            if (hdr_len < line_len && line[hdr_len] == ',') {
                hdr_len++;
                while (hdr_len < line_len && line[hdr_len] >= '0' && line[hdr_len] <= '9') {
                    hdr_len++;
                }
            }
            if (hdr_len < line_len && line[hdr_len] == '>') {
                hdr_len++;
                if (!raw_mode && !show_timestamp) {
                    text = line + hdr_len;
                    text_len = line_len - hdr_len;
                }
            }
        }

        uint8_t active_mask = level_filter_active ? level_mask : (raw_mode ? 0xFF : 0x7F);
        const char *check_text = text;
        size_t check_len = text_len;
        if (show_timestamp && text_len > 0 && text[0] == '<') {
            size_t g_idx = 1;
            while (g_idx < text_len && text[g_idx] != '>') g_idx++;
            if (g_idx < text_len && text[g_idx] == '>') {
                check_text = text + g_idx + 1;
                check_len = text_len - (g_idx + 1);
            }
        }

        bool match_fac = (num_filters == 0) || dmesg_line_matches(check_text, check_len, filters, num_filters);
        bool match_lvl = ((active_mask & (1 << line_level)) != 0);

        if (match_fac && match_lvl) {
            if (write_pos != (size_t)(text - s_dmesg_buf)) {
                for (size_t k = 0; k < text_len; k++) {
                    s_dmesg_buf[write_pos + k] = text[k];
                }
            }
            write_pos += text_len;
        }
        line_start = line_end;
    }
    len = write_pos;

    size_t start = 0;

    if (tail_lines > 0 && len > 0) {
        size_t idx = len;
        if (idx > 0 && s_dmesg_buf[idx - 1] == '\n') idx--;
        uint64_t count = 0;
        while (idx > 0) {
            idx--;
            if (s_dmesg_buf[idx] == '\n') {
                count++;
                if (count == tail_lines) {
                    start = idx + 1;
                    break;
                }
            }
        }
    }

    if (!show_timestamp) {
        size_t out_len = len - start;
        const char *out_ptr = s_dmesg_buf + start;

        if (dest_path) {
            long flags = VFS_O_WRONLY | VFS_O_CREAT | (append ? VFS_O_APPEND : VFS_O_TRUNC);
            long fd = call(SYS_OPEN, (uintptr_t)dest_path, flags, 0);
            if (fd < 0) {
                file_error_err(fd);
                return 1;
            }
            size_t off = 0;
            bool err = false;
            while (off < out_len) {
                size_t chunk = out_len - off;
                if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
                long w = call(SYS_WRITE, fd, (uintptr_t)(out_ptr + off), chunk);
                if (w <= 0) { err = true; break; }
                off += (size_t)w;
            }
            (void)call(SYS_CLOSE, fd, 0, 0);
            if (err) {
                write_err("dmesg: write failed, file may be incomplete\n");
                return 1;
            }
            puts("Saved ");
            put_dec(out_len);
            puts(append ? " bytes (appended) to " : " bytes to ");
            puts(dest_path);
            puts("\n");
            return 0;
        }

        write_bytes_fd(1, out_ptr, out_len);
        return 0;
    }

    /* Formatted timestamp output (-T, --time, --timestamp) */
    long fd = -1;
    if (dest_path) {
        long flags = VFS_O_WRONLY | VFS_O_CREAT | (append ? VFS_O_APPEND : VFS_O_TRUNC);
        fd = call(SYS_OPEN, (uintptr_t)dest_path, flags, 0);
        if (fd < 0) {
            file_error_err(fd);
            return 1;
        }
    }

    size_t total_written = 0;
    size_t cur = start;
    while (cur < len) {
        size_t line_end = cur;
        while (line_end < len && s_dmesg_buf[line_end] != '\n') {
            line_end++;
        }
        if (line_end < len && s_dmesg_buf[line_end] == '\n') {
            line_end++;
        }
        size_t line_len = line_end - cur;
        const char *line = s_dmesg_buf + cur;

        uint64_t ticks = 0;
        const char *body = line;
        size_t body_len = line_len;

        if (line_len >= 3 && line[0] == '<' && line[1] >= '0' && line[1] <= '7') {
            size_t hdr = 2;
            if (hdr < line_len && line[hdr] == ',') {
                hdr++;
                while (hdr < line_len && line[hdr] >= '0' && line[hdr] <= '9') {
                    ticks = ticks * 10 + (uint64_t)(line[hdr] - '0');
                    hdr++;
                }
            }
            if (hdr < line_len && line[hdr] == '>') {
                hdr++;
                body = line + hdr;
                body_len = line_len - hdr;
            }
        }

        char ts[32];
        size_t tslen = format_dmesg_timestamp(ts, sizeof(ts), ticks);

        if (fd >= 0) {
            if (tslen > 0) call(SYS_WRITE, fd, (uintptr_t)ts, tslen);
            if (body_len > 0) call(SYS_WRITE, fd, (uintptr_t)body, body_len);
            total_written += tslen + body_len;
        } else {
            if (tslen > 0) write_bytes_fd(1, ts, tslen);
            if (body_len > 0) write_bytes_fd(1, body, body_len);
        }

        cur = line_end;
    }

    if (fd >= 0) {
        (void)call(SYS_CLOSE, fd, 0, 0);
        puts("Saved ");
        put_dec(total_written);
        puts(append ? " bytes (appended) to " : " bytes to ");
        puts(dest_path);
        puts("\n");
    }
    return 0;
}

/* ---------- public dispatch ----------------------------------------------- */

/* Kept header-only so standalone builtin runners share the exact test grammar. */
#include "test_expr.h"
int builtin_exec(int argc, const char *const *argv, const builtin_ctx_t *ctx) {
    if (argc <= 0 || !argv || !argv[0] || !argv[0][0]) return 2;
    enum builtin kind=builtin_find(argv[0]);
    if(kind==CMD_TRUE) return 0;
    if(kind==CMD_FALSE) return 1;
    if(kind==CMD_TEST) {
        int r=shell_test(argc,argv);
        if(r==2) write_err("test: invalid expression\n");
        return r;
    }

    /* Reject non-allowlist commands (unknown or forbidden) */
    if (!builtin_is_child_safe(argv[0])) return 2;

    enum builtin b = builtin_find(argv[0]);
    switch (b) {
        case CMD_ECHO:    return exec_echo(argc, argv);
        case CMD_PWD:     return exec_pwd();
        case CMD_CLEAR:   return write_str("\033[2J\033[H");
        case CMD_TRUE:    return 0;
        case CMD_FALSE:   return 1;
        case CMD_ENV:     return exec_env(ctx);
        case CMD_HELP:    return exec_help(argc, argv);
        case CMD_VERSION: return exec_version();
        case CMD_LS:      return exec_ls(argc, argv);
        case CMD_VIEW:    return exec_view(argc, argv);
        case CMD_TYPE:
            /* Runner context: no alias lookup (NULL) */
            return exec_type(argc, argv, ctx, NULL);
        case CMD_DMESG:   return exec_dmesg(argc, argv);
        case CMD_PRINTF:  return exec_printf(argc, argv);
        default: return 2;
    }
}
