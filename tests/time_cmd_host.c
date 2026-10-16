#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "syscall_abi.h"

extern size_t time_format_default(char *buf, size_t max, int64_t ns);
extern size_t time_format_posix(char *buf, size_t max, int64_t ns);
extern int time_main(int argc, char **argv, char **envp);

static timespec_t mock_t0;
static timespec_t mock_t1;
static int mock_clock_call_count = 0;

static tms_t mock_tms;
static long mock_spawn_ret = 42;
static uint64_t mock_wait_status = 0;
static int mock_wait_nowait_calls = 0;
static int mock_wait_reap_calls = 0;

static char mock_stdout[4096];
static size_t mock_stdout_len = 0;
static char mock_stderr[4096];
static size_t mock_stderr_len = 0;

static void reset_mocks(void) {
    mock_t0 = (timespec_t){.tv_sec = 10, .tv_nsec = 0};
    mock_t1 = (timespec_t){.tv_sec = 11, .tv_nsec = 234000000L};
    mock_clock_call_count = 0;

    mock_tms = (tms_t){
        .tms_utime_ns = 891000000LL,
        .tms_stime_ns = 102000000LL,
        .tms_cutime_ns = 0,
        .tms_cstime_ns = 0
    };
    mock_spawn_ret = 42;
    mock_wait_status = 0;
    mock_wait_nowait_calls = 0;
    mock_wait_reap_calls = 0;

    mock_stdout_len = 0;
    mock_stdout[0] = '\0';
    mock_stderr_len = 0;
    mock_stderr[0] = '\0';
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_CLOCK_GETTIME) {
        timespec_t *out = (timespec_t *)b;
        if (out) {
            if (mock_clock_call_count++ == 0) {
                *out = mock_t0;
            } else {
                *out = mock_t1;
            }
        }
        return 0;
    }
    if (nr == SYS_SPAWN_EXT) {
        return mock_spawn_ret;
    }
    if (nr == SYS_WAITPID) {
        uint64_t *status = (uint64_t *)b;
        uint32_t options = (uint32_t)c;
        if (status) *status = mock_wait_status;
        if (options & WNOWAIT) {
            mock_wait_nowait_calls++;
        } else {
            mock_wait_reap_calls++;
        }
        return mock_spawn_ret;
    }
    if (nr == SYS_TIMES) {
        tms_t *out = (tms_t *)b;
        if (out) *out = mock_tms;
        return 0;
    }
    if (nr == SYS_STAT) {
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (st) {
            st->type = VFS_FILE;
            st->size = 1024;
        }
        return 0;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *data = (const char *)b;
        size_t len = (size_t)c;
        if (fd == 1) {
            if (mock_stdout_len + len < sizeof(mock_stdout)) {
                memcpy(mock_stdout + mock_stdout_len, data, len);
                mock_stdout_len += len;
                mock_stdout[mock_stdout_len] = '\0';
            }
            return (long)len;
        }
        if (fd == 2) {
            if (mock_stderr_len + len < sizeof(mock_stderr)) {
                memcpy(mock_stderr + mock_stderr_len, data, len);
                mock_stderr_len += len;
                mock_stderr[mock_stderr_len] = '\0';
            }
            return (long)len;
        }
    }
    return SYSCALL_ENOSYS;
}

static void test_formatting(void) {
    printf("Testing duration formatting (default and POSIX)...\n");
    char buf[64];

    /* Zero and edge cases */
    time_format_default(buf, sizeof(buf), 0);
    assert(strcmp(buf, "0m0.000s") == 0);
    time_format_posix(buf, sizeof(buf), 0);
    assert(strcmp(buf, "0.00") == 0);

    time_format_default(buf, sizeof(buf), -100);
    assert(strcmp(buf, "0m0.000s") == 0);
    time_format_posix(buf, sizeof(buf), -100);
    assert(strcmp(buf, "0.00") == 0);

    /* Sub-second durations */
    time_format_default(buf, sizeof(buf), 1000000LL); /* 1 ms */
    assert(strcmp(buf, "0m0.001s") == 0);
    time_format_posix(buf, sizeof(buf), 1000000LL); /* 0.001s -> 0.00 */
    assert(strcmp(buf, "0.00") == 0);

    time_format_default(buf, sizeof(buf), 10000000LL); /* 10 ms = 0.01s */
    assert(strcmp(buf, "0m0.010s") == 0);
    time_format_posix(buf, sizeof(buf), 10000000LL);
    assert(strcmp(buf, "0.01") == 0);

    time_format_default(buf, sizeof(buf), 891000000LL);
    assert(strcmp(buf, "0m0.891s") == 0);
    time_format_posix(buf, sizeof(buf), 891000000LL);
    assert(strcmp(buf, "0.89") == 0);

    /* 1 to 59 seconds */
    time_format_default(buf, sizeof(buf), 1234000000LL);
    assert(strcmp(buf, "0m1.234s") == 0);
    time_format_posix(buf, sizeof(buf), 1234000000LL);
    assert(strcmp(buf, "1.23") == 0);

    time_format_default(buf, sizeof(buf), 59999000000LL);
    assert(strcmp(buf, "0m59.999s") == 0);
    time_format_posix(buf, sizeof(buf), 59999000000LL);
    assert(strcmp(buf, "59.99") == 0);

    /* Minutes and multi-minutes */
    time_format_default(buf, sizeof(buf), 60000000000LL);
    assert(strcmp(buf, "1m0.000s") == 0);
    time_format_posix(buf, sizeof(buf), 60000000000LL);
    assert(strcmp(buf, "60.00") == 0);

    time_format_default(buf, sizeof(buf), 72345000000LL);
    assert(strcmp(buf, "1m12.345s") == 0);
    time_format_posix(buf, sizeof(buf), 72345000000LL);
    assert(strcmp(buf, "72.34") == 0);

    time_format_default(buf, sizeof(buf), 3661002000000LL); /* 61 min 1 sec 2 ms */
    assert(strcmp(buf, "61m1.002s") == 0);
    time_format_posix(buf, sizeof(buf), 3661002000000LL);
    assert(strcmp(buf, "3661.00") == 0);

    printf("  [PASS] Formatting verified.\n");
}

static void test_cli_parsing(void) {
    printf("Testing argument parsing and error handling...\n");

    /* No arguments */
    reset_mocks();
    char *argv_no_args[] = {"time", NULL};
    int ret = time_main(1, argv_no_args, NULL);
    assert(ret == 2);
    assert(strstr(mock_stderr, "missing command") != NULL);

    /* -p without command */
    reset_mocks();
    char *argv_p_no_cmd[] = {"time", "-p", NULL};
    ret = time_main(2, argv_p_no_cmd, NULL);
    assert(ret == 2);
    assert(strstr(mock_stderr, "missing command") != NULL);

    /* --help */
    reset_mocks();
    char *argv_help[] = {"time", "--help", NULL};
    ret = time_main(2, argv_help, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "Usage: time") != NULL);

    /* -h */
    reset_mocks();
    char *argv_h[] = {"time", "-h", NULL};
    ret = time_main(2, argv_h, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "Usage: time") != NULL);

    /* Unrecognized option */
    reset_mocks();
    char *argv_bad[] = {"time", "-z", "ls", NULL};
    ret = time_main(3, argv_bad, NULL);
    assert(ret == 2);
    assert(strstr(mock_stderr, "unrecognized option '-z'") != NULL);

    printf("  [PASS] CLI parsing verified.\n");
}

static void test_execution(void) {
    printf("Testing execution and output generation...\n");

    /* Default output */
    reset_mocks();
    char *argv_run[] = {"time", "sha256sum", "/mnt/bigfile", NULL};
    int ret = time_main(3, argv_run, NULL);
    assert(ret == 0);
    assert(mock_wait_nowait_calls == 1);
    assert(mock_wait_reap_calls == 1);
    assert(strstr(mock_stderr, "real\t0m1.234s\n") != NULL);
    assert(strstr(mock_stderr, "user\t0m0.891s\n") != NULL);
    assert(strstr(mock_stderr, "sys\t0m0.102s\n") != NULL);

    /* POSIX -p output */
    reset_mocks();
    char *argv_p_run[] = {"time", "-p", "sha256sum", "/mnt/bigfile", NULL};
    ret = time_main(4, argv_p_run, NULL);
    assert(ret == 0);
    assert(mock_wait_nowait_calls == 1);
    assert(mock_wait_reap_calls == 1);
    assert(strstr(mock_stderr, "real 1.23\n") != NULL);
    assert(strstr(mock_stderr, "user 0.89\n") != NULL);
    assert(strstr(mock_stderr, "sys 0.10\n") != NULL);

    /* Exit code propagation: normal exit code 7 */
    reset_mocks();
    mock_wait_status = (7u << 8); /* WIFEXITED == true, WEXITSTATUS == 7 */
    char *argv_exit[] = {"time", "mycmd", NULL};
    ret = time_main(2, argv_exit, NULL);
    assert(ret == 7);

    /* Exit code propagation: signal exit (SIGKILL = 9) */
    reset_mocks();
    mock_wait_status = 9u; /* killed by SIGKILL */
    ret = time_main(2, argv_exit, NULL);
    assert(ret == 128 + 9);

    /* Spawn failure (ENOENT -> exit 127) */
    reset_mocks();
    mock_spawn_ret = SYSCALL_ENOENT;
    ret = time_main(2, argv_exit, NULL);
    assert(ret == 127);
    assert(strstr(mock_stderr, "no such file or directory:") != NULL);

    /* Double dash handling */
    reset_mocks();
    char *argv_dd[] = {"time", "-p", "--", "-weird-cmd-name", NULL};
    ret = time_main(4, argv_dd, NULL);
    assert(ret == 0);
    assert(strstr(mock_stderr, "real 1.23\n") != NULL);

    printf("  [PASS] Execution and status propagation verified.\n");
}

int main(void) {
    printf("=== FortressOS /bin/time Host Unit Tests ===\n");
    test_formatting();
    test_cli_parsing();
    test_execution();
    printf(">>> ALL TIME COMMAND HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
