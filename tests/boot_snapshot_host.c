#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "boot_snapshot.h"
#include "common.h"
#include "permissions_cli.h"
#include "vfs.h"

/* --- Mock State --- */
static stat_ext_v1_t s_mock_var_log_st;
static long s_mock_var_log_stat_err = 0;

static stat_ext_v1_t s_mock_run_st;
static long s_mock_run_stat_err = 0;

static stat_ext_v1_t s_mock_var_boot_log_st;
static long s_mock_var_boot_log_st_err = SYSCALL_ENOENT;
static bool s_mock_var_boot_log_is_symlink = false;

static stat_ext_v1_t s_mock_var_boot_prev_st;
static long s_mock_var_boot_prev_st_err = SYSCALL_ENOENT;
static bool s_mock_var_boot_prev_is_symlink = false;

static stat_ext_v1_t s_mock_run_boot_log_st;
static long s_mock_run_boot_log_st_err = SYSCALL_ENOENT;
static bool s_mock_run_boot_log_is_symlink = false;

static stat_ext_v1_t s_mock_run_boot_prev_st;
static long s_mock_run_boot_prev_st_err = SYSCALL_ENOENT;
static bool s_mock_run_boot_prev_is_symlink = false;

static long s_mock_dmesg_ret = 0;
static char s_mock_dmesg_buf[DMESG_SIZE];

static long s_mock_var_open_err = 0;
static long s_mock_run_open_err = 0;
static long s_mock_rename_err = 0;

static bool s_mock_rename_called = false;
static char s_mock_rename_old[64];
static char s_mock_rename_new[64];

static bool s_mock_unlink_called = false;
static char s_mock_unlink_path[64];

static bool s_mock_sync_called = false;
static int s_mock_fchmod_fd = -1;
static uint32_t s_mock_fchmod_mode = 0;

static char s_stdout_buf[4096];
static size_t s_stdout_len = 0;

static char s_stderr_buf[4096];
static size_t s_stderr_len = 0;

static char s_written_file_buf[DMESG_SIZE];
static size_t s_written_file_len = 0;
static int s_active_open_fd = -1;

static bool s_runfs_cap_clamp = false;

static void reset_mock_state(void) {
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

    memset(&s_mock_var_boot_log_st, 0, sizeof(s_mock_var_boot_log_st));
    s_mock_var_boot_log_st.type = VFS_FILE;
    s_mock_var_boot_log_st.mode = VFS_S_IFREG | 0640;
    s_mock_var_boot_log_st.uid = 0;
    s_mock_var_boot_log_st_err = SYSCALL_ENOENT;
    s_mock_var_boot_log_is_symlink = false;

    memset(&s_mock_var_boot_prev_st, 0, sizeof(s_mock_var_boot_prev_st));
    s_mock_var_boot_prev_st.type = VFS_FILE;
    s_mock_var_boot_prev_st.mode = VFS_S_IFREG | 0640;
    s_mock_var_boot_prev_st.uid = 0;
    s_mock_var_boot_prev_st_err = SYSCALL_ENOENT;
    s_mock_var_boot_prev_is_symlink = false;

    memset(&s_mock_run_boot_log_st, 0, sizeof(s_mock_run_boot_log_st));
    s_mock_run_boot_log_st.type = VFS_FILE;
    s_mock_run_boot_log_st.mode = VFS_S_IFREG | 0640;
    s_mock_run_boot_log_st.uid = 0;
    s_mock_run_boot_log_st_err = SYSCALL_ENOENT;
    s_mock_run_boot_log_is_symlink = false;

    memset(&s_mock_run_boot_prev_st, 0, sizeof(s_mock_run_boot_prev_st));
    s_mock_run_boot_prev_st.type = VFS_FILE;
    s_mock_run_boot_prev_st.mode = VFS_S_IFREG | 0640;
    s_mock_run_boot_prev_st.uid = 0;
    s_mock_run_boot_prev_st_err = SYSCALL_ENOENT;
    s_mock_run_boot_prev_is_symlink = false;

    s_mock_dmesg_ret = 0;
    memset(s_mock_dmesg_buf, 0, sizeof(s_mock_dmesg_buf));

    s_mock_var_open_err = 0;
    s_mock_run_open_err = 0;
    s_mock_rename_err = 0;

    s_mock_rename_called = false;
    s_mock_rename_old[0] = '\0';
    s_mock_rename_new[0] = '\0';

    s_mock_unlink_called = false;
    s_mock_unlink_path[0] = '\0';

    s_mock_sync_called = false;
    s_mock_fchmod_fd = -1;
    s_mock_fchmod_mode = 0;

    s_stdout_len = 0;
    s_stdout_buf[0] = '\0';
    s_stderr_len = 0;
    s_stderr_buf[0] = '\0';

    s_written_file_len = 0;
    s_written_file_buf[0] = '\0';
    s_active_open_fd = -1;

    s_runfs_cap_clamp = false;
}

static void safe_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* --- Mock System Calls --- */
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_DMESG) {
        if (s_mock_dmesg_ret < 0) return s_mock_dmesg_ret;
        char *buf = (char *)a;
        size_t cap = (size_t)b;
        size_t n = (size_t)s_mock_dmesg_ret;
        if (n > cap) n = cap;
        if (buf && n > 0) {
            memcpy(buf, s_mock_dmesg_buf, n);
        }
        return (long)n;
    }

    if (nr == SYS_LSTAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (!path || !st) return SYSCALL_EINVAL;

        if (strcmp(path, BOOT_SNAPSHOT_FILE_PRIMARY) == 0) {
            if (s_mock_var_boot_log_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                return 0;
            }
            if (s_mock_var_boot_log_st_err == 0) {
                st->mode = s_mock_var_boot_log_st.mode;
                st->type = s_mock_var_boot_log_st.type;
                st->size = s_mock_var_boot_log_st.file_size;
                return 0;
            }
            return s_mock_var_boot_log_st_err;
        }

        if (strcmp(path, BOOT_SNAPSHOT_PREV_PRIMARY) == 0) {
            if (s_mock_var_boot_prev_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                return 0;
            }
            if (s_mock_var_boot_prev_st_err == 0) {
                st->mode = s_mock_var_boot_prev_st.mode;
                st->type = s_mock_var_boot_prev_st.type;
                st->size = s_mock_var_boot_prev_st.file_size;
                return 0;
            }
            return s_mock_var_boot_prev_st_err;
        }

        if (strcmp(path, BOOT_SNAPSHOT_FILE_FALLBACK) == 0) {
            if (s_mock_run_boot_log_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                return 0;
            }
            if (s_mock_run_boot_log_st_err == 0) {
                st->mode = s_mock_run_boot_log_st.mode;
                st->type = s_mock_run_boot_log_st.type;
                st->size = s_mock_run_boot_log_st.file_size;
                return 0;
            }
            return s_mock_run_boot_log_st_err;
        }

        if (strcmp(path, BOOT_SNAPSHOT_PREV_FALLBACK) == 0) {
            if (s_mock_run_boot_prev_is_symlink) {
                st->mode = VFS_S_IFLNK | 0777;
                st->type = VFS_STREAM;
                return 0;
            }
            if (s_mock_run_boot_prev_st_err == 0) {
                st->mode = s_mock_run_boot_prev_st.mode;
                st->type = s_mock_run_boot_prev_st.type;
                st->size = s_mock_run_boot_prev_st.file_size;
                return 0;
            }
            return s_mock_run_boot_prev_st_err;
        }

        return SYSCALL_ENOENT;
    }

    if (nr == SYS_UNLINK) {
        const char *path = (const char *)a;
        s_mock_unlink_called = true;
        if (path) {
            safe_copy(s_mock_unlink_path, path, sizeof(s_mock_unlink_path));
        }
        return 0;
    }

    if (nr == SYS_RENAME) {
        if (s_mock_rename_err != 0) return s_mock_rename_err;
        const char *oldp = (const char *)a;
        const char *newp = (const char *)b;
        s_mock_rename_called = true;
        if (oldp) safe_copy(s_mock_rename_old, oldp, sizeof(s_mock_rename_old));
        if (newp) safe_copy(s_mock_rename_new, newp, sizeof(s_mock_rename_new));
        return 0;
    }

    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (strcmp(path, BOOT_SNAPSHOT_FILE_PRIMARY) == 0) {
            if (s_mock_var_open_err != 0) return s_mock_var_open_err;
            s_active_open_fd = 10;
            return 10;
        }
        if (strcmp(path, BOOT_SNAPSHOT_FILE_FALLBACK) == 0) {
            if (s_mock_run_open_err != 0) return s_mock_run_open_err;
            s_active_open_fd = 11;
            return 11;
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_FCHMOD) {
        s_mock_fchmod_fd = (int)a;
        s_mock_fchmod_mode = (uint32_t)b;
        return 0;
    }

    if (nr == SYS_CLOSE) {
        s_active_open_fd = -1;
        return 0;
    }

    if (nr == SYS_SYNC) {
        s_mock_sync_called = true;
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
            if (s_runfs_cap_clamp && fd == 11) {
                if (s_written_file_len >= 4096) {
                    return SYSCALL_ENOSPC;
                }
                size_t avail = 4096 - s_written_file_len;
                size_t to_write = (len < avail) ? len : avail;
                memcpy(s_written_file_buf + s_written_file_len, data, to_write);
                s_written_file_len += to_write;
                s_written_file_buf[s_written_file_len] = '\0';
                return (long)to_write;
            }
            if (s_written_file_len + len < sizeof(s_written_file_buf)) {
                memcpy(s_written_file_buf + s_written_file_len, data, len);
                s_written_file_len += len;
                s_written_file_buf[s_written_file_len] = '\0';
            }
            return (long)len;
        }
        return SYSCALL_EBADF;
    }

    return SYSCALL_ENOSYS;
}

long permission_call4(long n, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    (void)c;
    (void)d;
    if (n == SYS_STAT_EXT) {
        const char *path = (const char *)a;
        stat_ext_v1_t *st = (stat_ext_v1_t *)b;
        if (!path || !st) return SYSCALL_EINVAL;

        if (strcmp(path, BOOT_SNAPSHOT_DIR_PRIMARY) == 0) {
            if (s_mock_var_log_stat_err != 0) return s_mock_var_log_stat_err;
            *st = s_mock_var_log_st;
            return 0;
        }
        if (strcmp(path, BOOT_SNAPSHOT_DIR_FALLBACK) == 0) {
            if (s_mock_run_stat_err != 0) return s_mock_run_stat_err;
            *st = s_mock_run_st;
            return 0;
        }
        if (strcmp(path, BOOT_SNAPSHOT_FILE_PRIMARY) == 0) {
            if (s_mock_var_boot_log_st_err != 0) return s_mock_var_boot_log_st_err;
            *st = s_mock_var_boot_log_st;
            return 0;
        }
        if (strcmp(path, BOOT_SNAPSHOT_PREV_PRIMARY) == 0) {
            if (s_mock_var_boot_prev_st_err != 0) return s_mock_var_boot_prev_st_err;
            *st = s_mock_var_boot_prev_st;
            return 0;
        }
        if (strcmp(path, BOOT_SNAPSHOT_FILE_FALLBACK) == 0) {
            if (s_mock_run_boot_log_st_err != 0) return s_mock_run_boot_log_st_err;
            *st = s_mock_run_boot_log_st;
            return 0;
        }
        if (strcmp(path, BOOT_SNAPSHOT_PREV_FALLBACK) == 0) {
            if (s_mock_run_boot_prev_st_err != 0) return s_mock_run_boot_prev_st_err;
            *st = s_mock_run_boot_prev_st;
            return 0;
        }
        return SYSCALL_ENOENT;
    }
    return SYSCALL_ENOSYS;
}

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static void test_boot_snapshot_success_primary(void) {
    printf("[1/11] Testing boot snapshot primary destination success...\n");
    reset_mock_state();

    const char *klog = "<6>PCIe bus enumerated\n<6>ACPI RSDP 0x000e0000\n<6>NVMe controller initialized\n";
    size_t klen = strlen(klog);
    memcpy(s_mock_dmesg_buf, klog, klen);
    s_mock_dmesg_ret = (long)klen;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    /* Assert written data */
    assert(s_written_file_len == klen);
    assert(memcmp(s_written_file_buf, klog, klen) == 0);
    assert(s_mock_fchmod_fd == 10);
    assert(s_mock_fchmod_mode == 0640);
    assert(s_mock_sync_called == true);
    assert(strstr(s_stdout_buf, "boot-snapshot: saved ") != NULL);
    assert(strstr(s_stdout_buf, "/var/log/boot.log") != NULL);
}

static void test_boot_snapshot_prerotate(void) {
    printf("[2/11] Testing pre-rotation of existing active boot.log to boot.log.prev...\n");
    reset_mock_state();

    /* Existing active and prev files */
    s_mock_var_boot_log_st_err = 0;
    s_mock_var_boot_log_st.file_size = 500;
    s_mock_var_boot_prev_st_err = 0;
    s_mock_var_boot_prev_st.file_size = 400;

    const char *new_log = "<6>New boot session starting...\n";
    size_t nlen = strlen(new_log);
    memcpy(s_mock_dmesg_buf, new_log, nlen);
    s_mock_dmesg_ret = (long)nlen;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    /* Assert previous log was unlinked before rename */
    assert(s_mock_unlink_called == true);
    assert(strcmp(s_mock_unlink_path, BOOT_SNAPSHOT_PREV_PRIMARY) == 0);

    /* Assert active was renamed to prev */
    assert(s_mock_rename_called == true);
    assert(strcmp(s_mock_rename_old, BOOT_SNAPSHOT_FILE_PRIMARY) == 0);
    assert(strcmp(s_mock_rename_new, BOOT_SNAPSHOT_PREV_PRIMARY) == 0);

    /* Assert new active was created and written */
    assert(s_written_file_len == nlen);
    assert(memcmp(s_written_file_buf, new_log, nlen) == 0);
}

static void test_boot_snapshot_fallback_missing_var_log(void) {
    printf("[3/11] Testing fallback to /run/boot.log on missing /var/log (ENOENT)...\n");
    reset_mock_state();

    s_mock_var_log_stat_err = SYSCALL_ENOENT;

    const char *klog = "<6>Live media boot detected\n";
    size_t klen = strlen(klog);
    memcpy(s_mock_dmesg_buf, klog, klen);
    s_mock_dmesg_ret = (long)klen;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    /* Assert written to fallback */
    assert(s_written_file_len == klen);
    assert(strstr(s_stdout_buf, "/run/boot.log") != NULL);
}

static void test_boot_snapshot_fallback_rofs_var_log(void) {
    printf("[4/11] Testing fallback to /run/boot.log on read-only /var/log (EROFS)...\n");
    reset_mock_state();

    s_mock_var_log_stat_err = SYSCALL_EROFS;

    const char *klog = "<6>Read-only root detected\n";
    size_t klen = strlen(klog);
    memcpy(s_mock_dmesg_buf, klog, klen);
    s_mock_dmesg_ret = (long)klen;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    assert(s_written_file_len == klen);
    assert(strstr(s_stdout_buf, "/run/boot.log") != NULL);
}

static void test_boot_snapshot_fallback_on_rename_or_open_rofs(void) {
    printf("[5/11] Testing fallback on EROFS during rename or open in /var/log...\n");
    reset_mock_state();

    /* /var/log directory stat succeeds, but open returns EROFS */
    s_mock_var_open_err = SYSCALL_EROFS;

    const char *klog = "<6>Testing open EROFS fallback\n";
    size_t klen = strlen(klog);
    memcpy(s_mock_dmesg_buf, klog, klen);
    s_mock_dmesg_ret = (long)klen;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    assert(s_written_file_len == klen);
    assert(strstr(s_stdout_buf, "/run/boot.log") != NULL);
}

static void test_boot_snapshot_runfs_capacity_clamp(void) {
    printf("[6/11] Testing runfs 4 KiB capacity clamping on /run/boot.log...\n");
    reset_mock_state();

    s_mock_var_log_stat_err = SYSCALL_ENOENT; /* Trigger fallback */
    s_runfs_cap_clamp = true;

    /* Fill dmesg buffer with 8192 bytes */
    for (size_t i = 0; i < 8192; i++) {
        s_mock_dmesg_buf[i] = (char)('A' + (i % 26));
    }
    s_mock_dmesg_ret = 8192;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);

    /* Assert clamped at 4096 bytes without error */
    assert(s_written_file_len == 4096);
    assert(strstr(s_stdout_buf, "4096 bytes to /run/boot.log") != NULL);
}

static void test_boot_snapshot_permission_denied_dmesg(void) {
    printf("[7/11] Testing permission denied rejection on non-root SYS_DMESG (EPERM)...\n");
    reset_mock_state();

    s_mock_dmesg_ret = SYSCALL_EPERM;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "permission denied") != NULL);
}

static void test_boot_snapshot_insecure_directory_rejection(void) {
    printf("[8/11] Testing insecure directory rejection (world-writable / non-root)...\n");
    reset_mock_state();

    /* World-writable directory */
    s_mock_var_log_st.mode = VFS_S_IFDIR | 0777;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "world-writable") != NULL);

    /* Non-root directory */
    reset_mock_state();
    s_mock_var_log_st.uid = 1000;

    r = boot_snapshot_main(1, argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "not owned by root") != NULL);
}

static void test_boot_snapshot_symlink_rejection(void) {
    printf("[9/11] Testing symlink rejection on active or prev log file...\n");
    reset_mock_state();

    /* Active is a symlink */
    s_mock_var_boot_log_is_symlink = true;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "symlink rejected") != NULL);

    /* Prev is a symlink */
    reset_mock_state();
    s_mock_var_boot_prev_is_symlink = true;

    r = boot_snapshot_main(1, argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "symlink rejected") != NULL);
}

static void test_boot_snapshot_cli_options(void) {
    printf("[10/11] Testing CLI argument parsing (-h, --help, unrecognized)...\n");
    reset_mock_state();

    char *h_argv[] = {"boot-snapshot", "-h", NULL};
    int r = boot_snapshot_main(2, h_argv);
    assert(r == 0);
    assert(strstr(s_stdout_buf, "Usage: boot-snapshot") != NULL);

    reset_mock_state();
    char *help_argv[] = {"boot-snapshot", "--help", NULL};
    r = boot_snapshot_main(2, help_argv);
    assert(r == 0);
    assert(strstr(s_stdout_buf, "Usage: boot-snapshot") != NULL);

    reset_mock_state();
    char *bad_argv[] = {"boot-snapshot", "--invalid", NULL};
    r = boot_snapshot_main(2, bad_argv);
    assert(r == 1);
    assert(strstr(s_stderr_buf, "unrecognized option") != NULL);
}

static void test_boot_snapshot_empty_dmesg(void) {
    printf("[11/11] Testing empty kernel ring buffer handling...\n");
    reset_mock_state();

    s_mock_dmesg_ret = 0;

    char *argv[] = {"boot-snapshot", NULL};
    int r = boot_snapshot_main(1, argv);
    assert(r == 0);
    assert(s_written_file_len == 0);
    assert(strstr(s_stdout_buf, "0 bytes to /var/log/boot.log") != NULL);
}

int main(void) {
    printf("====================================================\n");
    printf("Running boot-snapshot host test suite (ASan/UBSan)\n");
    printf("====================================================\n");

    test_boot_snapshot_success_primary();
    test_boot_snapshot_prerotate();
    test_boot_snapshot_fallback_missing_var_log();
    test_boot_snapshot_fallback_rofs_var_log();
    test_boot_snapshot_fallback_on_rename_or_open_rofs();
    test_boot_snapshot_runfs_capacity_clamp();
    test_boot_snapshot_permission_denied_dmesg();
    test_boot_snapshot_insecure_directory_rejection();
    test_boot_snapshot_symlink_rejection();
    test_boot_snapshot_cli_options();
    test_boot_snapshot_empty_dmesg();

    printf("====================================================\n");
    printf("All 11 boot-snapshot test suites passed successfully!\n");
    printf("====================================================\n");
    return 0;
}
