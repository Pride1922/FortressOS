#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "timeline_log.h"
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

static char s_stderr_buf[4096];
static size_t s_stderr_len = 0;

static char s_log_buf[16384];
static size_t s_log_len = 0;

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
    s_mock_var_log_messages_st_err = SYSCALL_ENOENT;
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
    s_stderr_len = 0;
    s_stderr_buf[0] = '\0';
    s_log_len = 0;
    s_log_buf[0] = '\0';

    s_write_calls = 0;
    s_mock_write_override = 0;
    s_mock_short_write_len = 0;

    timeline_log_reset_rate_limit();
}

long timeline_test_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    (void)d;
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

        if (strcmp(path, TIMELINE_LOG_FILE_PRIMARY) == 0) {
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
        if (strcmp(path, TIMELINE_LOG_FILE_FALLBACK) == 0) {
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

    if (nr == SYS_STAT_EXT) {
        const char *path = (const char *)a;
        stat_ext_v1_t *st = (stat_ext_v1_t *)b;
        if (!path || !st) return SYSCALL_EINVAL;

        if (strcmp(path, TIMELINE_LOG_DIR_PRIMARY) == 0) {
            if (s_mock_var_log_stat_err != 0) return s_mock_var_log_stat_err;
            *st = s_mock_var_log_st;
            return 0;
        }
        if (strcmp(path, TIMELINE_LOG_DIR_FALLBACK) == 0) {
            if (s_mock_run_stat_err != 0) return s_mock_run_stat_err;
            *st = s_mock_run_st;
            return 0;
        }
        if (strcmp(path, TIMELINE_LOG_FILE_PRIMARY) == 0) {
            if (s_mock_var_log_messages_st_err != 0) return s_mock_var_log_messages_st_err;
            *st = s_mock_var_log_messages_st;
            return 0;
        }
        if (strcmp(path, TIMELINE_LOG_FILE_FALLBACK) == 0) {
            if (s_mock_run_messages_st_err != 0) return s_mock_run_messages_st_err;
            *st = s_mock_run_messages_st;
            return 0;
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (strcmp(path, TIMELINE_LOG_FILE_PRIMARY) == 0) {
            if (s_mock_var_log_messages_open_err != 0) return s_mock_var_log_messages_open_err;
            return 10;
        }
        if (strcmp(path, TIMELINE_LOG_FILE_FALLBACK) == 0) {
            if (s_mock_run_messages_open_err != 0) return s_mock_run_messages_open_err;
            return 11;
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_FCHMOD) {
        s_last_fchmod_fd = (int)a;
        s_last_fchmod_mode = (uint32_t)b;
        return 0;
    }

    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (s_closed_count < sizeof(s_closed_fds) / sizeof(s_closed_fds[0])) {
            s_closed_fds[s_closed_count++] = fd;
        }
        return 0;
    }

    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *buf = (const char *)b;
        size_t len = (size_t)c;
        s_write_calls++;

        if (fd == 2) {
            for (size_t i = 0; i < len; i++) {
                if (s_stderr_len + 1 < sizeof(s_stderr_buf)) {
                    s_stderr_buf[s_stderr_len++] = buf[i];
                }
            }
            s_stderr_buf[s_stderr_len] = '\0';
            return (long)len;
        }

        if (fd == 10 || fd == 11) {
            if (s_mock_write_override != 0) return s_mock_write_override;
            if (s_mock_short_write_len > 0) {
                size_t actual = (s_mock_short_write_len < len) ? s_mock_short_write_len : len;
                for (size_t i = 0; i < actual; i++) {
                    if (s_log_len + 1 < sizeof(s_log_buf)) {
                        s_log_buf[s_log_len++] = buf[i];
                    }
                }
                s_log_buf[s_log_len] = '\0';
                return (long)actual;
            }
            for (size_t i = 0; i < len; i++) {
                if (s_log_len + 1 < sizeof(s_log_buf)) {
                    s_log_buf[s_log_len++] = buf[i];
                }
            }
            s_log_buf[s_log_len] = '\0';
            return (long)len;
        }

        return SYSCALL_EBADF;
    }

    return SYSCALL_ENOSYS;
}

/* =========================================================================
 * Test 1: Formatting and Arguments
 * ========================================================================= */
static void test_formatting(void) {
    printf("[1/9] Testing timeline_log() formatting and specifiers...\n");
    reset_mock_state();

    int rc = timeline_log("test", "User %s (uid=%d, hex=0x%x) authenticated from %s",
                          "alice", 1000, 0xabcd, "10.0.2.15");
    assert(rc == 0);
    assert(s_log_len > 0);
    assert(strstr(s_log_buf, "[2026-10-16 08:30:45 UTC] test: User alice (uid=1000, hex=0xabcd) authenticated from 10.0.2.15\n") != NULL);
    assert(s_closed_count == 1);
    assert(s_closed_fds[0] == 10);
    assert(s_last_fchmod_fd == 10);
    assert(s_last_fchmod_mode == 0640);
}

/* =========================================================================
 * Test 2: Fallback to Monotonic Uptime
 * ========================================================================= */
static void test_uptime_fallback(void) {
    printf("[2/9] Testing timeline_log() monotonic uptime fallback...\n");
    reset_mock_state();
    s_mock_realtime.tv_sec = 0; /* Pre-2026 epoch */
    s_mock_monotime.tv_sec = 123;
    s_mock_monotime.tv_nsec = 456000000;

    int rc = timeline_log("kernel", "Device initialized in %ums", 45);
    assert(rc == 0);
    assert(strstr(s_log_buf, "[  123.456s UPTIME] kernel: Device initialized in 45ms\n") != NULL);
}

/* =========================================================================
 * Test 3: Log Sanitization (Control Bytes and Terminal Attacks)
 * ========================================================================= */
static void test_sanitization(void) {
    printf("[3/9] Testing timeline_log() sanitization of control characters...\n");
    reset_mock_state();

    int rc = timeline_log("auth", "User\x1b[31m injected\r\nand control\x01\x7f");
    assert(rc == 0);
    assert(strstr(s_log_buf, "\\x1b[31m injected  and control\\x01\\x7f\n") != NULL);
}

/* =========================================================================
 * Test 4: Truncation to 512 Bytes with '...\n'
 * ========================================================================= */
static void test_truncation(void) {
    printf("[4/9] Testing record truncation to 512 bytes with ... indicator...\n");
    reset_mock_state();

    char huge[1024];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    int rc = timeline_log("overflow", "%s", huge);
    assert(rc == 0);
    assert(s_log_len <= TIMELINE_LOG_BUFFER_SIZE);
    assert(s_log_buf[s_log_len - 1] == '\n');
    assert(s_log_buf[s_log_len - 2] == '.');
    assert(s_log_buf[s_log_len - 3] == '.');
    assert(s_log_buf[s_log_len - 4] == '.');
}

/* =========================================================================
 * Test 5: Fallback Path to /run/messages
 * ========================================================================= */
static void test_fallback_path(void) {
    printf("[5/9] Testing fallback from /var/log/messages to /run/messages on ENOENT/EROFS...\n");
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_ENOENT;

    int rc = timeline_log("dhcpc", "Lease bound: IP 10.0.2.15/24");
    assert(rc == 0);
    assert(s_closed_count == 1);
    assert(s_closed_fds[0] == 11); /* Fallback fd */
    assert(strstr(s_log_buf, "Lease bound: IP 10.0.2.15/24\n") != NULL);

    /* Test EROFS on /var/log/messages */
    reset_mock_state();
    s_mock_var_log_messages_open_err = SYSCALL_EROFS;

    rc = timeline_log("sshd", "Accepted password for operator");
    assert(rc == 0);
    assert(s_closed_fds[0] == 11);
    assert(strstr(s_log_buf, "Accepted password for operator\n") != NULL);
}

/* =========================================================================
 * Test 6: Rejection on EACCES / EPERM (Fail Closed, No Fallback)
 * ========================================================================= */
static void test_fail_closed_permission(void) {
    printf("[6/9] Testing fail-closed rejection on EACCES/EPERM without fallback...\n");
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_EACCES;

    int rc = timeline_log("audit", "Should fail closed");
    assert(rc == -1);
    assert(s_log_len == 0);
    assert(s_stderr_len > 0);
    assert(strstr(s_stderr_buf, "timeline_log:") != NULL);
}

/* =========================================================================
 * Test 7: Untrusted Validation (Symlink, World-Writable, Non-Root)
 * ========================================================================= */
static void test_untrusted_validation(void) {
    printf("[7/9] Testing untrusted validation (symlink, world-writable, non-root)...\n");

    /* 7a. World-writable directory */
    reset_mock_state();
    s_mock_var_log_st.mode = VFS_S_IFDIR | 0777; /* World-writable */
    int rc = timeline_log("test", "World-writable dir rejected");
    assert(rc == -1);
    assert(s_log_len == 0);

    /* 7b. Non-root owned directory */
    reset_mock_state();
    s_mock_var_log_st.uid = 1000;
    rc = timeline_log("test", "Non-root dir rejected");
    assert(rc == -1);
    assert(s_log_len == 0);

    /* 7c. Symlink rejected */
    reset_mock_state();
    s_mock_var_log_messages_st_err = 0;
    s_mock_var_log_messages_is_symlink = true;
    rc = timeline_log("test", "Symlink rejected");
    assert(rc == -1);
    assert(s_log_len == 0);
}

/* =========================================================================
 * Test 8: Short-Write Handling (No Fragmentation Retry, No Replay)
 * ========================================================================= */
static void test_short_write(void) {
    printf("[8/9] Testing short-write handling (fail closed, no retry)...\n");
    reset_mock_state();
    s_mock_short_write_len = 10; /* Partial write of 10 bytes */

    int rc = timeline_log("test", "This write will be truncated by ENOSPC");
    assert(rc == -1);
    assert(s_write_calls == 2); /* 1st call for log fd, 2nd call for stderr diagnostic */
    assert(s_stderr_len > 0);
    assert(strstr(s_stderr_buf, "short write") != NULL);
}

/* =========================================================================
 * Test 9: Rate-Limiting of Stderr Diagnostics
 * ========================================================================= */
static void test_rate_limiting(void) {
    printf("[9/9] Testing rate-limiting of stderr diagnostics...\n");
    reset_mock_state();
    s_mock_var_log_stat_err = SYSCALL_EACCES;
    s_mock_monotime.tv_sec = 100;

    /* First failure: emits diagnostic */
    int rc = timeline_log("test", "Failure 1");
    assert(rc == -1);
    assert(s_stderr_len > 0);
    size_t first_stderr_len = s_stderr_len;

    /* Subsequent failures within 5 seconds: suppressed */
    s_mock_monotime.tv_sec = 101;
    rc = timeline_log("test", "Failure 2");
    assert(rc == -1);
    assert(s_stderr_len == first_stderr_len);

    s_mock_monotime.tv_sec = 103;
    rc = timeline_log("test", "Failure 3");
    assert(rc == -1);
    assert(s_stderr_len == first_stderr_len);

    /* After >= 5 seconds (105): next failure reports suppressed messages */
    s_mock_monotime.tv_sec = 106;
    rc = timeline_log("test", "Failure 4");
    assert(rc == -1);
    assert(s_stderr_len > first_stderr_len);
    assert(strstr(s_stderr_buf, "(suppressed 2 error messages)") != NULL);
}

int main(void) {
    printf("====================================================\n");
    printf("Running timeline_log() host test suite (ASan/UBSan)\n");
    printf("====================================================\n");

    test_formatting();
    test_uptime_fallback();
    test_sanitization();
    test_truncation();
    test_fallback_path();
    test_fail_closed_permission();
    test_untrusted_validation();
    test_short_write();
    test_rate_limiting();

    printf("====================================================\n");
    printf("All 9 test suites passed successfully!\n");
    printf("====================================================\n");
    return 0;
}
