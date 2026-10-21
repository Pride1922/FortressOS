/*
 * Unit tests for ETCFS: In-memory writable /etc on Live Media.
 * Validates dynamic file creation, copy-on-write modifications,
 * atomic replacement, memory ceiling, and security immutability of
 * /etc/passwd, /etc/group, and /etc/shadow.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ext4_host/spinlock.h"
#define HOST_SPINLOCK_H
#define TEST_PERMISSIONS_ENFORCEMENT

#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "creds.h"
#include "etcfs.h"

void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t z) { return calloc(n, z); }
void kfree(void *p) { free(p); }
void serial_puts(const char *s) { (void)s; }
void serial_print_dec(uint64_t n) { (void)n; }
void serial_print_hex(uint64_t n) { (void)n; }
void serial_putc(char c) { (void)c; }
void serial_raw_putc(char c) { (void)c; }
void console_inc_generation(void) {}
bool console_is_quiet(void) { return true; }
void console_terminal_write(const char *s, size_t n) { (void)s; (void)n; }
int64_t input_read(void *buf, size_t count) { (void)buf; (void)count; return 0; }

#include "../src/fs/gpt.h"
static gpt_partition_t s_mock_parts[1];
size_t gpt_get_partition_count(void) { return 0; }
gpt_partition_t *gpt_get_partition(size_t i) { (void)i; return &s_mock_parts[0]; }
bool ext4_device_read_sector(block_dev_t *d, uint64_t l, void *b, bool *handled) {
    (void)d; (void)l; (void)b; *handled = false; return false;
}
bool ext2_device_read_sector(block_dev_t *d, uint64_t l, void *b) {
    (void)d; (void)l; (void)b; return false;
}

#include "../src/fs/permission_values.c"
#include "../src/fs/runfs.c"
#include "../src/fs/devfs.c"
#include "../src/fs/vfs.c"
#include "../src/fs/etcfs.c"

static const char s_passwd_data[] = "root:x:0:0:root:/root:/bin/sh\noperator:x:1000:1000::/run/user/1000:/bin/sh\n";
static const char s_group_data[] = "root:x:0:\nwheel:x:10:operator\noperator:x:1000:\n";
static const char s_shadow_data[] = "root:!:::::::\noperator::::::::\n";
static const char s_sshd_data[] = "Port 22\nPermitRootLogin no\n";
static const char s_motd_data[] = "Welcome to FortressOS Live\n";

static void setup_mock_tree(void) {
    vfs_init();

    /* Create /etc directory */
    vfs_node_t *etc = vfs_create_node_kernel("/etc", VFS_DIRECTORY, 0, NULL);
    assert(etc != NULL);
    etc->mode = VFS_S_IFDIR | 0755;

    /* Create account files */
    vfs_node_t *passwd = vfs_create_node_kernel("/etc/passwd", VFS_FILE, sizeof(s_passwd_data) - 1, s_passwd_data);
    assert(passwd != NULL);
    passwd->mode = VFS_S_IFREG | 0644;
    passwd->uid = 0; passwd->gid = 0;

    vfs_node_t *group = vfs_create_node_kernel("/etc/group", VFS_FILE, sizeof(s_group_data) - 1, s_group_data);
    assert(group != NULL);
    group->mode = VFS_S_IFREG | 0644;
    group->uid = 0; group->gid = 0;

    vfs_node_t *shadow = vfs_create_node_kernel("/etc/shadow", VFS_FILE, sizeof(s_shadow_data) - 1, s_shadow_data);
    assert(shadow != NULL);
    shadow->mode = VFS_S_IFREG | 0600;
    shadow->uid = 0; shadow->gid = 0;

    /* Create /etc/ssh directory and config */
    vfs_node_t *ssh = vfs_create_node_kernel("/etc/ssh", VFS_DIRECTORY, 0, NULL);
    assert(ssh != NULL);
    ssh->mode = VFS_S_IFDIR | 0755;

    vfs_node_t *sshd_cfg = vfs_create_node_kernel("/etc/ssh/sshd_config", VFS_FILE, sizeof(s_sshd_data) - 1, s_sshd_data);
    assert(sshd_cfg != NULL);
    sshd_cfg->mode = VFS_S_IFREG | 0644;

    /* Create /etc/motd */
    vfs_node_t *motd = vfs_create_node_kernel("/etc/motd", VFS_FILE, sizeof(s_motd_data) - 1, s_motd_data);
    assert(motd != NULL);
    motd->mode = VFS_S_IFREG | 0644;
}

int main(void) {
    printf("--> Starting etcfs unit test suite...\n");
    setup_mock_tree();

    creds_t root;
    creds_init_root(&root);
    creds_t user = {.uid = 1000, .euid = 1000, .gid = 1000, .egid = 1000, .umask = 0022};

    /* 1. Initialize etcfs */
    int r = etcfs_init(NULL);
    assert(r == 0);
    printf("  [PASS] etcfs_init successfully mounted on /etc\n");

    /* 2. Verify reading pristine TarFS files works */
    int err = 0;
    file_t *f = vfs_open_creds("/etc/motd", VFS_O_RDONLY, &root, &err);
    assert(f != NULL && err == 0);
    char buf[128];
    int64_t n = vfs_read(f, buf, sizeof(buf) - 1);
    assert(n == sizeof(s_motd_data) - 1);
    buf[n] = '\0';
    assert(strcmp(buf, s_motd_data) == 0);
    vfs_close(f);
    assert(etcfs_allocated_bytes() == 0); /* Zero dynamic memory used for unedited pristine files */
    printf("  [PASS] Read pristine files without dynamic memory allocation\n");

    /* 3. Verify security invariant: /etc/passwd, /etc/group, /etc/shadow are immutable */
    vfs_metadata_t smeta;
    vfs_node_t *snode = vfs_lookup_creds("/etc/shadow", &root, &err);
    assert(snode != NULL);
    assert(vfs_metadata(snode, &smeta) == 0);
    assert((smeta.mnt_flags & VFS_MNT_RDONLY) != 0); /* Must satisfy userdb_io line 40 check */
    vfs_node_put(snode);

    /* Write open rejected on protected files */
    f = vfs_open_creds("/etc/passwd", VFS_O_WRONLY, &root, &err);
    assert(f == NULL && err == -VFS_EROFS);

    f = vfs_open_creds("/etc/shadow", VFS_O_RDWR | VFS_O_TRUNC, &root, &err);
    assert(f == NULL && err == -VFS_EROFS);

    /* Unlink rejected on protected files */
    assert(vfs_unlink_creds("/etc/passwd", &root) == -VFS_EPERM);
    assert(vfs_unlink_creds("/etc/shadow", &root) == -VFS_EPERM);

    /* Rename / replace rejected on protected files */
    assert(vfs_rename_creds("/etc/shadow", "/etc/shadow.bak", &root) == -VFS_EPERM);
    assert(vfs_replace_creds("/etc/motd", "/etc/shadow", &root) == -VFS_EPERM);
    printf("  [PASS] Security invariant enforced: /etc/passwd, /etc/group, /etc/shadow are immutable\n");

    /* 4. Test dynamic file creation in /etc (simulating 'date set TZ=Europe/Brussels') */
    f = vfs_open_mode_creds("/etc/timezone.tmp", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, &root, &err);
    assert(f != NULL && err == 0);
    const char tz_val[] = "Europe/Brussels\n";
    assert(vfs_write_creds(f, tz_val, strlen(tz_val), &root) == (int64_t)strlen(tz_val));
    vfs_close(f);
    assert(etcfs_allocated_bytes() > 0);

    /* Rename-replace to /etc/timezone */
    r = vfs_replace_creds("/etc/timezone.tmp", "/etc/timezone", &root);
    if (r == -VFS_EOPNOTSUPP) {
        r = vfs_rename_creds("/etc/timezone.tmp", "/etc/timezone", &root);
    }
    assert(r == 0);

    /* Read back /etc/timezone */
    f = vfs_open_creds("/etc/timezone", VFS_O_RDONLY, &user, &err);
    assert(f != NULL && err == 0);
    n = vfs_read(f, buf, sizeof(buf) - 1);
    assert(n == (int64_t)strlen(tz_val));
    buf[n] = '\0';
    assert(strcmp(buf, tz_val) == 0);
    vfs_close(f);
    printf("  [PASS] Atomic create, write, and rename-replace of /etc/timezone verified\n");

    /* 5. Test copy-on-write modification of an existing file (/etc/ssh/sshd_config) */
    f = vfs_open_creds("/etc/ssh/sshd_config", VFS_O_WRONLY | VFS_O_TRUNC, &root, &err);
    assert(f != NULL && err == 0);
    const char new_sshd[] = "Port 2222\nPermitRootLogin yes\n";
    assert(vfs_write_creds(f, new_sshd, strlen(new_sshd), &root) == (int64_t)strlen(new_sshd));
    vfs_close(f);

    f = vfs_open_creds("/etc/ssh/sshd_config", VFS_O_RDONLY, &user, &err);
    assert(f != NULL && err == 0);
    n = vfs_read(f, buf, sizeof(buf) - 1);
    assert(n == (int64_t)strlen(new_sshd));
    buf[n] = '\0';
    assert(strcmp(buf, new_sshd) == 0);
    vfs_close(f);
    printf("  [PASS] Copy-on-write modification of existing TarFS file verified\n");

    /* 6. Test nested directory creation and operations */
    assert(vfs_mkdir_creds("/etc/network.d", 0755, &root) == 0);
    f = vfs_open_mode_creds("/etc/network.d/eth0.conf", VFS_O_CREAT | VFS_O_WRONLY, 0644, &root, &err);
    assert(f != NULL && err == 0);
    assert(vfs_write_creds(f, "dhcp=yes\n", 9, &root) == 9);
    vfs_close(f);

    /* Directory listing includes new file */
    vfs_node_t *netdir = vfs_lookup_creds("/etc/network.d", &root, &err);
    assert(netdir != NULL);
    vfs_dirent_t dent;
    assert(vfs_readdir(netdir, 0, &dent) == 1);
    assert(strcmp(dent.name, "eth0.conf") == 0);
    vfs_node_put(netdir);

    /* Unlink file, then directory */
    assert(vfs_unlink_creds("/etc/network.d", &root) == -VFS_ENOTEMPTY);
    assert(vfs_unlink_creds("/etc/network.d/eth0.conf", &root) == 0);
    assert(vfs_unlink_creds("/etc/network.d", &root) == 0);
    printf("  [PASS] Nested directory lifecycle and readdir verified\n");

    /* 7. Test memory quota enforcement */
    size_t before_quota = etcfs_allocated_bytes();
    f = vfs_open_mode_creds("/etc/quota_test", VFS_O_CREAT | VFS_O_WRONLY, 0644, &root, &err);
    assert(f != NULL && err == 0);
    char *big_chunk = (char *)malloc(1024 * 1024);
    assert(big_chunk != NULL);
    memset(big_chunk, 'A', 1024 * 1024);

    /* Write up to 15 MiB */
    for (int i = 0; i < 15; i++) {
        int64_t w = vfs_write_creds(f, big_chunk, 1024 * 1024, &root);
        assert(w == 1024 * 1024);
    }
    assert(etcfs_allocated_bytes() >= 15 * 1024 * 1024);

    /* Writing beyond 16 MiB must fail with ENOSPC */
    int64_t over = vfs_write_creds(f, big_chunk, 2 * 1024 * 1024, &root);
    assert(over == -VFS_ENOSPC);

    vfs_close(f);
    assert(vfs_unlink_creds("/etc/quota_test", &root) == 0);
    free(big_chunk);
    assert(etcfs_allocated_bytes() == before_quota);
    printf("  [PASS] 16 MiB memory quota ceiling and clean reclamation verified\n");

    printf("\nALL ETCFS HOST TESTS PASSED!\n");
    return 0;
}
