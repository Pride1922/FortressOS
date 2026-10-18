#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "logger.h"
#include "common.h"
#include "permissions_cli.h"
#include "vfs.h"

/* --- Mock State --- */
static timespec_t s_mock_realtime;
static timespec_t s_mock_monotime;
static long s_mock_clock_real_ret = 0;
static long s_mock_clock_mono_ret = 0;

static stat_ext_v1_t s_mock_var_log_st;
static long s_mock_var_log_stat_err = 0;

static stat_ext_v1_t s_mock_run_st;
static long s_mock_run_stat_err = 0;

static stat_ext_v1_t s_mock_var_log_messages_st;
static long s_mock_var_log_messages_st_err = 0;
static bool s_mock_var_log_messages_is_symlink = false;

static stat_ext_v1_t s_mock_run_messages_st;
static long s_mock_run_messages_st_err = 0;
static bool s_mock_run_messages_is_symlink = false;

static long s_mock_var_log_messages_open_err = 0;
static long s_mock_run_messages_open_err = 0;

static int s_last_fchmod_fd = -1;
static uint32_t s_last_fchmod_mode = 0;

static int s_closed_fds[32];
static size_t s_closed_count = 0;

static char s_stdout_buf[4096];
static size_t s_stdout_len = 0;

static char s_stderr_buf[4096];
static size_t s_stderr_len = 0;

static char s_log_buf[16384];
static size_t s_log_len = 0;

static const char *s_stdin_data = NULL;
static size_t s_stdin_len = 0;
static size_t s_stdin_pos = 0;

static size_t s_write_calls = 0;
static long s_mock_write_override = 0;
static size_t s_mock_short_write_len = 0;

static void reset_mock_state(void) {
    s_mock_realtime.tv_sec = 1792139445; /* 2026-10-16 08:30:45 UTC */
    s_mock_realtime.tv_nsec = 123456789;
    s_mock_monotime.tv_sec = 12;
    s_mock_monotime.tv_nsec = 345000000;
    s_mock_clock_real_ret = 0;
    s_mock_clock_mono_ret = 0;

    memset(&s_mock_var_log_st, 0, sizeof(s_mock_var_log_st));
    s_mock_var_log_st.type = VFS_DIRECTORY;
    s_mock_var_log_st.mode = VFS_S_IFDIR | 0755;
    s_mock_var_log_st.uid = 0;
    s_mock_var_log_st.gid = 10;
    s_mock_var_log_stat_err = 0;

    memset(&s_mock_run_st, 0, sizeof(s_mock_run_st));
    s_mock_run_st.type = VFS_DIRECTORY;
    s_mock_run_st.mode = VFS_S_IFDIR | 0755;
    s_mock_run_st.uid = 0;
    s_mock_run_st.gid = 0;
    s_mock_run_stat_err = 0;

    memset(&s_mock_var_log_messages_st, 0, sizeof(s_mock_var_log_messages_st));
    s_mock_var_log_messages_st.type = VFS_FILE;
    s_mock_var_log_messages_st.mode = VFS_S_IFREG | 0640;
    s_mock_var_log_messages_st.uid = 0;
    s_mock_var_log_messages_st.gid = 10;
    s_mock_var_log_messages_st_err = SYSCALL_ENOENT; /* Initially not created */
    s_mock_var_log_messages_is_symlink = false;

    memset(&s_mock_run_messages_st, 0, sizeof(s_mock_run_messages_st));
    s_mock_run_messages_st.type = VFS_FILE;
    s_mock_run_messages_st.mode = VFS_S_IFREG | 0640;
    s_mock_run_messages_st.uid = 0;
    s_mock_run_messages_st.gid = 0;
    s_mock_run_messages_st_err = SYSCALL_ENOENT;
    s_mock_run_messages_is_symlink = false;

    s_mock_var_log_messages_open_err = 0;
    s_mock_run_messages_open_err = 0;

    s_last_fchmod_fd = -1;
    s_last_fchmod_mode = 0;

    s_closed_count = 0;

    s_stdout_len = 0;
    s_stdout_buf[0] = '\0';
    s_stderr_len = 0;
    s_stderr_buf[0] = '\0';
    s_log_len = 0;
    s_log_buf[0] = '\0';

    s_stdin_data = NULL;
    s_stdin_len = 0;
    s_stdin_pos = 0;

    s_write_calls = 0;
    s_mock_write_override = 0;
    s_mock_short_write_len = 0;
}

/* --- System Call Emulation --- */
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_CLOCK_GETTIME) {
        uint32_t clk_id = (uint32_t)a;
        timespec_t *ts = (timespec_t *)b;
        if (clk_id == CLOCK_REALTIME) {
            if (s_mock_clock_real_ret < 0) return s_mock_clock_real_ret;
            if (ts) *ts = s_mock_realtime;
            return 0;
        } else if (clk_id == CLOCK_MONOTONIC) {
            if (s_mock_clock_mono_ret < 0) return s_mock_clock_mono_ret;
            if (ts) *ts = s_mock_monotime;
            return 0;
        }
        return SYSCALL_EINVAL;
    }

    if (nr == SYS_LSTAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (!path || !st) return SYSCALL_EINVAL;

        if (strcmp(path, LOGGER_LOG_FILE_PRIMARY) == 0) {
            if (s_mock_var_log_messages_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                st->size = 0;
                return 0;
            }
            if (s_mock_var_log_messages_st_err == 0) {
                st->mode = s_mock_var_log_messages_st.mode;
                st->type = s_mock_var_log_messages_st.type;
                st->size = s_mock_var_log_messages_st.file_size;
                return 0;
            }
            return s_mock_var_log_messages_st_err;
        }
        if (strcmp(path, LOGGER_LOG_FILE_FALLBACK) == 0) {
            if (s_mock_run_messages_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                st->size = 0;
                return 0;
            }
            if (s_mock_run_messages_st_err == 0) {
                st->mode = s_mock_run_messages_st.mode;
                st->type = s_mock_run_messages_st.type;
                st->size = s_mock_run_messages_st.file_size;
                return 0;
            }
            return s_mock_run_messages_st_err;
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (strcmp(path, LOGGER_LOG_FILE_PRIMARY) == 0) {
            if (s_mock_var_log_messages_open_err != 0) {
                return s_mock_var_log_messages_open_err;
            }
            return 10; /* Primary file descriptor */
        }
        if (strcmp(path, LOGGER_LOG_FILE_FALLBACK) == 0) {
            if (s_mock_run_messages_open_err != 0) {
                return s_mock_run_messages_open_err;
            }
            return 11; /* Fallback file descriptor */
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_FCHMOD) {
        int fd = (int)a;
        uint32_t mode = (uint32_t)b;
        s_last_fchmod_fd = fd;
        s_last_fchmod_mode = mode;
        return 0;
    }

    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (s_closed_count < sizeof(s_closed_fds)/sizeof(s_closed_fds[0])) {
            s_closed_fds[s_closed_count++] = fd;
        }
        return 0;
    }

    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *data = (const char *)b;
        size_t len = (size_t)c;

        if (fd == 1) {
            if (s_stdout_len + len < sizeof(s_stdout_buf)) {
                memcpy(s_stdout_buf + s_stdout_len, data, len);
                s_stdout_len += len;
                s_stdout_buf[s_stdout_len] = '\0';
            }
            return (long)len;
        }
        if (fd == 2) {
            if (s_stderr_len + len < sizeof(s_stderr_buf)) {
                memcpy(s_stderr_buf + s_stderr_len, data, len);
                s_stderr_len += len;
                s_stderr_buf[s_stderr_len] = '\0';
            }
            return (long)len;
        }
        if (fd == 10 || fd == 11) {
            s_write_calls++;
            if (s_mock_write_override != 0) {
                return s_mock_write_override;
            }
            if (s_mock_short_write_len > 0) {
                size_t w = (len < s_mock_short_write_len) ? len : s_mock_short_write_len;
                if (s_log_len + w < sizeof(s_log_buf)) {
                    memcpy(s_log_buf + s_log_len, data, w);
                    s_log_len += w;
                    s_log_buf[s_log_len] = '\0';
                }
                return (long)w;
            }
            if (s_log_len + len < sizeof(s_log_buf)) {
                memcpy(s_log_buf + s_log_len, data, len);
                s_log_len += len;
                s_log_buf[s_log_len] = '\0';
            }
            return (long)len;
        }
        return SYSCALL_EBADF;
    }

    if (nr == SYS_READ) {
        int fd = (int)a;
        char *buf = (char *)b;
        size_t cap = (size_t)c;
        if (fd == 0) {
            if (s_stdin_pos < s_stdin_len && s_stdin_data) {
                size_t avail = s_stdin_len - s_stdin_pos;
                size_t take = (avail < cap) ? avail : cap;
                memcpy(buf, s_stdin_data + s_stdin_pos, take);
                s_stdin_pos += take;
                return (long)take;
            }
            return 0; /* EOF */
        }
        return SYSCALL_EBADF;
    }

    return SYSCALL_ENOSYS;
}

long permission_call4(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    (void)c; (void)d;
    if (nr == SYS_STAT_EXT) {
        const char *path = (const char *)a;
        stat_ext_v1_t *out = (stat_ext_v1_t *)b;
        if (!path || !out) return SYSCALL_EINVAL;

        if (strcmp(path, LOGGER_LOG_DIR_PRIMARY) == 0) {
            if (s_mock_var_log_stat_err != 0) return s_mock_var_log_stat_err;
            *out = s_mock_var_log_st;
            return 0;
        }
        if (strcmp(path, LOGGER_LOG_DIR_FALLBACK) == 0) {
            if (s_mock_run_stat_err != 0) return s_mock_run_stat_err;
            *out = s_mock_run_st;
            return 0;
        }
        if (strcmp(path, LOGGER_LOG_FILE_PRIMARY) == 0) {
            if (s_mock_var_log_messages_st_err != 0) return s_mock_var_log_messages_st_err;
            *out = s_mock_var_log_messages_st;
            return 0;
        }
        if (strcmp(path, LOGGER_LOG_FILE_FALLBACK) == 0) {
            if (s_mock_run_messages_st_err != 0) return s_mock_run_messages_st_err;
            *out = s_mock_run_messages_st;
            return 0;
        }
        return SYSCALL_ENOENT;
    }
    return SYSCALL_ENOSYS;
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void test_sanitization(void) {
    printf("[1/9] Testing logger_sanitize()...\n");
    char out[128];

    /* 1. Normal string */
    size_t len = logger_sanitize(out, sizeof(out), "Hello World 123", 15);
    assert(len == 15);
    assert(strcmp(out, "Hello World 123") == 0);

    /* 2. Embedded \r and \n replaced with space */
    len = logger_sanitize(out, sizeof(out), "Line1\r\nLine2\n", 13);
    assert(len == 13);
    assert(strcmp(out, "Line1  Line2 ") == 0);

    /* 3. Control characters < 0x20 escaped as \xHH */
    len = logger_sanitize(out, sizeof(out), "Tab\tEsc\x1b" "End", 11);
    assert(strcmp(out, "Tab\\x09Esc\\x1bEnd") == 0);

    /* 4. High bytes >= 0x7f escaped as \xHH */
    len = logger_sanitize(out, sizeof(out), "\x7f\x80\xff", 3);
    assert(strcmp(out, "\\x7f\\x80\\xff") == 0);

    /* 5. Buffer saturation */
    char small[8];
    len = logger_sanitize(small, sizeof(small), "123456789", 9);
    assert(len == 7);
    assert(strcmp(small, "1234567") == 0);
}

static void test_format_record_utc(void) {
    printf("[2/9] Testing logger_format_record() with valid UTC...\n");
    char rec[LOGGER_BUFFER_SIZE];

    /* Realtime: 2026-10-16 08:30:45 UTC */
    int64_t real_sec = 1792139445;
    int64_t mono_sec = 12;
    int64_t mono_nsec = 345000000;

    size_t len = logger_format_record(rec, sizeof(rec), "kernel", "eth0 link up", 12,
                                     real_sec, mono_sec, mono_nsec);
    assert(len > 0);
    assert(len <= LOGGER_BUFFER_SIZE);
    assert(strcmp(rec, "[2026-10-16 08:30:45 UTC] kernel: eth0 link up\n") == 0);

    /* Default tag when NULL or empty */
    len = logger_format_record(rec, sizeof(rec), NULL, "service started", 15,
                               real_sec, mono_sec, mono_nsec);
    assert(strcmp(rec, "[2026-10-16 08:30:45 UTC] logger: service started\n") == 0);

    len = logger_format_record(rec, sizeof(rec), "", "service started", 15,
                               real_sec, mono_sec, mono_nsec);
    assert(strcmp(rec, "[2026-10-16 08:30:45 UTC] logger: service started\n") == 0);
}

static void test_format_record_uptime_fallback(void) {
    printf("[3/9] Testing logger_format_record() monotonic uptime fallback...\n");
    char rec[LOGGER_BUFFER_SIZE];

    /* Realtime < 2026 (epoch 0 = 1970) */
    size_t len = logger_format_record(rec, sizeof(rec), "boot", "early stage", 11,
                                     0, 12, 345000000);
    assert(len > 0);
    assert(strcmp(rec, "[   12.345s UPTIME] boot: early stage\n") == 0);

    /* Realtime > 2100 */
    len = logger_format_record(rec, sizeof(rec), "test", "future clock", 12,
                               4200000000LL, 0, 5000000);
    assert(len > 0);
    assert(strcmp(rec, "[    0.005s UPTIME] test: future clock\n") == 0);

    /* Realtime failure (-1) */
    len = logger_format_record(rec, sizeof(rec), "audit", "failure check", 13,
                               -1, 1234, 567000000);
    assert(len > 0);
    assert(strcmp(rec, "[ 1234.567s UPTIME] audit: failure check\n") == 0);
}

static void test_format_truncation(void) {
    printf("[4/9] Testing record truncation to 512 bytes with ... indicator...\n");
    char rec[LOGGER_BUFFER_SIZE];

    /* Construct a 600-byte message */
    char long_msg[600];
    memset(long_msg, 'A', sizeof(long_msg));
    long_msg[sizeof(long_msg) - 1] = '\0';

    int64_t real_sec = 1792139445;
    size_t len = logger_format_record(rec, sizeof(rec), "test", long_msg, sizeof(long_msg) - 1,
                                     real_sec, 0, 0);

    assert(len == 512);
    assert(rec[508] == '.');
    assert(rec[509] == '.');
    assert(rec[510] == '.');
    assert(rec[511] == '\n');
    assert(strncmp(rec, "[2026-10-16 08:30:45 UTC] test: AAAAA", 37) == 0);

    /* Verify truncation with escape sequence at boundary does not emit broken \x */
    char esc_msg[600];
    memset(esc_msg, '\x1b', sizeof(esc_msg)); /* each expands to 4 bytes */
    len = logger_format_record(rec, sizeof(rec), "esc", esc_msg, sizeof(esc_msg),
                               real_sec, 0, 0);
    assert(len <= 512);
    assert(rec[len - 4] == '.');
    assert(rec[len - 3] == '.');
    assert(rec[len - 2] == '.');
    assert(rec[len - 1] == '\n');
    /* Verify characters before "..." end with a complete escape (e.g. 'b' from \x1b) */
    assert(rec[len - 5] == 'b');
}

static void test_destination_selection_and_fallback(void) {
    printf("[5/9] Testing logger_open_destination() primary and fallback paths...\n");

    /* 1. Primary path /var/log/messages succeeds */
    reset_mock_state();
    int fd = logger_open_destination();
    assert(fd == 10);
    assert(s_last_fchmod_fd == 10);
    assert(s_last_fchmod_mode == 0640);

    /* 2. Primary /var/log does not exist (ENOENT) -> fallback to /run/messages */
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_ENOENT;
    fd = logger_open_destination();
    assert(fd == 11);
    assert(s_last_fchmod_fd == 11);
    assert(s_last_fchmod_mode == 0640);

    /* 3. Primary /var/log is read-only (EROFS) -> fallback to /run/messages */
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_EROFS;
    fd = logger_open_destination();
    assert(fd == 11);
    assert(s_last_fchmod_fd == 11);

    /* 4. Primary open of /var/log/messages returns EROFS -> fallback to /run */
    reset_mock_state();
    s_mock_var_log_messages_open_err = SYSCALL_EROFS;
    fd = logger_open_destination();
    assert(fd == 11);
}

static void test_untrusted_validation_rejections(void) {
    printf("[6/9] Testing untrusted validation (world-writable, symlink, non-root)...\n");

    /* 1. /var/log is world-writable -> fail closed, NO fallback */
    reset_mock_state();
    s_mock_var_log_st.mode = VFS_S_IFDIR | 0777;
    int fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "world-writable") != NULL);

    /* 2. /var/log is owned by non-root (uid=1000) -> fail closed, NO fallback */
    reset_mock_state();
    s_mock_var_log_st.uid = 1000;
    fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "not owned by root") != NULL);

    /* 3. /var/log/messages is a symlink -> fail closed, NO fallback */
    reset_mock_state();
    s_mock_var_log_messages_is_symlink = true;
    fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "symlink rejected") != NULL);

    /* 4. /var/log/messages existing file is world-writable -> fail closed */
    reset_mock_state();
    s_mock_var_log_messages_st_err = 0;
    s_mock_var_log_messages_st.mode = VFS_S_IFREG | 0666;
    fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "world-writable") != NULL);

    /* 5. Permission denied (EACCES/EPERM) on /var/log -> fail closed, NO fallback */
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_EACCES;
    fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "permission denied") != NULL);

    /* 6. Fallback /run is world-writable -> fail closed */
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_ENOENT;
    s_mock_run_st.mode = VFS_S_IFDIR | 0777;
    fd = logger_open_destination();
    assert(fd < 0);
    assert(strstr(s_stderr_buf, "world-writable") != NULL);
}

static void test_write_and_short_write_semantics(void) {
    printf("[7/9] Testing logger_write_record() and short-write handling...\n");

    /* 1. Normal write succeeds */
    reset_mock_state();
    const char *msg = "test log entry\n";
    int r = logger_write_record(10, msg, strlen(msg));
    assert(r == 0);
    assert(s_write_calls == 1);
    assert(strcmp(s_log_buf, msg) == 0);

    /* 2. Short write (storage exhausted mid-write) -> fails closed, reports truncation notice, NO retry */
    reset_mock_state();
    s_mock_short_write_len = 5; /* only writes 5 bytes */
    r = logger_write_record(10, msg, strlen(msg));
    assert(r != 0);
    assert(s_write_calls == 1); /* Exactly one SYS_WRITE, never retried */
    assert(strstr(s_stderr_buf, "short write") != NULL);

    /* 3. Storage failure (ENOSPC on write) -> fails closed */
    reset_mock_state();
    s_mock_write_override = SYSCALL_ENOSPC;
    r = logger_write_record(10, msg, strlen(msg));
    assert(r != 0);
    assert(s_write_calls == 1);
    assert(strstr(s_stderr_buf, "write failed") != NULL);
}

static void test_cli_argument_parsing(void) {
    printf("[8/9] Testing logger CLI arguments...\n");

    /* 1. Basic command line message */
    reset_mock_state();
    char *argv1[] = {"logger", "-t", "auth", "User login ok", NULL};
    int r = logger_main(4, argv1);
    assert(r == 0);
    assert(s_closed_count == 1);
    assert(s_closed_fds[0] == 10);
    assert(strcmp(s_log_buf, "[2026-10-16 08:30:45 UTC] auth: User login ok\n") == 0);

    /* 2. Default tag when -t omitted */
    reset_mock_state();
    char *argv2[] = {"logger", "Simple default tag message", NULL};
    r = logger_main(2, argv2);
    assert(r == 0);
    assert(strcmp(s_log_buf, "[2026-10-16 08:30:45 UTC] logger: Simple default tag message\n") == 0);

    /* 3. Multiple positional arguments joined with space */
    reset_mock_state();
    char *argv3[] = {"logger", "-t", "net", "interface", "eth0", "carrier", "lost", NULL};
    r = logger_main(7, argv3);
    assert(r == 0);
    assert(strcmp(s_log_buf, "[2026-10-16 08:30:45 UTC] net: interface eth0 carrier lost\n") == 0);

    /* 4. Missing argument to -t -> exit 1 */
    reset_mock_state();
    char *argv4[] = {"logger", "-t", NULL};
    r = logger_main(2, argv4);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "option requires an argument: -t") != NULL);

    /* 5. Help option --help -> exit 0 */
    reset_mock_state();
    char *argv5[] = {"logger", "--help", NULL};
    r = logger_main(2, argv5);
    assert(r == 0);
    assert(strstr(s_stdout_buf, "usage: logger") != NULL);

    /* 6. Unrecognized option -> exit 1 */
    reset_mock_state();
    char *argv6[] = {"logger", "-x", "hello", NULL};
    r = logger_main(3, argv6);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "unrecognized option") != NULL);
}

static void test_stdin_streaming(void) {
    printf("[9/9] Testing logger stdin streaming...\n");

    /* 1. Multi-line stream from stdin */
    reset_mock_state();
    const char *input = "First line\nSecond line\r\nThird line\n";
    s_stdin_data = input;
    s_stdin_len = strlen(input);
    s_stdin_pos = 0;

    char *argv1[] = {"logger", "-t", "stream", NULL};
    int r = logger_main(3, argv1);
    assert(r == 0);
    const char *expected =
        "[2026-10-16 08:30:45 UTC] stream: First line\n"
        "[2026-10-16 08:30:45 UTC] stream: Second line\n"
        "[2026-10-16 08:30:45 UTC] stream: Third line\n";
    assert(strcmp(s_log_buf, expected) == 0);

    /* 2. Unterminated input line before EOF -> appends \n */
    reset_mock_state();
    const char *unterminated = "No newline at end";
    s_stdin_data = unterminated;
    s_stdin_len = strlen(unterminated);
    s_stdin_pos = 0;

    char *argv2[] = {"logger", "-t", "pipe", NULL};
    r = logger_main(3, argv2);
    assert(r == 0);
    assert(strcmp(s_log_buf, "[2026-10-16 08:30:45 UTC] pipe: No newline at end\n") == 0);

    /* 3. Empty stdin -> 0 writes, exits 0 */
    reset_mock_state();
    s_stdin_data = "";
    s_stdin_len = 0;
    s_stdin_pos = 0;

    char *argv3[] = {"logger", NULL};
    r = logger_main(1, argv3);
    assert(r == 0);
    assert(s_log_len == 0);

    /* 4. Oversized line (> 512 bytes) on stdin -> truncated with ..., remainder skipped until \n */
    reset_mock_state();
    char big_stream[800];
    memset(big_stream, 'B', 700);
    big_stream[700] = '\n';
    memcpy(big_stream + 701, "Next line\n", 11);
    s_stdin_data = big_stream;
    s_stdin_len = strlen(big_stream);
    s_stdin_pos = 0;

    char *argv4[] = {"logger", "-t", "big", NULL};
    r = logger_main(3, argv4);
    assert(r == 0);
    /* First record truncated with ...\n, followed by second line */
    assert(strstr(s_log_buf, "...\n") != NULL);
    assert(strstr(s_log_buf, "[2026-10-16 08:30:45 UTC] big: Next line\n") != NULL);
}

int main(void) {
    printf("====================================================\n");
    printf("Running /bin/logger host test suite (ASan/UBSan)\n");
    printf("====================================================\n");

    test_sanitization();
    test_format_record_utc();
    test_format_record_uptime_fallback();
    test_format_truncation();
    test_destination_selection_and_fallback();
    test_untrusted_validation_rejections();
    test_write_and_short_write_semantics();
    test_cli_argument_parsing();
    test_stdin_streaming();

    printf("====================================================\n");
    printf("All 9 test suites passed successfully!\n");
    printf("====================================================\n");
    return 0;
}
