/* Host unit test for ps -o columns under ASan/UBSan */
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "types.h"
#include "syscall_abi.h"

static char s_out_buf[16384];
static size_t s_out_len = 0;
static char s_err_buf[4096];
static size_t s_err_len = 0;

static proc_info_t s_mock_procs[4];
static size_t s_mock_count = 0;

void reset_mock(void) {
    s_out_len = 0;
    s_out_buf[0] = '\0';
    s_err_len = 0;
    s_err_buf[0] = '\0';
    s_mock_count = 0;
}

long sys_write(int fd, const void *buf, size_t count) {
    if (fd == 1) {
        if (s_out_len + count < sizeof(s_out_buf)) {
            memcpy(s_out_buf + s_out_len, buf, count);
            s_out_len += count;
            s_out_buf[s_out_len] = '\0';
        }
        return (long)count;
    } else if (fd == 2) {
        if (s_err_len + count < sizeof(s_err_buf)) {
            memcpy(s_err_buf + s_err_len, buf, count);
            s_err_len += count;
            s_err_buf[s_err_len] = '\0';
        }
        return (long)count;
    }
    return (long)count;
}

long sys_procinfo(uint64_t index, proc_info_t *buf) {
    if (index >= s_mock_count) return 0; /* end of process table */
    *buf = s_mock_procs[index];
    return 1;
}

int ps_main(int argc, char **argv);

static void print_msg(const char *s) {
    (void)write(1, s, strlen(s));
}

int main(void) {
    /* Setup mock process table */
    reset_mock();
    s_mock_count = 2;
    s_mock_procs[0].pid = 1;
    s_mock_procs[0].ppid = 0;
    s_mock_procs[0].pgid = 1;
    s_mock_procs[0].sid = 1;
    s_mock_procs[0].state = PROC_STATE_RUNNING;
    memcpy(s_mock_procs[0].name, "init", 5);

    s_mock_procs[1].pid = 2;
    s_mock_procs[1].ppid = 1;
    s_mock_procs[1].pgid = 2;
    s_mock_procs[1].sid = 1;
    s_mock_procs[1].state = PROC_STATE_RUNNING;
    memcpy(s_mock_procs[1].name, "shell", 6);

    /* Test 1: Default ps invocation (no args) */
    {
        char *argv[] = {"ps", NULL};
        int rc = ps_main(1, argv);
        assert(rc == 0);
        assert(strstr(s_out_buf, "PID") != NULL);
        assert(strstr(s_out_buf, "PPID") != NULL);
        assert(strstr(s_out_buf, "init") != NULL);
        assert(strstr(s_out_buf, "shell") != NULL);
        print_msg("PASS: default ps\n");
    }

    /* Test 2: ps -o pid,name */
    {
        reset_mock();
        s_mock_count = 2;
        char *argv[] = {"ps", "-o", "pid,name", NULL};
        int rc = ps_main(3, argv);
        assert(rc == 0);
        assert(strstr(s_out_buf, "  PID NAME\n") != NULL);
        assert(strstr(s_out_buf, "    1 init\n") != NULL);
        assert(strstr(s_out_buf, "    2 shell\n") != NULL);
        assert(strstr(s_out_buf, "PPID") == NULL);
        print_msg("PASS: ps -o pid,name\n");
    }

    /* Test 3: ps -o comm,state,ppid */
    {
        reset_mock();
        s_mock_count = 2;
        char *argv[] = {"ps", "-o", "comm,state,ppid", NULL};
        int rc = ps_main(3, argv);
        assert(rc == 0);
        assert(strstr(s_out_buf, "NAME") != NULL);
        assert(strstr(s_out_buf, "STATE") != NULL);
        assert(strstr(s_out_buf, "PPID") != NULL);
        assert(strstr(s_out_buf, "init") != NULL);
        assert(strstr(s_out_buf, "RUNNING") != NULL);
        print_msg("PASS: ps -o comm,state,ppid\n");
    }

    /* Test 4: ps -o invalid_col */
    {
        reset_mock();
        s_mock_count = 2;
        char *argv[] = {"ps", "-o", "foo", NULL};
        int rc = ps_main(3, argv);
        assert(rc == 1);
        assert(strstr(s_err_buf, "unknown column 'foo'") != NULL);
        print_msg("PASS: ps -o invalid rejection\n");
    }

    /* Test 5: ps --help */
    {
        reset_mock();
        char *argv[] = {"ps", "--help", NULL};
        int rc = ps_main(2, argv);
        assert(rc == 0);
        assert(strstr(s_out_buf, "Usage: ps") != NULL);
        assert(strstr(s_out_buf, "-o") != NULL);
        print_msg("PASS: ps --help\n");
    }

    print_msg("ALL PS HOST TESTS PASSED!\n");
    return 0;
}
