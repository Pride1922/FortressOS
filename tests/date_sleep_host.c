#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

extern int date_main(int argc, char **argv);
extern int sleep_main(int argc, char **argv);

static timespec_t mock_realtime;
static long mock_clock_ret = 0;

static timespec_t mock_sleep_req;
static timespec_t mock_sleep_rem;
static long mock_sleep_ret = 0;

static char mock_stdout[4096];
static size_t mock_stdout_len = 0;
static char mock_stderr[4096];
static size_t mock_stderr_len = 0;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)c;
    if (nr == SYS_CLOCK_GETTIME) {
        if (mock_clock_ret < 0) return mock_clock_ret;
        if (a == CLOCK_REALTIME) {
            timespec_t *out = (timespec_t *)b;
            if (out) *out = mock_realtime;
            return 0;
        }
        return SYSCALL_EINVAL;
    }
    if (nr == SYS_NANOSLEEP) {
        if (a) mock_sleep_req = *(const timespec_t *)a;
        if (b) *(timespec_t *)b = mock_sleep_rem;
        return mock_sleep_ret;
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

static void reset_buffers(void) {
    mock_stdout_len = 0;
    mock_stdout[0] = '\0';
    mock_stderr_len = 0;
    mock_stderr[0] = '\0';
    mock_clock_ret = 0;
    mock_sleep_ret = 0;
    memset(&mock_sleep_req, 0, sizeof(mock_sleep_req));
    memset(&mock_sleep_rem, 0, sizeof(mock_sleep_rem));
}

static void test_date_tool(void) {
    printf("Testing /bin/date tool...\n");

    /* 2026-10-16 08:30:45 UTC */
    mock_realtime.tv_sec = 1792139445;
    mock_realtime.tv_nsec = 123456789;

    /* 1. Default date output */
    reset_buffers();
    char *argv_default[] = {"date", NULL};
    int ret = date_main(1, argv_default);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri Oct 16 08:30:45 UTC 2026\n") == 0);

    /* 2. ISO-8601 mode */
    reset_buffers();
    char *argv_iso[] = {"date", "-I", NULL};
    ret = date_main(2, argv_iso);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16T08:30:45Z\n") == 0);

    /* 3. RFC-2822 mode */
    reset_buffers();
    char *argv_rfc[] = {"date", "-R", NULL};
    ret = date_main(2, argv_rfc);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri, 16 Oct 2026 08:30:45 +0000\n") == 0);

    /* 4. Seconds since epoch (+%s) */
    reset_buffers();
    char *argv_epoch[] = {"date", "+%s", NULL};
    ret = date_main(2, argv_epoch);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "1792139445\n") == 0);

    /* 5. Help */
    reset_buffers();
    char *argv_help[] = {"date", "--help", NULL};
    ret = date_main(2, argv_help);
    assert(ret == 0);
    assert(strstr(mock_stdout, "usage: date") != NULL);

    /* 6. Invalid option */
    reset_buffers();
    char *argv_bad[] = {"date", "--bogus", NULL};
    ret = date_main(2, argv_bad);
    assert(ret != 0);

    printf("  [PASS] date tool verified.\n");
}

static void test_sleep_tool(void) {
    printf("Testing /bin/sleep tool...\n");

    /* 1. Basic integer seconds */
    reset_buffers();
    char *argv1[] = {"sleep", "2", NULL};
    int ret = sleep_main(2, argv1);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 2);
    assert(mock_sleep_req.tv_nsec == 0);

    /* 2. Fractional seconds */
    reset_buffers();
    char *argv2[] = {"sleep", "0.5", NULL};
    ret = sleep_main(2, argv2);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 0);
    assert(mock_sleep_req.tv_nsec == 500000000L);

    /* 3. Suffix minutes (1.5m = 90s) */
    reset_buffers();
    char *argv3[] = {"sleep", "1.5m", NULL};
    ret = sleep_main(2, argv3);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 90);
    assert(mock_sleep_req.tv_nsec == 0);

    /* 4. Multiple intervals */
    reset_buffers();
    char *argv4[] = {"sleep", "1", "0.25", NULL};
    ret = sleep_main(3, argv4);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 1);
    assert(mock_sleep_req.tv_nsec == 250000000L);

    /* 5. Help */
    reset_buffers();
    char *argv_help[] = {"sleep", "--help", NULL};
    ret = sleep_main(2, argv_help);
    assert(ret == 0);
    assert(strstr(mock_stdout, "usage: sleep") != NULL);

    /* 6. Missing operand */
    reset_buffers();
    char *argv_none[] = {"sleep", NULL};
    ret = sleep_main(1, argv_none);
    assert(ret != 0);

    /* 7. Invalid interval */
    reset_buffers();
    char *argv_invalid[] = {"sleep", "xyz", NULL};
    ret = sleep_main(2, argv_invalid);
    assert(ret != 0);

    /* 8. Interrupted by signal */
    reset_buffers();
    mock_sleep_ret = SYSCALL_EINTR;
    char *argv_intr[] = {"sleep", "5", NULL};
    ret = sleep_main(2, argv_intr);
    assert(ret == 130);

    printf("  [PASS] sleep tool verified.\n");
}

int main(void) {
    printf("=== Date & Sleep Userland Tool Host Tests ===\n");
    test_date_tool();
    test_sleep_tool();
    printf(">>> ALL DATE & SLEEP HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
