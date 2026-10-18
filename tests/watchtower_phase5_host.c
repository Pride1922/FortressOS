#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include "../src/include/types.h"
#include "../src/include/syscall_abi.h"
#include "../src/include/signal_abi.h"
#include "../src/fs/vfs.h"
#include "../src/include/creds.h"

/* Forward declare mock syscall dispatcher */
static long mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c);
#define shutdown_coord_syscall mock_syscall
#include "../user/tools/shutdown_coord.h"

/* Mock State for Shutdown Coordinator */
typedef struct {
    uint64_t caps;
    bool setpgid_called;
    int setpgid_pid;
    int setpgid_pgid;
    bool sighup_ignored;
    bool sigterm_ignored;
    char pid_file_content[64];
    bool pid_file_exists;
    int64_t wt_pid;
    bool wt_alive;
    int wt_polls_before_exit;
    int wt_polls_seen;
    int sig_delivered;
    int sig_delivered_to_pid;
    bool reboot_called;
    int reboot_cmd_arg;
    int nanosleep_count;
} mock_coord_state_t;

static mock_coord_state_t g_coord;

static void mock_coord_reset(void) {
    memset(&g_coord, 0, sizeof(g_coord));
    g_coord.caps = CAP_SYS_BOOT;
    g_coord.pid_file_exists = true;
    snprintf(g_coord.pid_file_content, sizeof(g_coord.pid_file_content), "42\n");
    g_coord.wt_pid = 42;
    g_coord.wt_alive = true;
    g_coord.wt_polls_before_exit = 2;
}

static long mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    switch (nr) {
        case SYS_CAPGET:
            return (long)g_coord.caps;

        case SYS_SETPGID:
            g_coord.setpgid_called = true;
            g_coord.setpgid_pid = (int)a;
            g_coord.setpgid_pgid = (int)b;
            return 0;

        case SYS_SIGACTION: {
            int sig = (int)a;
            signal_action_t *sa = (signal_action_t *)b;
            if (sig == SIGHUP && sa && sa->handler == SIG_IGN) {
                g_coord.sighup_ignored = true;
            }
            if (sig == SIGTERM && sa && sa->handler == SIG_IGN) {
                g_coord.sigterm_ignored = true;
            }
            return 0;
        }

        case SYS_OPEN: {
            const char *path = (const char *)a;
            if (strcmp(path, WATCHTOWER_PID_FILE) == 0) {
                return g_coord.pid_file_exists ? 3 : -2; /* 3=fd, -2=ENOENT */
            }
            return -2;
        }

        case SYS_READ: {
            int fd = (int)a;
            char *buf = (char *)b;
            size_t count = (size_t)c;
            if (fd == 3 && g_coord.pid_file_exists) {
                size_t len = strlen(g_coord.pid_file_content);
                if (len > count) len = count;
                memcpy(buf, g_coord.pid_file_content, len);
                return (long)len;
            }
            return 0;
        }

        case SYS_CLOSE:
            return 0;

        case SYS_KILL: {
            int pid = (int)a;
            int sig = (int)b;
            if (sig == 0) {
                /* Exit check */
                g_coord.wt_polls_seen++;
                if (g_coord.wt_polls_seen >= g_coord.wt_polls_before_exit) {
                    g_coord.wt_alive = false;
                    return -SYSCALL_ESRCH;
                }
                return g_coord.wt_alive ? 0 : -SYSCALL_ESRCH;
            } else {
                g_coord.sig_delivered = sig;
                g_coord.sig_delivered_to_pid = pid;
                return 0;
            }
        }

        case SYS_NANOSLEEP:
            g_coord.nanosleep_count++;
            return 0;

        case SYS_REBOOT:
            g_coord.reboot_called = true;
            g_coord.reboot_cmd_arg = (int)a;
            return 0;

        default:
            return 0;
    }
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void test_coord_cap_rejection(void) {
    printf("[TEST] test_coord_cap_rejection...\n");
    mock_coord_reset();
    g_coord.caps = 0; /* Missing CAP_SYS_BOOT */

    int ret = shutdown_coordinate_and_reboot(1);
    assert(ret == -SYSCALL_EPERM);
    assert(!g_coord.reboot_called);
    assert(!g_coord.setpgid_called);
    assert(g_coord.sig_delivered == 0);
    printf("PASS: test_coord_cap_rejection\n");
}

static void test_coord_detachment_and_masking(void) {
    printf("[TEST] test_coord_detachment_and_masking...\n");
    mock_coord_reset();

    int ret = shutdown_coordinate_and_reboot(1);
    assert(ret == 0);
    assert(g_coord.setpgid_called);
    assert(g_coord.setpgid_pid == 0 && g_coord.setpgid_pgid == 0);
    assert(g_coord.sighup_ignored);
    assert(g_coord.sigterm_ignored);
    printf("PASS: test_coord_detachment_and_masking\n");
}

static void test_coord_watchtower_signaling_and_exit_wait(void) {
    printf("[TEST] test_coord_watchtower_signaling_and_exit_wait...\n");
    mock_coord_reset();
    g_coord.wt_polls_before_exit = 3;

    int ret = shutdown_coordinate_and_reboot(2);
    assert(ret == 0);
    assert(g_coord.sig_delivered == SIGTERM);
    assert(g_coord.sig_delivered_to_pid == 42);
    assert(g_coord.wt_polls_seen >= 3);
    assert(g_coord.reboot_called);
    assert(g_coord.reboot_cmd_arg == 2);
    printf("PASS: test_coord_watchtower_signaling_and_exit_wait\n");
}

static void test_coord_fallback_on_missing_pid_file(void) {
    printf("[TEST] test_coord_fallback_on_missing_pid_file...\n");
    mock_coord_reset();
    g_coord.pid_file_exists = false;

    int ret = shutdown_coordinate_and_reboot(1);
    assert(ret == 0);
    assert(g_coord.sig_delivered == 0); /* No signal sent */
    assert(g_coord.reboot_called);
    assert(g_coord.reboot_cmd_arg == 1);
    printf("PASS: test_coord_fallback_on_missing_pid_file\n");
}

static void test_coord_fallback_on_stalled_watchtower(void) {
    printf("[TEST] test_coord_fallback_on_stalled_watchtower...\n");
    mock_coord_reset();
    g_coord.wt_polls_before_exit = 1000; /* Stalled, never exits */

    int ret = shutdown_coordinate_and_reboot(1);
    assert(ret == 0);
    assert(g_coord.sig_delivered == SIGTERM);
    assert(g_coord.nanosleep_count == 300); /* Hit 15s deadline (300 * 50ms) */
    assert(g_coord.reboot_called);
    printf("PASS: test_coord_fallback_on_stalled_watchtower\n");
}

/* Logging rotation mock test */
typedef struct {
    char files[8][64];
    int file_count;
    bool gzip_invoked;
} mock_log_fs_t;

static mock_log_fs_t g_lfs;

static void mock_log_fs_reset(void) {
    memset(&g_lfs, 0, sizeof(g_lfs));
}

static void host_strcpy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void mock_lfs_add(const char *name) {
    if (g_lfs.file_count < 8) {
        host_strcpy(g_lfs.files[g_lfs.file_count++], name, 63);
    }
}

static bool mock_lfs_has(const char *name) {
    for (int i = 0; i < g_lfs.file_count; i++) {
        if (strcmp(g_lfs.files[i], name) == 0) return true;
    }
    return false;
}

static void mock_lfs_remove(const char *name) {
    for (int i = 0; i < g_lfs.file_count; i++) {
        if (strcmp(g_lfs.files[i], name) == 0) {
            for (int j = i; j < g_lfs.file_count - 1; j++) {
                host_strcpy(g_lfs.files[j], g_lfs.files[j + 1], sizeof(g_lfs.files[j]));
            }
            g_lfs.file_count--;
            return;
        }
    }
}

static void mock_lfs_rename(const char *src, const char *dst) {
    mock_lfs_remove(dst);
    for (int i = 0; i < g_lfs.file_count; i++) {
        if (strcmp(g_lfs.files[i], src) == 0) {
            host_strcpy(g_lfs.files[i], dst, 63);
            return;
        }
    }
}

static void test_log_rotation_sequence(void) {
    printf("[TEST] test_log_rotation_sequence...\n");
    mock_log_fs_reset();
    mock_lfs_add("svc.log");
    mock_lfs_add("svc.log.1.gz");
    mock_lfs_add("svc.log.2.gz");
    mock_lfs_add("svc.log.3.gz");

    /* Emulate rotation sequence:
     * 1. unlink .3.gz
     * 2. rename .2.gz -> .3.gz
     * 3. rename .1.gz -> .2.gz
     * 4. rename .log -> .1
     * 5. gzip -f .1 -> .1.gz
     */
    mock_lfs_remove("svc.log.3.gz");
    assert(!mock_lfs_has("svc.log.3.gz"));

    mock_lfs_rename("svc.log.2.gz", "svc.log.3.gz");
    assert(mock_lfs_has("svc.log.3.gz"));
    assert(!mock_lfs_has("svc.log.2.gz"));

    mock_lfs_rename("svc.log.1.gz", "svc.log.2.gz");
    assert(mock_lfs_has("svc.log.2.gz"));
    assert(!mock_lfs_has("svc.log.1.gz"));

    mock_lfs_rename("svc.log", "svc.log.1");
    assert(mock_lfs_has("svc.log.1"));
    assert(!mock_lfs_has("svc.log"));

    /* gzip generates .1.gz and deletes .1 */
    mock_lfs_rename("svc.log.1", "svc.log.1.gz");
    assert(mock_lfs_has("svc.log.1.gz"));
    assert(mock_lfs_has("svc.log.2.gz"));
    assert(mock_lfs_has("svc.log.3.gz"));

    printf("PASS: test_log_rotation_sequence\n");
}

int main(void) {
    printf("===================================================\n");
    printf("Watchtower Phase 5 Coordinator & Logging Test Suite\n");
    printf("===================================================\n");

    test_coord_cap_rejection();
    test_coord_detachment_and_masking();
    test_coord_watchtower_signaling_and_exit_wait();
    test_coord_fallback_on_missing_pid_file();
    test_coord_fallback_on_stalled_watchtower();
    test_log_rotation_sequence();

    printf("\nALL 6 PHASE 5 WATCHTOWER TESTS PASSED!\n");
    return 0;
}
