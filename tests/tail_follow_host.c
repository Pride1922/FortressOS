/* Host unit tests for /bin/tail -f (bounded follow) under ASan/UBSan. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define MAX_MOCK_DATA 65536

typedef struct {
    char data[MAX_MOCK_DATA];
    size_t size;
    bool exists;
} mock_file_t;

static mock_file_t s_file;

typedef struct {
    bool open;
    size_t offset;
    bool is_stdin;
} mock_fd_t;

static mock_fd_t s_fds[32];

static char s_stdout_buf[65536];
static size_t s_stdout_len;

static char s_stderr_buf[4096];
static size_t s_stderr_len;

static int s_nanosleep_calls;
static timespec_t s_last_req;

static void (*s_sigint_handler)(int) = NULL;
static void (*s_sigterm_handler)(int) = NULL;

static void (*s_sleep_callback)(int iteration) = NULL;
static bool s_inject_eintr = false;

static void mock_reset(void) {
    memset(&s_file, 0, sizeof(s_file));
    s_file.exists = true;

    memset(s_fds, 0, sizeof(s_fds));
    s_fds[0].open = true;
    s_fds[0].is_stdin = true;
    s_fds[1].open = true;
    s_fds[2].open = true;

    s_stdout_len = 0;
    s_stdout_buf[0] = '\0';
    s_stderr_len = 0;
    s_stderr_buf[0] = '\0';

    s_nanosleep_calls = 0;
    memset(&s_last_req, 0, sizeof(s_last_req));

    s_sigint_handler = NULL;
    s_sigterm_handler = NULL;
    s_sleep_callback = NULL;
    s_inject_eintr = false;
}

static void mock_file_set(const char *content) {
    size_t len = strlen(content);
    assert(len < MAX_MOCK_DATA);
    memcpy(s_file.data, content, len);
    s_file.size = len;
    s_file.exists = true;
}

static void mock_file_append(const char *content) {
    size_t len = strlen(content);
    assert(s_file.size + len < MAX_MOCK_DATA);
    memcpy(s_file.data + s_file.size, content, len);
    s_file.size += len;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        if (!s_file.exists || !strcmp(path, "missing")) {
            return SYSCALL_ENOENT;
        }
        vfs_stat_t *st = (vfs_stat_t *)b;
        st->size = (uint64_t)s_file.size;
        st->type = VFS_FILE;
        st->mode = 0644;
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (!s_file.exists || !strcmp(path, "missing")) {
            return SYSCALL_ENOENT;
        }
        for (int i = 3; i < 32; i++) {
            if (!s_fds[i].open) {
                s_fds[i].open = true;
                s_fds[i].offset = 0;
                s_fds[i].is_stdin = false;
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (fd < 0 || fd >= 32 || !s_fds[fd].open) {
            return SYSCALL_EBADF;
        }
        s_fds[fd].open = false;
        return 0;
    }
    if (nr == SYS_DUP2) {
        int oldfd = (int)a;
        int newfd = (int)b;
        if (oldfd < 0 || oldfd >= 32 || !s_fds[oldfd].open) return SYSCALL_EBADF;
        if (newfd < 0 || newfd >= 32) return SYSCALL_EBADF;
        s_fds[newfd] = s_fds[oldfd];
        return newfd;
    }
    if (nr == SYS_READ) {
        int fd = (int)a;
        if (fd < 0 || fd >= 32 || !s_fds[fd].open) return SYSCALL_EBADF;
        mock_fd_t *f = &s_fds[fd];
        if (f->offset >= s_file.size) {
            return 0; /* EOF at this moment */
        }
        size_t avail = s_file.size - f->offset;
        size_t to_read = (size_t)c < avail ? (size_t)c : avail;
        memcpy((void *)b, s_file.data + f->offset, to_read);
        f->offset += to_read;
        return (long)to_read;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *src = (const char *)b;
        size_t count = (size_t)c;
        if (fd == 1) {
            assert(s_stdout_len + count < sizeof(s_stdout_buf));
            memcpy(s_stdout_buf + s_stdout_len, src, count);
            s_stdout_len += count;
            s_stdout_buf[s_stdout_len] = '\0';
            return (long)count;
        }
        if (fd == 2) {
            assert(s_stderr_len + count < sizeof(s_stderr_buf));
            memcpy(s_stderr_buf + s_stderr_len, src, count);
            s_stderr_len += count;
            s_stderr_buf[s_stderr_len] = '\0';
            return (long)count;
        }
        return SYSCALL_EBADF;
    }
    if (nr == SYS_SIGACTION) {
        int sig = (int)a;
        const signal_action_t *sa = (const signal_action_t *)b;
        if (sig == SIGINT) {
            s_sigint_handler = (void (*)(int))sa->handler;
        } else if (sig == SIGTERM) {
            s_sigterm_handler = (void (*)(int))sa->handler;
        }
        return 0;
    }
    if (nr == SYS_NANOSLEEP) {
        s_nanosleep_calls++;
        s_last_req = *(const timespec_t *)a;
        if (s_sleep_callback) {
            s_sleep_callback(s_nanosleep_calls);
        }
        if (s_inject_eintr) {
            s_inject_eintr = false;
            return SYSCALL_EINTR;
        }
        return 0;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

/* -------------------------------------------------------------------------- */
/* Test 1: Follow loop logic with append and SIGINT exit status 130           */
/* -------------------------------------------------------------------------- */
static void cb_test_append(int iteration) {
    if (iteration == 1) {
        mock_file_append("line 4\n");
    } else if (iteration == 2) {
        mock_file_append("line 5\n");
    } else if (iteration == 3) {
        assert(s_sigint_handler != NULL);
        s_sigint_handler(SIGINT);
        s_inject_eintr = true;
    }
}

static void test_follow_append(void) {
    printf("  [PASS] Test 1: Follow-loop append & offset tracking under ASan/UBSan... ");
    mock_reset();
    mock_file_set("line 1\nline 2\nline 3\n");
    s_sleep_callback = cb_test_append;

    char *argv[] = {"tail", "-n", "2", "-f", "test.log", NULL};
    int ret = tail_main(5, argv);

    assert(ret == 130);
    assert(strcmp(s_stdout_buf, "line 2\nline 3\nline 4\nline 5\n") == 0);
    assert(s_nanosleep_calls == 3);
    assert(s_last_req.tv_sec == 0 && s_last_req.tv_nsec == 100000000L);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Truncation detection & recovery                                    */
/* -------------------------------------------------------------------------- */
static void cb_test_truncate(int iteration) {
    if (iteration == 1) {
        /* Truncate file to 0 bytes */
        mock_file_set("");
    } else if (iteration == 2) {
        /* Append fresh content */
        mock_file_append("new line after truncate\n");
    } else if (iteration == 3) {
        assert(s_sigint_handler != NULL);
        s_sigint_handler(SIGINT);
        s_inject_eintr = true;
    }
}

static void test_truncation(void) {
    printf("  [PASS] Test 2: Truncation detection & re-read from offset 0... ");
    mock_reset();
    mock_file_set("old line 1\nold line 2\n");
    s_sleep_callback = cb_test_truncate;

    char *argv[] = {"tail", "-f", "test.log", NULL};
    int ret = tail_main(3, argv);

    assert(ret == 130);
    assert(strstr(s_stderr_buf, "tail: test.log: file truncated\n") != NULL);
    assert(strstr(s_stdout_buf, "old line 1\nold line 2\n") != NULL);
    assert(strstr(s_stdout_buf, "new line after truncate\n") != NULL);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: File deletion error and exit                                       */
/* -------------------------------------------------------------------------- */
static void cb_test_delete(int iteration) {
    if (iteration == 1) {
        s_file.exists = false;
    }
}

static void test_file_deletion(void) {
    printf("  [PASS] Test 3: File deletion detection & clean exit 1... ");
    mock_reset();
    mock_file_set("hello\n");
    s_sleep_callback = cb_test_delete;

    char *argv[] = {"tail", "-f", "test.log", NULL};
    int ret = tail_main(3, argv);

    assert(ret == 1);
    assert(strstr(s_stderr_buf, "tail: test.log: file has been deleted\n") != NULL);
    assert(strcmp(s_stdout_buf, "hello\n") == 0);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Follow stdin diagnostic & exit 0                                   */
/* -------------------------------------------------------------------------- */
static void test_stdin_follow(void) {
    printf("  [PASS] Test 4: Follow standard input (tail -f -) diagnostic & exit 0... ");
    mock_reset();
    mock_file_set("piped data\n");

    char *argv[] = {"tail", "-f", "-", NULL};
    int ret = tail_main(3, argv);

    assert(ret == 0);
    assert(strcmp(s_stdout_buf, "piped data\n") == 0);
    assert(strstr(s_stderr_buf, "tail: -: cannot follow standard input\n") != NULL);

    /* Also test without operand (stdin by default) */
    mock_reset();
    mock_file_set("stdin default\n");
    char *argv2[] = {"tail", "-f", NULL};
    ret = tail_main(2, argv2);
    assert(ret == 0);
    assert(strcmp(s_stdout_buf, "stdin default\n") == 0);
    assert(strstr(s_stderr_buf, "tail: cannot follow standard input\n") != NULL);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: Clean SIGTERM exit (status 0)                                      */
/* -------------------------------------------------------------------------- */
static void cb_test_sigterm(int iteration) {
    if (iteration == 1) {
        assert(s_sigterm_handler != NULL);
        s_sigterm_handler(SIGTERM);
        s_inject_eintr = true;
    }
}

static void test_sigterm(void) {
    printf("  [PASS] Test 5: Clean exit on SIGTERM (status 0)... ");
    mock_reset();
    mock_file_set("initial\n");
    s_sleep_callback = cb_test_sigterm;

    char *argv[] = {"tail", "-f", "test.log", NULL};
    int ret = tail_main(3, argv);

    assert(ret == 0);
    assert(strcmp(s_stdout_buf, "initial\n") == 0);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 6: Byte offset following (-c N -f)                                    */
/* -------------------------------------------------------------------------- */
static void cb_test_bytes(int iteration) {
    if (iteration == 1) {
        mock_file_append("XYZ");
    } else if (iteration == 2) {
        s_sigint_handler(SIGINT);
        s_inject_eintr = true;
    }
}

static void test_bytes_follow(void) {
    printf("  [PASS] Test 6: tail -c N -f byte offset follow... ");
    mock_reset();
    mock_file_set("0123456789");
    s_sleep_callback = cb_test_bytes;

    char *argv[] = {"tail", "-c", "4", "-f", "test.log", NULL};
    int ret = tail_main(5, argv);

    assert(ret == 130);
    assert(strcmp(s_stdout_buf, "6789XYZ") == 0);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 7: Initially empty file followed from offset 0                        */
/* -------------------------------------------------------------------------- */
static void cb_test_empty(int iteration) {
    if (iteration == 1) {
        mock_file_append("first appended line\n");
    } else if (iteration == 2) {
        s_sigint_handler(SIGINT);
        s_inject_eintr = true;
    }
}

static void test_empty_file(void) {
    printf("  [PASS] Test 7: Initially empty file followed from 0... ");
    mock_reset();
    mock_file_set("");
    s_sleep_callback = cb_test_empty;

    char *argv[] = {"tail", "-f", "test.log", NULL};
    int ret = tail_main(3, argv);

    assert(ret == 130);
    assert(strcmp(s_stdout_buf, "first appended line\n") == 0);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Main test runner                                                           */
/* -------------------------------------------------------------------------- */
int main(void) {
    printf("Running tail follow host tests under ASan/UBSan...\n");
    test_follow_append();
    test_truncation();
    test_file_deletion();
    test_stdin_follow();
    test_sigterm();
    test_bytes_follow();
    test_empty_file();
    printf(">>> ALL TAIL FOLLOW HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
