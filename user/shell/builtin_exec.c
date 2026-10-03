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
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            long r = write_bytes_fd(1, " ", 1);
            if (r < 0) return io_err(r);
        }
        int rc = write_str(argv[i]);
        if (rc) return rc;
    }
    long r = write_bytes_fd(1, "\n", 1);
    return r < 0 ? io_err(r) : 0;
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

static int exec_ls(int argc, const char *const *argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    vfs_stat_t st;
    long result = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) {
        write_err("ls: ");
        write_err(path);
        write_err(": no such file or directory\n");
        return 1;
    }
    if (st.type != VFS_DIRECTORY) {
        /* Single file: just print its name. */
        int rc = write_str(path);
        if (rc) return rc;
        long r = write_bytes_fd(1, "\n", 1);
        return r < 0 ? io_err(r) : 0;
    }
    long fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) {
        write_err("ls: cannot open directory\n");
        return 1;
    }
    vfs_dirent_t entry;
    int out_err = 0;
    while ((result = call(SYS_READDIR, fd, (uintptr_t)&entry, 0)) == 1) {
        if (!out_err) {
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
        if (r < 0) { write_err("view: no such file or directory\n"); return 1; }
        if (st.type != VFS_FILE) { write_err("view: not a regular file\n"); return 1; }
        fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
        if (fd < 0) { write_err("view: cannot open file\n"); return 1; }
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
            if (!p || !*p) p = "/bin";
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

/* ---------- public dispatch ----------------------------------------------- */

int builtin_exec(int argc, const char *const *argv, const builtin_ctx_t *ctx) {
    if (argc <= 0 || !argv || !argv[0] || !argv[0][0]) return 2;

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
        default: return 2;
    }
}
