/* Host unit tests for which and env tools under ASan/UBSan */
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"

static char s_out_buf[16384];
static size_t s_out_len = 0;
static char s_err_buf[4096];
static size_t s_err_len = 0;

static const char *s_spawned_cmd = NULL;
static const char *const *s_spawned_argv = NULL;
static const char *const *s_spawned_envp = NULL;

void reset_mock(void) {
    s_out_len = 0;
    s_out_buf[0] = '\0';
    s_err_len = 0;
    s_err_buf[0] = '\0';
    s_spawned_cmd = NULL;
    s_spawned_argv = NULL;
    s_spawned_envp = NULL;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)c;
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const void *buf = (const void *)b;
        size_t n = (size_t)c;
        if (fd == 1) {
            if (s_out_len + n < sizeof(s_out_buf)) {
                memcpy(s_out_buf + s_out_len, buf, n);
                s_out_len += n;
                s_out_buf[s_out_len] = '\0';
            }
            return (long)n;
        } else if (fd == 2) {
            if (s_err_len + n < sizeof(s_err_buf)) {
                memcpy(s_err_buf + s_err_len, buf, n);
                s_err_len += n;
                s_err_buf[s_err_len] = '\0';
            }
            return (long)n;
        }
        return (long)n;
    }
    if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (!path || !st) return SYSCALL_EINVAL;
        if (strcmp(path, "/bin/cat") == 0 ||
            strcmp(path, "/bin/ls") == 0 ||
            strcmp(path, "/sbin/ifconfig") == 0 ||
            strcmp(path, "/usr/bin/cat") == 0 ||
            strcmp(path, "/opt/myapp") == 0) {
            memset(st, 0, sizeof(*st));
            st->type = VFS_FILE;
            return 0;
        }
        return SYSCALL_ENOENT;
    }
    if (nr == SYS_SPAWN_EXT) {
        s_spawned_cmd = (const char *)a;
        const spawn_opts_t *opts = (const spawn_opts_t *)b;
        if (opts) {
            s_spawned_argv = (const char *const *)opts->argv;
            s_spawned_envp = (const char *const *)opts->envp;
        }
        return 42; /* mock pid */
    }
    if (nr == SYS_WAIT) {
        uint64_t *status = (uint64_t *)b;
        if (status) *status = 0;
        return 0;
    }
    return 0;
}

static void print_msg(const char *s) {
    (void)write(1, s, strlen(s));
}

static void test_which(void) {
    const char *envp[] = {"PATH=/bin:/sbin:/usr/bin", NULL};

    /* 1. which cat */
    reset_mock();
    char *argv1[] = {"which", "cat", NULL};
    int rc = which_main(2, argv1, envp);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "/bin/cat\n") == 0);

    /* 2. which ifconfig */
    reset_mock();
    char *argv2[] = {"which", "ifconfig", NULL};
    rc = which_main(2, argv2, envp);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "/sbin/ifconfig\n") == 0);

    /* 3. which -a cat (multiple PATH matches) */
    reset_mock();
    char *argv3[] = {"which", "-a", "cat", NULL};
    rc = which_main(3, argv3, envp);
    assert(rc == 0);
    assert(strstr(s_out_buf, "/bin/cat\n") != NULL);
    assert(strstr(s_out_buf, "/usr/bin/cat\n") != NULL);

    /* 4. which direct path /opt/myapp */
    reset_mock();
    char *argv4[] = {"which", "/opt/myapp", NULL};
    rc = which_main(2, argv4, envp);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "/opt/myapp\n") == 0);

    /* 5. which nonexistent */
    reset_mock();
    char *argv5[] = {"which", "nonexistent_tool", NULL};
    rc = which_main(2, argv5, envp);
    assert(rc == 1);
    assert(strstr(s_out_buf, "no nonexistent_tool in (/bin:/sbin:/usr/bin)") != NULL);

    /* 6. which --help */
    reset_mock();
    char *argv6[] = {"which", "--help", NULL};
    rc = which_main(2, argv6, envp);
    assert(rc == 0);
    assert(strstr(s_out_buf, "Usage: which") != NULL);

    print_msg("PASS: which (PATH search, -a all matches, direct path, missing tool, --help)\n");
}

static void test_env(void) {
    const char *envp[] = {"PATH=/bin:/sbin", "USER=root", "TERM=fortress", NULL};

    /* 1. env with no arguments (dump environment) */
    reset_mock();
    char *argv1[] = {"env", NULL};
    int rc = env_main(1, argv1, envp);
    assert(rc == 0);
    assert(strstr(s_out_buf, "PATH=/bin:/sbin\n") != NULL);
    assert(strstr(s_out_buf, "USER=root\n") != NULL);
    assert(strstr(s_out_buf, "TERM=fortress\n") != NULL);

    /* 2. env FOO=bar BAZ=123 (dump modified environment) */
    reset_mock();
    char *argv2[] = {"env", "FOO=bar", "USER=alice", NULL};
    rc = env_main(3, argv2, envp);
    assert(rc == 0);
    assert(strstr(s_out_buf, "FOO=bar\n") != NULL);
    assert(strstr(s_out_buf, "USER=alice\n") != NULL);
    assert(strstr(s_out_buf, "USER=root\n") == NULL); /* overwritten */

    /* 3. env -i FOO=bar (ignore environment) */
    reset_mock();
    char *argv3[] = {"env", "-i", "FOO=bar", NULL};
    rc = env_main(3, argv3, envp);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "FOO=bar\n") == 0);

    /* 4. env VAR=val cat arg1 (execute command) */
    reset_mock();
    char *argv4[] = {"env", "MYVAR=42", "cat", "file.txt", NULL};
    rc = env_main(4, argv4, envp);
    assert(rc == 0);
    assert(s_spawned_cmd != NULL && strcmp(s_spawned_cmd, "/bin/cat") == 0);
    assert(s_spawned_argv != NULL && strcmp(s_spawned_argv[0], "cat") == 0);
    assert(s_spawned_argv[1] != NULL && strcmp(s_spawned_argv[1], "file.txt") == 0);

    bool found_myvar = false;
    for (int i = 0; s_spawned_envp && s_spawned_envp[i]; i++) {
        if (strcmp(s_spawned_envp[i], "MYVAR=42") == 0) found_myvar = true;
    }
    assert(found_myvar);

    /* 5. env missing_tool */
    reset_mock();
    char *argv5[] = {"env", "missing_tool", NULL};
    rc = env_main(2, argv5, envp);
    assert(rc == 127);
    assert(strstr(s_err_buf, "command not found") != NULL);

    /* 6. env --help */
    reset_mock();
    char *argv6[] = {"env", "--help", NULL};
    rc = env_main(2, argv6, envp);
    assert(rc == 0);
    assert(strstr(s_out_buf, "Usage: env") != NULL);

    print_msg("PASS: env (dump env, key=val override/append, -i, command spawn, missing tool, --help)\n");
}

int main(void) {
    test_which();
    test_env();
    print_msg("ALL WHICH & ENV HOST TESTS PASSED!\n");
    return 0;
}
