#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "common.h"
#include "creds.h"

extern int date_main(int argc, char **argv, const char *const *envp);

static timespec_t mock_realtime;
static long mock_clock_ret = 0;

static char mock_stdout[4096];
static size_t mock_stdout_len = 0;
static char mock_stderr[4096];
static size_t mock_stderr_len = 0;

static char mock_timezone[256];
static bool mock_timezone_exists = false;
static char mock_timezone_tmp[256];
static bool mock_timezone_tmp_exists = false;
static char mock_localtime[256];
static bool mock_localtime_exists = false;

static uint32_t mock_euid = 0;
static uint64_t mock_caps = CAP_SYS_ADMIN;
static bool mock_rename_replace_called = false;
static bool mock_rename_called = false;

    static size_t fd10_pos = 0;
    static size_t fd11_pos = 0;
    static size_t fd12_pos = 0;
static void copy_str(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

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

    if (nr == SYS_GETRESUID) {
        uint32_t *ruid = (uint32_t *)a;
        uint32_t *euid = (uint32_t *)b;
        uint32_t *suid = (uint32_t *)c;
        if (ruid) *ruid = mock_euid;
        if (euid) *euid = mock_euid;
        if (suid) *suid = mock_euid;
        return 0;
    }

    if (nr == SYS_CAPGET) {
        return (long)mock_caps;
    }

    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (!strcmp(path, "/etc/timezone")) {
            if (!mock_timezone_exists) return SYSCALL_ENOENT;
            fd10_pos = 0;
            return 10;
        }
        if (!strcmp(path, "/etc/timezone.tmp")) {
            if ((b & VFS_O_CREAT) || (b & VFS_O_WRONLY) || (b & VFS_O_TRUNC)) {
                mock_timezone_tmp[0] = '\0';
                mock_timezone_tmp_exists = true;
                fd11_pos = 0;
                return 11;
            }
            if (!mock_timezone_tmp_exists) return SYSCALL_ENOENT;
            fd11_pos = 0;
            return 11;
        }
        if (!strcmp(path, "/etc/localtime")) {
            if (!mock_localtime_exists) return SYSCALL_ENOENT;
            fd12_pos = 0;
            return 12;
        }
        if (!strcmp(path, "/etc/timezone.dhcp") || !strcmp(path, "/run/timezone")) {
            return SYSCALL_ENOENT;
        }

        /* Check for /usr/share/zoneinfo/ or /etc/zoneinfo/ or build/zoneinfo/ */
        const char *sub = NULL;
        if (!strncmp(path, "/usr/share/zoneinfo/", 20)) {
            sub = path + 20;
        } else if (!strncmp(path, "/etc/zoneinfo/", 14)) {
            sub = path + 14;
        } else if (!strncmp(path, "build/zoneinfo/", 15)) {
            sub = path + 15;
        }
        if (sub) {
            char hostpath[256];
            snprintf(hostpath, sizeof(hostpath), "build/zoneinfo/%s", sub);
            int fd = open(hostpath, O_RDONLY);
            if (fd >= 0) return 100 + fd;
            return SYSCALL_ENOENT;
        }
        return SYSCALL_ENOENT;
    }

    if (nr == SYS_READ) {
        long fd = (long)a;
        void *dst = (void *)b;
        size_t count = (size_t)c;
        if (fd == 10) {
            size_t total = strlen(mock_timezone);
            if (fd10_pos >= total) return 0;
            size_t avail = total - fd10_pos;
            if (avail > count) avail = count;
            memcpy(dst, mock_timezone + fd10_pos, avail);
            fd10_pos += avail;
            return (long)avail;
        }
        if (fd == 11) {
            size_t total = strlen(mock_timezone_tmp);
            if (fd11_pos >= total) return 0;
            size_t avail = total - fd11_pos;
            if (avail > count) avail = count;
            memcpy(dst, mock_timezone_tmp + fd11_pos, avail);
            fd11_pos += avail;
            return (long)avail;
        }
        if (fd == 12) {
            size_t total = strlen(mock_localtime);
            if (fd12_pos >= total) return 0;
            size_t avail = total - fd12_pos;
            if (avail > count) avail = count;
            memcpy(dst, mock_localtime + fd12_pos, avail);
            fd12_pos += avail;
            return (long)avail;
        }
        if (fd >= 100) {
            int host_fd = (int)(fd - 100);
            ssize_t n = read(host_fd, dst, count);
            if (n < 0) return SYSCALL_EIO;
            return (long)n;
        }
        return SYSCALL_EBADF;
    }

    if (nr == SYS_WRITE) {
        long fd = (long)a;
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
        if (fd == 11) {
            if (fd11_pos + len < sizeof(mock_timezone_tmp)) {
                memcpy(mock_timezone_tmp + fd11_pos, data, len);
                fd11_pos += len;
                mock_timezone_tmp[fd11_pos] = '\0';
            }
            return (long)len;
        }
        return SYSCALL_EBADF;
    }

    if (nr == SYS_CLOSE) {
        long fd = (long)a;
        if (fd >= 100) {
            close((int)(fd - 100));
        }
        return 0;
    }

    if (nr == SYS_RENAME_REPLACE) {
        mock_rename_replace_called = true;
        const char *oldpath = (const char *)a;
        const char *newpath = (const char *)b;
        if (!strcmp(oldpath, "/etc/timezone.tmp") && !strcmp(newpath, "/etc/timezone")) {
            copy_str(mock_timezone, mock_timezone_tmp, sizeof(mock_timezone));
            mock_timezone_exists = true;
            mock_timezone_tmp_exists = false;
            return 0;
        }
        return SYSCALL_EINVAL;
    }

    if (nr == SYS_RENAME) {
        mock_rename_called = true;
        const char *oldpath = (const char *)a;
        const char *newpath = (const char *)b;
        if (!strcmp(oldpath, "/etc/timezone.tmp") && !strcmp(newpath, "/etc/timezone")) {
            copy_str(mock_timezone, mock_timezone_tmp, sizeof(mock_timezone));
            mock_timezone_exists = true;
            mock_timezone_tmp_exists = false;
            return 0;
        }
        return SYSCALL_EINVAL;
    }

    if (nr == SYS_UNLINK) {
        const char *path = (const char *)a;
        if (!strcmp(path, "/etc/timezone.tmp")) {
            mock_timezone_tmp_exists = false;
            mock_timezone_tmp[0] = '\0';
            return 0;
        }
        if (!strcmp(path, "/etc/timezone")) {
            mock_timezone_exists = false;
            mock_timezone[0] = '\0';
            return 0;
        }
        return SYSCALL_ENOENT;
    }

    return SYSCALL_ENOSYS;
}

static void reset_buffers(void) {
    mock_stdout_len = 0;
    mock_stdout[0] = '\0';
    mock_stderr_len = 0;
    mock_stderr[0] = '\0';
    mock_clock_ret = 0;
    mock_euid = 0;
    mock_caps = CAP_SYS_ADMIN;
    mock_rename_replace_called = false;
    mock_rename_called = false;
}

int main(void) {
    printf("=== Comprehensive /bin/date Host Test Suite ===\n");

    /* Reference timestamp: 1792139445 = 2026-10-16 08:30:45 UTC */
    mock_realtime.tv_sec = 1792139445;
    mock_realtime.tv_nsec = 0;

    /* -------------------------------------------------------------
     * Test 1: Help Menu (--help)
     * ------------------------------------------------------------- */
    printf("[1/6] Testing --help and -h formatting...\n");
    reset_buffers();
    char *argv_help[] = {"date", "--help", NULL};
    int ret = date_main(2, argv_help, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "Usage: date [OPTION]... [+FORMAT]\n       date set TZ=<Region/City>") != NULL);
    assert(strstr(mock_stdout, "Format specifiers (via +FORMAT):") != NULL);
    assert(strstr(mock_stdout, "%Y   Year (4 digits)") != NULL);
    assert(strstr(mock_stdout, "%s   Seconds since epoch") != NULL);
    assert(strstr(mock_stdout, "sudo date set TZ=Europe/Brussels") != NULL);

    reset_buffers();
    char *argv_h[] = {"date", "-h", NULL};
    assert(date_main(2, argv_h, NULL) == 0);
    assert(strstr(mock_stdout, "Usage: date") != NULL);
    printf("  [PASS] Help menu formatted and concise.\n");

    /* -------------------------------------------------------------
     * Test 2: 'date set TZ=...' Privileges and Syntax Validation
     * ------------------------------------------------------------- */
    printf("[2/6] Testing date set TZ=... permissions and validation...\n");
    mock_timezone_exists = false;
    mock_timezone[0] = '\0';

    /* Unprivileged caller (EUID 1000, no caps) */
    reset_buffers();
    mock_euid = 1000;
    mock_caps = 0;
    char *argv_set_unpriv[] = {"date", "set", "TZ=Europe/Brussels", NULL};
    ret = date_main(3, argv_set_unpriv, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "permission denied") != NULL);
    assert(!mock_timezone_exists);

    /* Privilege granted via CAP_SYS_ADMIN even if EUID != 0 */
    reset_buffers();
    mock_euid = 1000;
    mock_caps = CAP_SYS_ADMIN;
    ret = date_main(3, argv_set_unpriv, NULL);
    assert(ret == 0);
    assert(mock_rename_replace_called);
    assert(strcmp(mock_timezone, "Europe/Brussels\n") == 0);
    assert(strcmp(mock_stdout, "Timezone set to Europe/Brussels.\n") == 0);

    /* Invalid timezone: non-existent / bogus */
    reset_buffers();
    mock_euid = 0;
    mock_caps = CAP_SYS_ADMIN;
    char *argv_set_bogus[] = {"date", "set", "TZ=Invalid/Timezone", NULL};
    ret = date_main(3, argv_set_bogus, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "invalid or unavailable timezone") != NULL);

    /* Path traversal attempt */
    reset_buffers();
    char *argv_set_traversal[] = {"date", "set", "TZ=../../etc/shadow", NULL};
    ret = date_main(3, argv_set_traversal, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "invalid or unavailable timezone") != NULL);

    /* Empty TZ= */
    reset_buffers();
    char *argv_set_empty[] = {"date", "set", "TZ=", NULL};
    ret = date_main(3, argv_set_empty, NULL);
    assert(ret != 0);

    /* Malformed syntax */
    reset_buffers();
    char *argv_set_bad_syntax[] = {"date", "set", "Europe/Brussels", NULL};
    ret = date_main(3, argv_set_bad_syntax, NULL);
    assert(ret != 0);

    /* Successful atomic persistent write via SYS_RENAME_REPLACE */
    reset_buffers();
    mock_rename_replace_called = false;
    char *argv_set_valid[] = {"date", "set", "TZ=Europe/Brussels", NULL};
    ret = date_main(3, argv_set_valid, NULL);
    assert(ret == 0);
    assert(mock_rename_replace_called);
    assert(strcmp(mock_timezone, "Europe/Brussels\n") == 0);
    assert(strcmp(mock_stdout, "Timezone set to Europe/Brussels.\n") == 0);
    printf("  [PASS] date set TZ=... validates and atomically persists.\n");

    /* -------------------------------------------------------------
     * Test 3: Timezone Resolution (Query Mode)
     * ------------------------------------------------------------- */
    printf("[3/6] Testing query mode timezone resolution order...\n");

    /* With /etc/timezone containing Europe/Brussels:
     * 1792139445 is 2026-10-16 08:30:45 UTC.
     * Brussels is CEST (UTC+2) in mid-October.
     * Local time: 10:30:45 CEST 2026. */
    reset_buffers();
    char *argv_plain[] = {"date", NULL};
    ret = date_main(1, argv_plain, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "10:30:45") != NULL);
    assert(strstr(mock_stdout, "CEST") != NULL);

    /* TZ env var overrides /etc/timezone */
    reset_buffers();
    const char *env_utc[] = {"TZ=UTC", NULL};
    ret = date_main(1, argv_plain, env_utc);
    assert(ret == 0);
    assert(strstr(mock_stdout, "08:30:45 UTC") != NULL);

    /* Fallback to /etc/localtime when /etc/timezone is missing */
    mock_timezone_exists = false;
    mock_timezone[0] = '\0';
    mock_localtime_exists = true;
    copy_str(mock_localtime, "Europe/Brussels\n", sizeof(mock_localtime));
    reset_buffers();
    ret = date_main(1, argv_plain, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "10:30:45 CEST") != NULL);

    /* Fallback to UTC when both /etc/timezone and /etc/localtime are missing */
    mock_localtime_exists = false;
    mock_localtime[0] = '\0';
    reset_buffers();
    ret = date_main(1, argv_plain, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "08:30:45 UTC 2026") != NULL);

    printf("  [PASS] Timezone resolution order verified (TZ > /etc/timezone > /etc/localtime > UTC).\n");

    /* -------------------------------------------------------------
     * Test 4: Custom Formatting (+FORMAT)
     * ------------------------------------------------------------- */
    printf("[4/6] Testing custom format strings (+FORMAT)...\n");

    /* Reset timezone to Brussels */
    mock_timezone_exists = true;
    copy_str(mock_timezone, "Europe/Brussels\n", sizeof(mock_timezone));

    /* %Y-%m-%d */
    reset_buffers();
    char *argv_ymd[] = {"date", "+%Y-%m-%d", NULL};
    ret = date_main(2, argv_ymd, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16\n") == 0);

    /* %Y-%m-%d %H:%M:%S in local Brussels time (10:30:45) */
    reset_buffers();
    char *argv_full[] = {"date", "+%Y-%m-%d %H:%M:%S", NULL};
    ret = date_main(2, argv_full, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16 10:30:45\n") == 0);

    /* date -u +"%Y-%m-%d %H:%M:%S" in UTC (08:30:45) */
    reset_buffers();
    char *argv_utc_full[] = {"date", "-u", "+%Y-%m-%d %H:%M:%S", NULL};
    ret = date_main(3, argv_utc_full, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16 08:30:45\n") == 0);

    /* Big 6 comprehensive test with literal %% */
    reset_buffers();
    char *argv_big6[] = {"date", "+Y=%Y m=%m d=%d H=%H M=%M S=%S s=%s %%", NULL};
    ret = date_main(2, argv_big6, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Y=2026 m=10 d=16 H=10 M=30 S=45 s=1792139445 %\n") == 0);

    /* Rejection of unsupported specifiers */
    reset_buffers();
    char *argv_bad_spec1[] = {"date", "+%Z", NULL};
    ret = date_main(2, argv_bad_spec1, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "unsupported format specifier") != NULL);

    reset_buffers();
    char *argv_bad_spec2[] = {"date", "+%Y-%m-%d %X", NULL};
    ret = date_main(2, argv_bad_spec2, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "unsupported format specifier") != NULL);

    reset_buffers();
    char *argv_trailing_pct[] = {"date", "+%Y-%m-", NULL};
    argv_trailing_pct[1] = "+%Y-%m-%";
    ret = date_main(2, argv_trailing_pct, NULL);
    assert(ret != 0);
    assert(strstr(mock_stderr, "unsupported format specifier") != NULL);

    printf("  [PASS] Custom format specifiers (+FORMAT) verified.\n");

    /* -------------------------------------------------------------
     * Test 5: Epoch Seconds (+%s) & Edge Cases
     * ------------------------------------------------------------- */
    printf("[5/6] Testing epoch seconds (+%%s) and boundary conditions...\n");

    /* Epoch with standard +%s */
    reset_buffers();
    char *argv_epoch[] = {"date", "+%s", NULL};
    ret = date_main(2, argv_epoch, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "1792139445\n") == 0);

    /* Negative epoch (-1 = 1969-12-31 23:59:59 UTC) */
    mock_realtime.tv_sec = -1;
    reset_buffers();
    ret = date_main(2, argv_epoch, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "-1\n") == 0);

    /* Corrupt /etc/timezone must NOT fail date +%s */
    mock_realtime.tv_sec = 1792139445;
    copy_str(mock_timezone, "../../etc/shadow\n", sizeof(mock_timezone));
    reset_buffers();
    ret = date_main(2, argv_epoch, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "1792139445\n") == 0);

    printf("  [PASS] Epoch seconds and resilience verified.\n");

    /* -------------------------------------------------------------
     * Test 6: Standard Options (-u, -I, -R) Regressions
     * ------------------------------------------------------------- */
    printf("[6/6] Verifying standard options (-u, -I, -R) zero-regression...\n");

    /* Reset timezone to UTC for predictable output */
    mock_timezone_exists = false;

    /* date -u */
    reset_buffers();
    char *argv_u[] = {"date", "-u", NULL};
    ret = date_main(2, argv_u, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri Oct 16 08:30:45 UTC 2026\n") == 0);

    /* date -I */
    reset_buffers();
    char *argv_iso[] = {"date", "-I", NULL};
    ret = date_main(2, argv_iso, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16T08:30:45Z\n") == 0);

    /* date -R */
    reset_buffers();
    char *argv_rfc[] = {"date", "-R", NULL};
    ret = date_main(2, argv_rfc, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri, 16 Oct 2026 08:30:45 +0000\n") == 0);

    printf("  [PASS] Standard options (-u, -I, -R) verified.\n");

    printf("\n>>> ALL /bin/date HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
