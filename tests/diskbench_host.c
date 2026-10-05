/* Host unit test suite for diskbench with mocked syscalls under ASan/UBSan */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "common.h"
#include "syscall_abi.h"
#include "signal_abi.h"

typedef struct {
    char name[256];
    bool open;
    bool is_dir;
    size_t size;
    size_t pos;
    unsigned char data[65536];
} mock_entry_t;

#define MAX_MOCK_ENTRIES 128
static mock_entry_t entries[MAX_MOCK_ENTRIES];
static size_t entry_count = 0;

static void safe_strcpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char stdout_buf[65536];
static size_t stdout_len = 0;
static char stderr_buf[8192];
static size_t stderr_len = 0;

static uint64_t mock_uptime_ticks = 1000;
static uint64_t mock_tick_hz = 100;
static uint64_t sigint_handler = 0;
static uint64_t sigterm_handler = 0;

static jmp_buf exit_jmp;
static bool intercept_exit = false;
static int last_exit_code = 0;
static bool trigger_signal_on_meta_open = false;

static void reset_mock(void) {
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    stdout_len = 0;
    stdout_buf[0] = '\0';
    stderr_len = 0;
    stderr_buf[0] = '\0';
    mock_uptime_ticks = 1000;
    mock_tick_hz = 100;
    sigint_handler = 0;
    sigterm_handler = 0;
    intercept_exit = false;
    last_exit_code = 0;
    trigger_signal_on_meta_open = false;
}

static mock_entry_t *find_entry(const char *name) {
    for (size_t i = 0; i < entry_count; i++) {
        if (!strcmp(entries[i].name, name)) {
            return &entries[i];
        }
    }
    return NULL;
}

static size_t active_entry_count(void) {
    size_t active = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].name[0] != '\0') active++;
    }
    return active;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_SYSINFO) {
        sysinfo_t *info = (sysinfo_t *)a;
        if (!info) return SYSCALL_EFAULT;
        memset(info, 0, sizeof(*info));
        info->uptime_ticks = mock_uptime_ticks;
        info->tick_hz = mock_tick_hz;
        mock_uptime_ticks += 25; /* Advance 250 ms every time sysinfo is sampled */
        return 0;
    }
    if (nr == SYS_SIGACTION) {
        int sig = (int)a;
        const signal_action_t *sa = (const signal_action_t *)b;
        if (sig == SIGINT) sigint_handler = sa ? sa->handler : 0;
        if (sig == SIGTERM) sigterm_handler = sa ? sa->handler : 0;
        return 0;
    }
    if (nr == SYS_DMESG) {
        char *buf = (char *)a;
        size_t cap = (size_t)b;
        const char *msg = "[    0.050000] ext2: mounted /dev/nvme0n1 on /mnt (read-write)\n";
        size_t len = strlen(msg);
        if (len > cap) len = cap;
        memcpy(buf, msg, len);
        return (long)len;
    }
    if (nr == SYS_MKDIR) {
        const char *path = (const char *)a;
        mock_entry_t *e = find_entry(path);
        if (!e) {
            assert(entry_count < MAX_MOCK_ENTRIES);
            e = &entries[entry_count++];
            safe_strcpy(e->name, path, sizeof(e->name));
        }
        e->is_dir = true;
        return 0;
    }
    if (nr == SYS_UNLINK) {
        const char *path = (const char *)a;
        mock_entry_t *e = find_entry(path);
        if (e) {
            e->name[0] = '\0';
            e->open = false;
            e->size = 0;
            e->pos = 0;
        }
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        int flags = (int)b;

        if (trigger_signal_on_meta_open && strstr(path, "m_0005")) {
            /* Simulate SIGINT arrival during metadata test */
            void (*handler)(int) = (void (*)(int))sigint_handler;
            assert(handler != NULL);
            handler(SIGINT);
            return -1;
        }

        mock_entry_t *e = find_entry(path);
        if (!e) {
            if (!(flags & VFS_O_CREAT)) return SYSCALL_ENOENT;
            assert(entry_count < MAX_MOCK_ENTRIES);
            e = &entries[entry_count++];
            safe_strcpy(e->name, path, sizeof(e->name));
            e->is_dir = false;
            e->size = 0;
        }
        e->open = true;
        e->pos = 0;
        if (flags & VFS_O_TRUNC) e->size = 0;
        int fd = (int)(e - entries) + 10;
        return fd;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (fd >= 10 && fd < (int)(MAX_MOCK_ENTRIES + 10)) {
            entries[fd - 10].open = false;
        }
        return 0;
    }
    if (nr == SYS_READ) {
        int fd = (int)a;
        if (fd < 10 || fd >= (int)(MAX_MOCK_ENTRIES + 10)) return SYSCALL_EBADF;
        mock_entry_t *e = &entries[fd - 10];
        if (!e->open) return SYSCALL_EBADF;
        size_t count = (size_t)c;
        if (e->pos >= e->size) return 0;
        size_t avail = e->size - e->pos;
        if (count > avail) count = avail;
        char *dst = (char *)b;
        for (size_t i = 0; i < count; i++) {
            dst[i] = (char)((e->pos + i) & 0xFF);
        }
        e->pos += count;
        return (long)count;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *src = (const char *)b;
        size_t count = (size_t)c;
        if (fd == 1) {
            if (stdout_len + count < sizeof(stdout_buf) - 1) {
                memcpy(stdout_buf + stdout_len, src, count);
                stdout_len += count;
                stdout_buf[stdout_len] = '\0';
            }
            return (long)count;
        }
        if (fd == 2) {
            if (stderr_len + count < sizeof(stderr_buf) - 1) {
                memcpy(stderr_buf + stderr_len, src, count);
                stderr_len += count;
                stderr_buf[stderr_len] = '\0';
            }
            return (long)count;
        }
        if (fd >= 10 && fd < (int)(MAX_MOCK_ENTRIES + 10)) {
            mock_entry_t *e = &entries[fd - 10];
            if (!e->open) return SYSCALL_EBADF;
            e->pos += count;
            if (e->pos > e->size) e->size = e->pos;
            return (long)count;
        }
        return SYSCALL_EBADF;
    }
    if (nr == SYS_EXIT) {
        last_exit_code = (int)a;
        if (intercept_exit) {
            longjmp(exit_jmp, 1);
        }
        return 0;
    }
    return 0;
}

int main(void) {
    /* Test 1: --help option */
    {
        reset_mock();
        char *argv[] = {"diskbench", "--help"};
        int rc = diskbench_main(2, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Usage: diskbench") != NULL);
        assert(strstr(stdout_buf, "-w SIZE") != NULL);
        assert(strstr(stdout_buf, "-n COUNT") != NULL);
        assert(strstr(stdout_buf, "-t TEST") != NULL);
        assert(strstr(stdout_buf, "-c") != NULL);
        assert(strstr(stdout_buf, "-s") != NULL);
        printf("PASS: test 1 (--help)\n");
    }

    /* Test 2: Standard run with small size and count, verifies timing and throughput calculation */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-w", "128K", "-n", "10", "/mnt"};
        int rc = diskbench_main(5, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "=== diskbench: /mnt ===") != NULL);
        assert(strstr(stdout_buf, "Filesystem:   ext2") != NULL);
        assert(strstr(stdout_buf, "Write: 131072 bytes in ") != NULL);
        assert(strstr(stdout_buf, "Read:  131072 bytes in ") != NULL);
        assert(strstr(stdout_buf, "Meta:  10 files created & unlinked in ") != NULL);
        assert(strstr(stdout_buf, "Summary:") != NULL);
        /* All files and test directory must be cleanly unlinked */
        assert(active_entry_count() == 0);
        printf("PASS: test 2 (default output & timing measurement)\n");
    }

    /* Test 3: Comparison mode (-c) */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-c", "-w", "64K", "-n", "5", "/mnt"};
        int rc = diskbench_main(7, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "diskbench context mount=/mnt fs=ext2") != NULL);
        assert(strstr(stdout_buf, "diskbench write bytes=65536 time_ms=") != NULL);
        assert(strstr(stdout_buf, "diskbench read bytes=65536 time_ms=") != NULL);
        assert(strstr(stdout_buf, "diskbench meta files=5 time_ms=") != NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 3 (comparison mode -c)\n");
    }

    /* Test 4: Silent / summary mode (-s) */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-s", "-w", "64K", "-n", "5", "/mnt"};
        int rc = diskbench_main(7, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "=== diskbench") == NULL);
        assert(strstr(stdout_buf, "write: 65536 bytes in ") != NULL);
        assert(strstr(stdout_buf, "read:  65536 bytes in ") != NULL);
        assert(strstr(stdout_buf, "meta:  5 files in ") != NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 4 (silent/summary mode -s)\n");
    }

    /* Test 5: Filter -t write */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-t", "write", "-w", "64K", "/mnt"};
        int rc = diskbench_main(6, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Write:") != NULL);
        assert(strstr(stdout_buf, "Read:") == NULL);
        assert(strstr(stdout_buf, "Meta:") == NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 5 (-t write filter)\n");
    }

    /* Test 6: Filter -t read */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-t", "read", "-w", "64K", "/mnt"};
        int rc = diskbench_main(6, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Write:") == NULL);
        assert(strstr(stdout_buf, "Read:") != NULL);
        assert(strstr(stdout_buf, "Meta:") == NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 6 (-t read filter)\n");
    }

    /* Test 7: Filter -t meta */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-t", "meta", "-n", "8", "/mnt"};
        int rc = diskbench_main(6, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Write:") == NULL);
        assert(strstr(stdout_buf, "Read:") == NULL);
        assert(strstr(stdout_buf, "Meta:") != NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 7 (-t meta filter)\n");
    }

    /* Test 8: Custom test directory with -d */
    {
        reset_mock();
        char *argv[] = {"diskbench", "-d", "/tmp/benchdir", "-w", "64K", "-n", "4"};
        int rc = diskbench_main(7, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Directory:    /tmp/benchdir") != NULL);
        assert(active_entry_count() == 0);
        printf("PASS: test 8 (-d custom directory)\n");
    }

    /* Test 9: Size and count bounds checking */
    {
        /* Size > 64 MiB */
        reset_mock();
        char *argv1[] = {"diskbench", "-w", "65M"};
        int rc1 = diskbench_main(3, argv1);
        assert(rc1 == 2);
        assert(strstr(stderr_buf, "size exceeds maximum 64 MiB") != NULL);

        /* Size == 0 */
        reset_mock();
        char *argv2[] = {"diskbench", "-w", "0"};
        int rc2 = diskbench_main(3, argv2);
        assert(rc2 == 2);
        assert(strstr(stderr_buf, "size must be greater than 0") != NULL);

        /* Count > 4096 */
        reset_mock();
        char *argv3[] = {"diskbench", "-n", "4097"};
        int rc3 = diskbench_main(3, argv3);
        assert(rc3 == 2);
        assert(strstr(stderr_buf, "count exceeds maximum 4096") != NULL);

        /* Count == 0 */
        reset_mock();
        char *argv4[] = {"diskbench", "-n", "0"};
        int rc4 = diskbench_main(3, argv4);
        assert(rc4 == 2);
        assert(strstr(stderr_buf, "count must be greater than 0") != NULL);

        /* Max valid values */
        reset_mock();
        char *argv5[] = {"diskbench", "-w", "64M", "-n", "4096", "-t", "write"};
        int rc5 = diskbench_main(7, argv5);
        assert(rc5 == 0);
        assert(active_entry_count() == 0);

        printf("PASS: test 9 (bounds checking for -w and -n)\n");
    }

    /* Test 10: Invalid options and arguments */
    {
        reset_mock();
        char *argv1[] = {"diskbench", "-x"};
        int rc1 = diskbench_main(2, argv1);
        assert(rc1 == 2);

        reset_mock();
        char *argv2[] = {"diskbench", "-t", "invalid"};
        int rc2 = diskbench_main(3, argv2);
        assert(rc2 == 2);
        assert(strstr(stderr_buf, "invalid test; choose write, read, meta, or all") != NULL);

        printf("PASS: test 10 (invalid options handling)\n");
    }

    /* Test 11: Simulated signal cleanup */
    {
        reset_mock();
        trigger_signal_on_meta_open = true;
        intercept_exit = true;

        if (setjmp(exit_jmp) == 0) {
            char *argv[] = {"diskbench", "-t", "meta", "-n", "10", "/mnt"};
            diskbench_main(6, argv);
            assert(0 && "Expected longjmp from SYS_EXIT in on_signal");
        } else {
            /* Verified signal handler exited with 128 + SIGINT = 130 */
            assert(last_exit_code == 128 + SIGINT);
            /* Verify all created meta files and test directory were unlinked by on_signal */
            assert(active_entry_count() == 0);
            printf("PASS: test 11 (simulated signal cleanup on SIGINT)\n");
        }
    }

    printf("ALL DISKBENCH HOST TESTS PASSED\n");
    return 0;
}
