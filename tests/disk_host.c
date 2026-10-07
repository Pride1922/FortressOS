/* Host unit test suite for disk with mocked syscalls under ASan/UBSan */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "common.h"
#include "syscall_abi.h"
#include "signal_abi.h"

static char stdout_buf[65536];
static size_t stdout_len = 0;
static char stderr_buf[8192];
static size_t stderr_len = 0;

static mount_info_t mock_mounts[16];
static size_t mock_mount_count = 0;
static bool mock_mount_fail = false;

static block_info_t mock_blocks[16];
static size_t mock_block_count = 0;
static bool mock_block_fail = false;

/* Mock filesystem entries for disk bench */
typedef struct {
    char name[256];
    bool open;
    bool is_dir;
    size_t size;
    size_t pos;
} mock_fs_entry_t;

#define MAX_MOCK_ENTRIES 128
static mock_fs_entry_t entries[MAX_MOCK_ENTRIES];
static size_t entry_count = 0;

static uint64_t mock_uptime_ticks = 1000;
static uint64_t mock_tick_hz = 100;
static uint64_t mock_tsc_hz = 0;
static uint64_t sigint_handler = 0;
static uint64_t sigterm_handler = 0;

static void safe_strcpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static mock_fs_entry_t *find_fs_entry(const char *name) {
    for (size_t i = 0; i < entry_count; i++) {
        if (!strcmp(entries[i].name, name)) {
            return &entries[i];
        }
    }
    return NULL;
}

static void reset_mock(void) {
    memset(mock_mounts, 0, sizeof(mock_mounts));
    mock_mount_count = 0;
    mock_mount_fail = false;

    memset(mock_blocks, 0, sizeof(mock_blocks));
    mock_block_count = 0;
    mock_block_fail = false;

    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    mock_uptime_ticks = 1000;
    mock_tick_hz = 100;
    mock_tsc_hz = 0;
    sigint_handler = 0;
    sigterm_handler = 0;

    stdout_len = 0;
    stdout_buf[0] = '\0';
    stderr_len = 0;
    stderr_buf[0] = '\0';
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_SYSINFO) {
        sysinfo_t *info = (sysinfo_t *)a;
        if (!info) return SYSCALL_EFAULT;
        memset(info, 0, sizeof(*info));
        info->uptime_ticks = mock_uptime_ticks;
        info->tick_hz = mock_tick_hz;
        info->tsc_hz = mock_tsc_hz; info->tsc_hz = mock_tsc_hz;
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
        mock_fs_entry_t *e = find_fs_entry(path);
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
        mock_fs_entry_t *e = find_fs_entry(path);
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
        mock_fs_entry_t *e = find_fs_entry(path);
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
        mock_fs_entry_t *e = &entries[fd - 10];
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
            mock_fs_entry_t *e = &entries[fd - 10];
            if (!e->open) return SYSCALL_EBADF;
            e->pos += count;
            if (e->pos > e->size) e->size = e->pos;
            return (long)count;
        }
        return SYSCALL_EBADF;
    }
    if (nr == SYS_MOUNTINFO) {
        if (mock_mount_fail) return -1;
        uint32_t idx = (uint32_t)a;
        mount_info_t *out = (mount_info_t *)b;
        if (!out) return -1;
        if (idx < mock_mount_count) {
            *out = mock_mounts[idx];
            return 1;
        }
        return 0;
    }
    if (nr == SYS_BLOCKINFO) {
        if (mock_block_fail) return -1;
        uint32_t idx = (uint32_t)a;
        block_info_t *out = (block_info_t *)b;
        if (!out) return -1;
        if (idx < mock_block_count) {
            *out = mock_blocks[idx];
            return 1;
        }
        return 0;
    }
    return -1;
}

int main(void) {
    printf("[disk_host] Starting tests...\n");

    /* Test 1: disk --help */
    {
        reset_mock();
        char *argv[] = { "disk", "--help", NULL };
        int rc = disk_main(2, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Usage: disk [subcommand]") != NULL);
        assert(strstr(stdout_buf, "list  [-c]") != NULL);
        assert(strstr(stdout_buf, "usage [-c]") != NULL);
        assert(strstr(stdout_buf, "bench [options]") != NULL);
        printf("[disk_host] Test 1: --help passed\n");
    }

    /* Test 2: disk with no args defaults to disk list */
    {
        reset_mock();
        safe_strcpy(mock_blocks[0].name, "initramfs", sizeof(mock_blocks[0].name));
        mock_blocks[0].sector_size = 512;
        mock_blocks[0].size_bytes = 1048576;
        mock_block_count = 1;

        safe_strcpy(mock_mounts[0].source, "initramfs", sizeof(mock_mounts[0].source));
        safe_strcpy(mock_mounts[0].mount_path, "/", sizeof(mock_mounts[0].mount_path));
        mock_mounts[0].fs_type = VFS_FS_TARFS;
        mock_mount_count = 1;

        char *argv[] = { "disk", NULL };
        int rc = disk_main(1, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "NAME") != NULL);
        assert(strstr(stdout_buf, "SIZE") != NULL);
        assert(strstr(stdout_buf, "SECTOR") != NULL);
        assert(strstr(stdout_buf, "MOUNT") != NULL);
        assert(strstr(stdout_buf, "initramfs") != NULL);
        assert(strstr(stdout_buf, "1M") != NULL);
        assert(strstr(stdout_buf, "512B") != NULL);
        assert(strstr(stdout_buf, "/") != NULL);
        printf("[disk_host] Test 2: default invocation (disk -> list) passed\n");
    }

    /* Test 3: disk list with multiple devices (mounted and unmounted) */
    {
        reset_mock();
        safe_strcpy(mock_mounts[0].source, "initramfs", sizeof(mock_mounts[0].source));
        safe_strcpy(mock_mounts[0].mount_path, "/", sizeof(mock_mounts[0].mount_path));
        mock_mounts[0].fs_type = VFS_FS_TARFS;

        safe_strcpy(mock_mounts[1].source, "sda", sizeof(mock_mounts[1].source));
        safe_strcpy(mock_mounts[1].mount_path, "/mnt", sizeof(mock_mounts[1].mount_path));
        mock_mounts[1].fs_type = VFS_FS_EXT2;
        mock_mount_count = 2;

        safe_strcpy(mock_blocks[0].name, "initramfs", sizeof(mock_blocks[0].name));
        mock_blocks[0].sector_size = 512;
        mock_blocks[0].size_bytes = 1048576;

        safe_strcpy(mock_blocks[1].name, "nvme0n1", sizeof(mock_blocks[1].name));
        mock_blocks[1].sector_size = 512;
        mock_blocks[1].size_bytes = 4194304;

        safe_strcpy(mock_blocks[2].name, "sda", sizeof(mock_blocks[2].name));
        mock_blocks[2].sector_size = 512;
        mock_blocks[2].size_bytes = 4194304;
        mock_block_count = 3;

        char *argv[] = { "disk", "list", NULL };
        int rc = disk_main(2, argv);
        assert(rc == 0);

        assert(strstr(stdout_buf, "NAME") != NULL);
        assert(strstr(stdout_buf, "SIZE") != NULL);
        assert(strstr(stdout_buf, "SECTOR") != NULL);
        assert(strstr(stdout_buf, "MOUNT") != NULL);

        assert(strstr(stdout_buf, "initramfs") != NULL);
        assert(strstr(stdout_buf, "nvme0n1") != NULL);
        assert(strstr(stdout_buf, "sda") != NULL);
        assert(strstr(stdout_buf, "/") != NULL);
        assert(strstr(stdout_buf, "/mnt") != NULL);
        assert(strstr(stdout_buf, "—") != NULL);

        printf("[disk_host] Test 3: disk list table passed\n");
    }

    /* Test 4: disk list -c (comparison format) */
    {
        reset_mock();
        safe_strcpy(mock_mounts[0].source, "initramfs", sizeof(mock_mounts[0].source));
        safe_strcpy(mock_mounts[0].mount_path, "/", sizeof(mock_mounts[0].mount_path));
        mock_mounts[0].fs_type = VFS_FS_TARFS;

        safe_strcpy(mock_mounts[1].source, "sda", sizeof(mock_mounts[1].source));
        safe_strcpy(mock_mounts[1].mount_path, "/mnt", sizeof(mock_mounts[1].mount_path));
        mock_mounts[1].fs_type = VFS_FS_EXT2;
        mock_mount_count = 2;

        safe_strcpy(mock_blocks[0].name, "initramfs", sizeof(mock_blocks[0].name));
        mock_blocks[0].sector_size = 512;
        mock_blocks[0].size_bytes = 1048576;

        safe_strcpy(mock_blocks[1].name, "nvme0n1", sizeof(mock_blocks[1].name));
        mock_blocks[1].sector_size = 512;
        mock_blocks[1].size_bytes = 4194304;

        safe_strcpy(mock_blocks[2].name, "sda", sizeof(mock_blocks[2].name));
        mock_blocks[2].sector_size = 512;
        mock_blocks[2].size_bytes = 4194304;
        mock_block_count = 3;

        char *argv[] = { "disk", "list", "-c", NULL };
        int rc = disk_main(3, argv);
        assert(rc == 0);

        assert(strstr(stdout_buf, "name=initramfs sector=512 size=1048576 mount=/") != NULL);
        assert(strstr(stdout_buf, "name=nvme0n1 sector=512 size=4194304\n") != NULL);
        assert(strstr(stdout_buf, "name=sda sector=512 size=4194304 mount=/mnt") != NULL);

        printf("[disk_host] Test 4: disk list -c comparison format passed\n");
    }

    /* Test 5: disk usage with 2 mounts */
    {
        reset_mock();
        safe_strcpy(mock_mounts[0].source, "initramfs", sizeof(mock_mounts[0].source));
        safe_strcpy(mock_mounts[0].mount_path, "/", sizeof(mock_mounts[0].mount_path));
        mock_mounts[0].fs_type = VFS_FS_TARFS;
        mock_mounts[0].flags = MOUNT_FLAGS_RO;
        mock_mounts[0].block_size = 512;
        mock_mounts[0].total_blocks = 32768;
        mock_mounts[0].free_blocks = 0;
        mock_mounts[0].total_inodes = 42;
        mock_mounts[0].free_inodes = 0;

        safe_strcpy(mock_mounts[1].source, "nvme0n1p1", sizeof(mock_mounts[1].source));
        safe_strcpy(mock_mounts[1].mount_path, "/mnt", sizeof(mock_mounts[1].mount_path));
        mock_mounts[1].fs_type = VFS_FS_EXT2;
        mock_mounts[1].flags = MOUNT_FLAGS_RW;
        mock_mounts[1].block_size = 4096;
        mock_mounts[1].total_blocks = 66846720ULL;
        mock_mounts[1].free_blocks = 35389440ULL;
        mock_mounts[1].total_inodes = 1024;
        mock_mounts[1].free_inodes = 901;
        mock_mount_count = 2;

        char *argv[] = { "disk", "usage", NULL };
        int rc = disk_main(2, argv);
        assert(rc == 0);

        assert(strstr(stdout_buf, "MOUNT") != NULL);
        assert(strstr(stdout_buf, "SOURCE") != NULL);
        assert(strstr(stdout_buf, "FS") != NULL);
        assert(strstr(stdout_buf, "SIZE") != NULL);
        assert(strstr(stdout_buf, "USED") != NULL);
        assert(strstr(stdout_buf, "AVAIL") != NULL);
        assert(strstr(stdout_buf, "USE%") != NULL);
        assert(strstr(stdout_buf, "INODES") != NULL);
        assert(strstr(stdout_buf, "IUSE%") != NULL);

        printf("[disk_host] Test 5: disk usage table passed\n");
    }

    /* Test 6: disk usage -c */
    {
        reset_mock();
        safe_strcpy(mock_mounts[0].source, "initramfs", sizeof(mock_mounts[0].source));
        safe_strcpy(mock_mounts[0].mount_path, "/", sizeof(mock_mounts[0].mount_path));
        mock_mounts[0].fs_type = VFS_FS_TARFS;
        mock_mounts[0].block_size = 512;
        mock_mounts[0].total_blocks = 32768;
        mock_mounts[0].free_blocks = 0;
        mock_mounts[0].total_inodes = 42;
        mock_mounts[0].free_inodes = 0;
        mock_mount_count = 1;

        char *argv[] = { "disk", "usage", "-c", NULL };
        int rc = disk_main(3, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "mount=/ source=initramfs fs=TarFS") != NULL);
        printf("[disk_host] Test 6: disk usage -c passed\n");
    }

    /* Test 7: unrecognized subcommand */
    {
        reset_mock();
        char *argv[] = { "disk", "bad_cmd", NULL };
        int rc = disk_main(2, argv);
        assert(rc == 2);
        assert(strstr(stderr_buf, "unknown subcommand") != NULL);
        printf("[disk_host] Test 7: unrecognized subcommand passed\n");
    }

    /* Test 8: SYS_BLOCKINFO failure */
    {
        reset_mock();
        mock_block_fail = true;
        char *argv[] = { "disk", "list", NULL };
        int rc = disk_main(2, argv);
        assert(rc != 0);
        assert(strstr(stderr_buf, "failed to query block info") != NULL);
        printf("[disk_host] Test 8: SYS_BLOCKINFO failure handling passed\n");
    }

    /* Test 9: disk bench --help */
    {
        reset_mock();
        char *argv[] = { "disk", "bench", "--help", NULL };
        int rc = disk_main(3, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "Usage: diskbench") != NULL);
        assert(strstr(stdout_buf, "-w SIZE") != NULL);
        assert(strstr(stdout_buf, "-n COUNT") != NULL);
        printf("[disk_host] Test 9: disk bench --help passed\n");
    }

    /* Test 10: disk bench execution (standard table) */
    {
        reset_mock();
        char *argv[] = { "disk", "bench", "-w", "64K", "-n", "10", "/mnt", NULL };
        int rc = disk_main(7, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "=== diskbench: /mnt ===") != NULL);
        assert(strstr(stdout_buf, "Write: 65536 bytes") != NULL);
        assert(strstr(stdout_buf, "Read:  65536 bytes") != NULL);
        assert(strstr(stdout_buf, "Meta:  10 files") != NULL);
        assert(strstr(stdout_buf, "Summary:") != NULL);
        printf("[disk_host] Test 10: disk bench execution passed\n");
    }

    /* Test 11: disk bench -c (comparison format) */
    {
        reset_mock();
        char *argv[] = { "disk", "bench", "-c", "-w", "64K", "-n", "10", "/mnt", NULL };
        int rc = disk_main(8, argv);
        assert(rc == 0);
        assert(strstr(stdout_buf, "diskbench context mount=/mnt") != NULL);
        assert(strstr(stdout_buf, "diskbench write bytes=65536") != NULL);
        assert(strstr(stdout_buf, "diskbench read bytes=65536") != NULL);
        assert(strstr(stdout_buf, "diskbench meta files=10") != NULL);
        printf("[disk_host] Test 11: disk bench -c comparison mode passed\n");
    }

    printf("[disk_host] All 11 host tests passed successfully!\n");
    return 0;
}
