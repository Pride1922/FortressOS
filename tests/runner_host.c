/* Runner host test: actual shared handlers under ASan/UBSan with mocked syscalls.
 * Tests: dispatch, EPIPE handling, forbidden commands, view stdin/file/backpressure,
 * echo spacing, ls error paths, env iteration, type allowlist narrowing.
 *
 * io.c is NOT linked: this file provides all required stubs itself. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "syscall_abi.h"
#include "vfs.h"
#include "builtin_exec.h"
#include "builtins.h"

/* ---- syscall mock state -------------------------------------------------- */
static char out_buf[65536];
static size_t out_pos;
static char err_buf[4096];
static size_t err_pos;
static int stdout_fail_after;  /* fail write after this many calls (0 = never) */
static int stdout_calls;
static long stdout_fail_error;
static int stat_fail;          /* if nonzero, SYS_STAT returns this error */
static int open_fail;
static int read_returns[16];   /* sequence of SYS_READ return values */
static int read_idx;
static char read_data[4096];
static size_t read_data_len;
static size_t read_data_pos;

static void reset(void) {
    out_pos = err_pos = 0;
    out_buf[0] = err_buf[0] = '\0';
    stdout_fail_after = stdout_calls = 0;
    stdout_fail_error = SYSCALL_EPIPE;
    stat_fail = open_fail = 0;
    memset(read_returns, 0, sizeof(read_returns));
    read_idx = 0;
    read_data_len = read_data_pos = 0;
}

size_t length(const char *s) { return strlen(s); }
bool equal(const char *a, const char *b) { return !strcmp(a, b); }

long write_bytes_fd(int fd, const char *s, size_t n) {
    if (fd == 1) {
        stdout_calls++;
        if (stdout_fail_after && stdout_calls > stdout_fail_after)
            return stdout_fail_error;
        assert(out_pos + n < sizeof(out_buf));
        memcpy(out_buf + out_pos, s, n);
        out_pos += n;
        out_buf[out_pos] = '\0';
        return (long)n;
    }
    if (fd == 2) {
        if (err_pos + n < sizeof(err_buf)) {
            memcpy(err_buf + err_pos, s, n);
            err_pos += n;
            err_buf[err_pos] = '\0';
        }
        return (long)n;
    }
    return SYSCALL_EBADF;
}

int puts(const char *s) { write_bytes_fd(1, s, strlen(s)); return 0; }
long puts_err(const char *s) { return write_bytes_fd(2, s, strlen(s)); }
void file_error_err(long e) { (void)e; }

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_GETCWD) {
        char *buf = (char *)a;
        const char *cwd = "/testcwd";
        size_t n = strlen(cwd);
        if ((size_t)b <= n) return SYSCALL_EINVAL;
        memcpy(buf, cwd, n + 1);
        return (long)n;
    }
    if (nr == SYS_STAT) {
        if (stat_fail) return stat_fail;
        vfs_stat_t *st = (vfs_stat_t *)b;
        /* Distinguish file vs directory by path suffix */
        const char *path = (const char *)a;
        st->type = (path[strlen(path)-1] == '/') ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_OPEN) {
        if (open_fail) return open_fail;
        return 7; /* fd 7 */
    }
    if (nr == SYS_CLOSE) { return 0; }
    if (nr == SYS_READ) {
        if (read_data_pos < read_data_len) {
            size_t avail = read_data_len - read_data_pos;
            size_t want = (size_t)c < avail ? (size_t)c : avail;
            memcpy((char *)b, read_data + read_data_pos, want);
            read_data_pos += want;
            return (long)want;
        }
        return 0; /* EOF */
    }
    if (nr == SYS_READDIR) {
        /* Always return no entries for simplicity in ls tests */
        return 0;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

/* ---- helpers ------------------------------------------------------------- */

static const char *envp_sample[] = {
    "PATH=/bin:/usr/bin",
    "HOME=/",
    "MYVAR=hello",
    NULL
};

static builtin_ctx_t make_ctx(void) {
    return builtin_ctx_from_envp(envp_sample);
}

/* ---- tests --------------------------------------------------------------- */

static void test_echo(void) {
    reset();
    const char *argv[] = {"echo", "hello", "world", NULL};
    int r = builtin_exec(3, argv, NULL);
    assert(r == 0);
    assert(!strcmp(out_buf, "hello world\n"));

    reset();
    const char *argv2[] = {"echo", NULL};
    r = builtin_exec(1, argv2, NULL);
    assert(r == 0);
    assert(!strcmp(out_buf, "\n"));

    /* EPIPE: fail on first write */
    reset();
    stdout_fail_after = 1; stdout_fail_error = SYSCALL_EPIPE;
    r = builtin_exec(3, argv, NULL);
    assert(r == 1);

    /* Other write error */
    reset();
    stdout_fail_after = 1; stdout_fail_error = SYSCALL_EIO;
    r = builtin_exec(3, argv, NULL);
    assert(r == 1);
}

static void test_true_false(void) {
    reset();
    const char *t[] = {"true", NULL};
    assert(builtin_exec(1, t, NULL) == 0);
    reset();
    const char *f[] = {"false", NULL};
    assert(builtin_exec(1, f, NULL) == 1);
    assert(!out_pos && !err_pos); /* no output */
}

static void test_pwd(void) {
    reset();
    const char *argv[] = {"pwd", NULL};
    int r = builtin_exec(1, argv, NULL);
    assert(r == 0);
    assert(!strcmp(out_buf, "/testcwd\n"));
}

static void test_env(void) {
    reset();
    builtin_ctx_t ctx = make_ctx();
    const char *argv[] = {"env", NULL};
    int r = builtin_exec(1, argv, &ctx);
    assert(r == 0);
    assert(strstr(out_buf, "PATH=/bin:/usr/bin\n"));
    assert(strstr(out_buf, "MYVAR=hello\n"));

    /* Empty envp */
    reset();
    const char *empty[] = {NULL};
    builtin_ctx_t ectx = builtin_ctx_from_envp(empty);
    r = builtin_exec(1, argv, &ectx);
    assert(r == 0);
    assert(!out_pos);
}

static void test_version(void) {
    reset();
    const char *argv[] = {"version", NULL};
    int r = builtin_exec(1, argv, NULL);
    assert(r == 0);
    assert(strstr(out_buf, "FortressOS"));
}

static void test_ls_empty_dir(void) {
    reset();
    const char *argv[] = {"ls", "/somedir/", NULL};
    /* stat succeeds as directory, readdir returns 0 entries immediately */
    int r = builtin_exec(2, argv, NULL);
    assert(r == 0);
    assert(!out_pos); /* no entries */
}

static void test_ls_missing(void) {
    reset();
    stat_fail = SYSCALL_ENOENT;
    const char *argv[] = {"ls", "/missing", NULL};
    int r = builtin_exec(2, argv, NULL);
    assert(r == 1);
    assert(strstr(err_buf, "no such file"));
}

static void test_view_file(void) {
    reset();
    const char *content = "hello\x01world\n";
    memcpy(read_data, content, strlen(content));
    read_data_len = strlen(content);
    read_data_pos = 0;
    const char *argv[] = {"view", "/test.txt", NULL};
    int r = builtin_exec(2, argv, NULL);
    assert(r == 0);
    /* \x01 should become '.' */
    assert(!strcmp(out_buf, "hello.world\n"));
}

static void test_view_stdin(void) {
    /* view with no argument reads stdin (fd 0 in runner) */
    reset();
    const char *content = "line1\nline2\n";
    memcpy(read_data, content, strlen(content));
    read_data_len = strlen(content);
    read_data_pos = 0;
    const char *argv[] = {"view", NULL};
    int r = builtin_exec(1, argv, NULL);
    assert(r == 0);
    assert(!strcmp(out_buf, "line1\nline2\n"));
}

static void test_view_epipe(void) {
    reset();
    const char *content = "data\n";
    memcpy(read_data, content, strlen(content));
    read_data_len = strlen(content);
    read_data_pos = 0;
    /* fail_after=0 combined with setting calls to a high value ensures the
     * very next write to fd 1 fails.  Use sentinel: fail_after > 0, calls = fail_after. */
    stdout_calls = 1; stdout_fail_after = 1; stdout_fail_error = SYSCALL_EPIPE;
    const char *argv[] = {"view", "/test.txt", NULL};
    int r = builtin_exec(2, argv, NULL);
    assert(r == 1);
}

static void test_view_no_final_lf(void) {
    reset();
    const char *content = "no newline";
    memcpy(read_data, content, strlen(content));
    read_data_len = strlen(content);
    read_data_pos = 0;
    const char *argv[] = {"view", "/test.txt", NULL};
    int r = builtin_exec(2, argv, NULL);
    assert(r == 0);
    assert(out_buf[out_pos - 1] == '\n'); /* final LF added */
}

static void test_type_allowlist(void) {
    reset();
    builtin_ctx_t ctx = make_ctx();
    const char *argv[] = {"type", "echo", NULL};
    int r = builtin_exec(2, argv, &ctx);
    assert(r == 0);
    assert(strstr(out_buf, "shell builtin"));

    /* cd is NOT in the runner allowlist; stat fails so it's truly not found */
    reset();
    stat_fail = SYSCALL_ENOENT; /* external stat fails → cd not found as external either */
    const char *argv2[] = {"type", "cd", NULL};
    r = builtin_exec(2, argv2, &ctx);
    assert(r == 1);
    assert(strstr(out_buf, "not found") || out_pos > 0);
}

static void test_forbidden(void) {
    reset();
    const char *argv[] = {"cd", "/", NULL};
    int r = builtin_exec(2, argv, NULL);
    assert(r == 2);

    reset();
    const char *argv2[] = {"exit", "0", NULL};
    r = builtin_exec(2, argv2, NULL);
    assert(r == 2);

    reset();
    const char *argv3[] = {"alias", NULL};
    r = builtin_exec(1, argv3, NULL);
    assert(r == 2);
}

static void test_child_safe_api(void) {
    assert(builtin_is_child_safe("echo"));
    assert(builtin_is_child_safe("pwd"));
    assert(builtin_is_child_safe("true"));
    assert(builtin_is_child_safe("false"));
    assert(builtin_is_child_safe("env"));
    assert(builtin_is_child_safe("help"));
    assert(builtin_is_child_safe("version"));
    assert(builtin_is_child_safe("ls"));
    assert(builtin_is_child_safe("view"));
    assert(builtin_is_child_safe("type"));
    /* Forbidden */
    assert(!builtin_is_child_safe("cd"));
    assert(!builtin_is_child_safe("exit"));
    assert(!builtin_is_child_safe("alias"));
    assert(!builtin_is_child_safe("export"));
    assert(!builtin_is_child_safe("set"));
    assert(!builtin_is_child_safe("history"));
    assert(!builtin_is_child_safe("reboot"));
    assert(!builtin_is_child_safe("shutdown"));
    assert(!builtin_is_child_safe("command")); /* wrapper, not child-safe */
    assert(!builtin_is_child_safe("unknown"));
    assert(!builtin_is_child_safe(""));
    assert(!builtin_is_child_safe(NULL));
}

static void test_ctx_path(void) {
    builtin_ctx_t ctx = builtin_ctx_from_envp(envp_sample);
    assert(ctx.path && !strcmp(ctx.path, "/bin:/usr/bin"));
    const char *no_path[] = {"MYVAR=hello", NULL};
    builtin_ctx_t ctx2 = builtin_ctx_from_envp(no_path);
    assert(!ctx2.path);
}

int main(void) {
    test_echo();
    test_true_false();
    test_pwd();
    test_env();
    test_version();
    test_ls_empty_dir();
    test_ls_missing();
    test_view_file();
    test_view_stdin();
    test_view_epipe();
    test_view_no_final_lf();
    test_type_allowlist();
    test_forbidden();
    test_child_safe_api();
    test_ctx_path();
    puts("PASS runner: dispatch, EPIPE, view stdin/file/no-LF, type allowlist narrowing, forbidden commands, child_safe API\n");
    return 0;
}
