/* Host unit test suite for cp and touch with mocked syscalls under ASan/UBSan */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

typedef struct {
    bool open;
    bool is_write;
    bool modified;
    const unsigned char *read_data;
    size_t read_len;
    size_t read_pos;
    unsigned char write_data[65536];
    size_t write_len;
} mock_fd_t;

static mock_fd_t fds[32];
static char mock_file_names[32][64];

static unsigned char stdout_buf[65536];
static size_t stdout_len;
static char stderr_buf[4096];
static size_t stderr_len;

static int stats, opens, closes, reads, writes, chmods;
static int fail_open, fail_close, fail_read, fail_write, fail_chmod;
static uint32_t last_chmod_mode;
static char last_chmod_path[64];

static struct {
    char name[64];
    bool exists;
    bool is_dir;
    uint32_t mode;
    unsigned char data[65536];
    size_t size;
} vfs_files[16];
static size_t vfs_file_count;

static void reset_vfs(void) {
    memset(fds, 0, sizeof(fds));
    memset(mock_file_names, 0, sizeof(mock_file_names));
    stdout_len = 0; stdout_buf[0] = 0;
    stderr_len = 0; stderr_buf[0] = 0;
    stats = opens = closes = reads = writes = chmods = 0;
    fail_open = fail_close = fail_read = fail_write = fail_chmod = 0;
    last_chmod_mode = 0;
    last_chmod_path[0] = 0;
    vfs_file_count = 0;
    memset(vfs_files, 0, sizeof(vfs_files));

    /* Standard descriptors */
    fds[0].open = true;
    fds[1].open = true;
    fds[2].open = true;
}

static void add_vfs_file(const char *name, const void *data, size_t size, uint32_t mode, bool is_dir) {
    assert(vfs_file_count < 16);
    size_t idx = vfs_file_count++;
    size_t n = strlen(name);
    if (n >= sizeof(vfs_files[idx].name)) n = sizeof(vfs_files[idx].name) - 1;
    memcpy(vfs_files[idx].name, name, n);
    vfs_files[idx].name[n] = '\0';
    vfs_files[idx].exists = true;
    vfs_files[idx].is_dir = is_dir;
    vfs_files[idx].mode = mode;
    vfs_files[idx].size = size;
    if (data && size) {
        assert(size <= sizeof(vfs_files[idx].data));
        memcpy(vfs_files[idx].data, data, size);
    }
}

static int find_vfs_file(const char *name) {
    for (size_t i = 0; i < vfs_file_count; i++) {
        if (!strcmp(vfs_files[i].name, name) && vfs_files[i].exists) {
            return (int)i;
        }
    }
    return -1;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        stats++;
        const char *name = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        if (!strcmp(name, "denied")) return SYSCALL_EACCES;
        int idx = find_vfs_file(name);
        if (idx < 0) return SYSCALL_ENOENT;
        st->type = vfs_files[idx].is_dir ? VFS_DIRECTORY : VFS_FILE;
        st->size = vfs_files[idx].size;
        st->mode = vfs_files[idx].mode;
        return 0;
    }
    if (nr == SYS_OPEN) {
        opens++;
        if (fail_open) return SYSCALL_EMFILE;
        const char *name = (const char *)a;
        int flags = (int)b;
        if (!strcmp(name, "denied")) return SYSCALL_EACCES;

        int idx = find_vfs_file(name);
        if (flags & VFS_O_CREAT) {
            if (idx < 0) {
                add_vfs_file(name, NULL, 0, 0644, false);
                idx = (int)(vfs_file_count - 1);
            }
        } else {
            if (idx < 0) return SYSCALL_ENOENT;
        }

        if (flags & VFS_O_TRUNC) {
            vfs_files[idx].size = 0;
        }

        /* Allocate fd >= 3 */
        for (int i = 3; i < 32; i++) {
            if (!fds[i].open) {
                fds[i].open = true;
                fds[i].is_write = (flags & (VFS_O_WRONLY | VFS_O_RDWR)) != 0;
                fds[i].modified = (flags & VFS_O_TRUNC) != 0;
                fds[i].read_data = vfs_files[idx].data;
                fds[i].read_len = vfs_files[idx].size;
                fds[i].read_pos = 0;
                fds[i].write_len = 0;
                size_t n = strlen(name);
                if (n >= sizeof(mock_file_names[i])) n = sizeof(mock_file_names[i]) - 1;
                memcpy(mock_file_names[i], name, n);
                mock_file_names[i][n] = '\0';
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        closes++;
        int fd = (int)a;
        if (fd < 0 || fd >= 32 || !fds[fd].open) return SYSCALL_EBADF;
        if (fail_close) return SYSCALL_EIO;

        if (fds[fd].is_write && fds[fd].modified) {
            int idx = find_vfs_file(mock_file_names[fd]);
            if (idx >= 0) {
                memcpy(vfs_files[idx].data, fds[fd].write_data, fds[fd].write_len);
                vfs_files[idx].size = fds[fd].write_len;
            }
        }
        fds[fd].open = false;
        return 0;
    }
    if (nr == SYS_READ) {
        reads++;
        if (reads == fail_read) return SYSCALL_EIO;
        int fd = (int)a;
        if (fd < 0 || fd >= 32 || !fds[fd].open) return SYSCALL_EBADF;
        mock_fd_t *m = &fds[fd];
        size_t avail = m->read_len - m->read_pos;
        size_t n = c < avail ? c : avail;
        if (n > 0) {
            memcpy((void *)b, m->read_data + m->read_pos, n);
            m->read_pos += n;
        }
        return (long)n;
    }
    if (nr == SYS_WRITE) {
        writes++;
        if (writes == fail_write) return SYSCALL_EIO;
        int fd = (int)a;
        size_t n = c;
        if (fd == 1) {
            assert(stdout_len + n < sizeof(stdout_buf));
            memcpy(stdout_buf + stdout_len, (const void *)b, n);
            stdout_len += n;
            stdout_buf[stdout_len] = 0;
            return (long)n;
        }
        if (fd == 2) {
            assert(stderr_len + n < sizeof(stderr_buf));
            memcpy(stderr_buf + stderr_len, (const void *)b, n);
            stderr_len += n;
            stderr_buf[stderr_len] = 0;
            return (long)n;
        }
        if (fd >= 3 && fd < 32 && fds[fd].open) {
            assert(fds[fd].write_len + n <= sizeof(fds[fd].write_data));
            memcpy(fds[fd].write_data + fds[fd].write_len, (const void *)b, n);
            fds[fd].write_len += n;
            fds[fd].modified = true;
            return (long)n;
        }
        return SYSCALL_EBADF;
    }
    if (nr == SYS_CHMOD) {
        chmods++;
        if (fail_chmod) return SYSCALL_EPERM;
        last_chmod_mode = (uint32_t)b;
        size_t n = strlen((const char *)a);
        if (n >= sizeof(last_chmod_path)) n = sizeof(last_chmod_path) - 1;
        memcpy(last_chmod_path, (const char *)a, n);
        last_chmod_path[n] = '\0';
        int idx = find_vfs_file((const char *)a);
        if (idx >= 0) vfs_files[idx].mode = (uint32_t)b;
        return 0;
    }
    return SYSCALL_ENOSYS;
}

int main(void) {
    printf("Running cp host tests under ASan/UBSan...\n");

    /* Test 1: cp --help */
    reset_vfs();
    char *args_help[] = {"cp", "--help", NULL};
    assert(cp_main(2, args_help) == 0);
    assert(strstr((char *)stdout_buf, "Usage: cp"));
    printf("  [PASS] Test 1: cp --help\n");

    /* Test 2: cp missing arguments */
    reset_vfs();
    char *args_no_arg[] = {"cp", NULL};
    assert(cp_main(1, args_no_arg) == 1);
    char *args_one_arg[] = {"cp", "file1", NULL};
    assert(cp_main(2, args_one_arg) == 1);
    printf("  [PASS] Test 2: cp missing arguments error\n");

    /* Test 3: cp non-existent source */
    reset_vfs();
    char *args_no_src[] = {"cp", "nosuchfile", "dst", NULL};
    assert(cp_main(3, args_no_src) == 1);
    printf("  [PASS] Test 3: cp non-existent source error\n");

    /* Test 4: cp single regular file to new destination */
    reset_vfs();
    const char *content = "Hello, FortressOS cp test payload!";
    add_vfs_file("source.txt", content, strlen(content), 0755, false);
    char *args_copy[] = {"cp", "source.txt", "dest.txt", NULL};
    assert(cp_main(3, args_copy) == 0);
    int dst_idx = find_vfs_file("dest.txt");
    assert(dst_idx >= 0);
    assert(vfs_files[dst_idx].size == strlen(content));
    assert(!memcmp(vfs_files[dst_idx].data, content, strlen(content)));
    assert(vfs_files[dst_idx].mode == 0755);
    printf("  [PASS] Test 4: cp copy file and mode preservation\n");

    /* Test 5: cp source and destination are identical */
    reset_vfs();
    add_vfs_file("same.txt", "data", 4, 0644, false);
    char *args_same[] = {"cp", "same.txt", "same.txt", NULL};
    assert(cp_main(3, args_same) == 1);
    assert(strstr(stderr_buf, "are the same file"));
    printf("  [PASS] Test 5: cp identical source and destination error\n");

    /* Test 6: cp omitting directory */
    reset_vfs();
    add_vfs_file("mydir", NULL, 0, 0755, true);
    char *args_dir[] = {"cp", "mydir", "dest.txt", NULL};
    assert(cp_main(3, args_dir) == 1);
    assert(strstr(stderr_buf, "omitting directory"));
    printf("  [PASS] Test 6: cp omitting directory\n");

    /* Test 7: cp multiple files to a directory */
    reset_vfs();
    add_vfs_file("f1.txt", "file1 data", 10, 0644, false);
    add_vfs_file("f2.txt", "file2 data", 10, 0644, false);
    add_vfs_file("target_dir", NULL, 0, 0755, true);
    char *args_multi[] = {"cp", "f1.txt", "f2.txt", "target_dir", NULL};
    assert(cp_main(4, args_multi) == 0);
    int out1 = find_vfs_file("target_dir/f1.txt");
    int out2 = find_vfs_file("target_dir/f2.txt");
    assert(out1 >= 0 && out2 >= 0);
    assert(!memcmp(vfs_files[out1].data, "file1 data", 10));
    assert(!memcmp(vfs_files[out2].data, "file2 data", 10));
    printf("  [PASS] Test 7: cp multiple files into directory\n");

    /* Test 8: cp multiple files to non-directory target fails */
    reset_vfs();
    add_vfs_file("f1.txt", "file1 data", 10, 0644, false);
    add_vfs_file("f2.txt", "file2 data", 10, 0644, false);
    char *args_multi_fail[] = {"cp", "f1.txt", "f2.txt", "not_a_dir", NULL};
    assert(cp_main(4, args_multi_fail) == 1);
    assert(strstr(stderr_buf, "target is not a directory"));
    printf("  [PASS] Test 8: cp multiple files to non-directory fails\n");

    /* Test 9: cp multi-chunk bounded buffer copy */
    reset_vfs();
    unsigned char big_payload[10000];
    for (size_t i = 0; i < sizeof(big_payload); i++) big_payload[i] = (unsigned char)(i & 0xFF);
    add_vfs_file("large.bin", big_payload, sizeof(big_payload), 0600, false);
    char *args_large[] = {"cp", "large.bin", "large_copy.bin", NULL};
    assert(cp_main(3, args_large) == 0);
    int l_idx = find_vfs_file("large_copy.bin");
    assert(l_idx >= 0);
    assert(vfs_files[l_idx].size == sizeof(big_payload));
    assert(!memcmp(vfs_files[l_idx].data, big_payload, sizeof(big_payload)));
    printf("  [PASS] Test 9: cp multi-chunk bounded buffer copy\n");

    /* Test 10: cp read error handling */
    reset_vfs();
    add_vfs_file("src.bin", "sample", 6, 0644, false);
    fail_read = 1;
    char *args_r_err[] = {"cp", "src.bin", "dst.bin", NULL};
    assert(cp_main(3, args_r_err) == 1);
    assert(strstr(stderr_buf, "error reading"));
    printf("  [PASS] Test 10: cp read error clean failure\n");

    /* Test 11: cp write error handling */
    reset_vfs();
    add_vfs_file("src.bin", "sample", 6, 0644, false);
    fail_write = 1;
    char *args_w_err[] = {"cp", "src.bin", "dst.bin", NULL};
    assert(cp_main(3, args_w_err) == 1);
    printf("  [PASS] Test 11: cp write error clean failure\n");

    /* Test 12: cp close error handling */
    reset_vfs();
    add_vfs_file("src.bin", "sample", 6, 0644, false);
    fail_close = 1;
    char *args_c_err[] = {"cp", "src.bin", "dst.bin", NULL};
    assert(cp_main(3, args_c_err) == 1);
    printf("  [PASS] Test 12: cp close error clean failure\n");

    /* Test 12b: cp chmod failure warns, retains data, exits 1 */
    reset_vfs();
    add_vfs_file("root_owned.txt", "payload", 7, 0755, false);
    fail_chmod = 1;
    char *args_perm[] = {"cp", "root_owned.txt", "dest_copy.txt", NULL};
    assert(cp_main(3, args_perm) == 1);
    assert(strstr(stderr_buf, "preserving permissions for 'dest_copy.txt' failed: Permission denied"));
    int perm_idx = find_vfs_file("dest_copy.txt");
    assert(perm_idx >= 0);
    assert(vfs_files[perm_idx].size == 7);
    assert(!memcmp(vfs_files[perm_idx].data, "payload", 7));
    printf("  [PASS] Test 12b: cp permission preservation warning\n");

    printf("\nRunning touch host tests under ASan/UBSan...\n");

    /* Test 13: touch --help */
    reset_vfs();
    char *args_t_help[] = {"touch", "--help", NULL};
    assert(touch_main(2, args_t_help) == 0);
    assert(strstr((char *)stdout_buf, "Usage: touch"));
    printf("  [PASS] Test 13: touch --help\n");

    /* Test 14: touch missing file operand */
    reset_vfs();
    char *args_t_none[] = {"touch", NULL};
    assert(touch_main(1, args_t_none) == 1);
    assert(strstr(stderr_buf, "missing file operand"));
    printf("  [PASS] Test 14: touch missing operand\n");

    /* Test 15: touch creates new file */
    reset_vfs();
    char *args_t_new[] = {"touch", "created.txt", NULL};
    assert(touch_main(2, args_t_new) == 0);
    int t_idx = find_vfs_file("created.txt");
    assert(t_idx >= 0);
    assert(vfs_files[t_idx].size == 0);
    printf("  [PASS] Test 15: touch creates new file\n");

    /* Test 16: touch existing file leaves content untouched */
    reset_vfs();
    add_vfs_file("keep.txt", "retained data", 13, 0644, false);
    char *args_t_exist[] = {"touch", "keep.txt", NULL};
    assert(touch_main(2, args_t_exist) == 0);
    int k_idx = find_vfs_file("keep.txt");
    assert(k_idx >= 0);
    assert(vfs_files[k_idx].size == 13);
    assert(!memcmp(vfs_files[k_idx].data, "retained data", 13));
    printf("  [PASS] Test 16: touch existing file preserves content\n");

    /* Test 17: touch -c does not create missing file */
    reset_vfs();
    char *args_t_nocreate[] = {"touch", "-c", "absent.txt", NULL};
    assert(touch_main(3, args_t_nocreate) == 0);
    assert(find_vfs_file("absent.txt") < 0);
    printf("  [PASS] Test 17: touch -c suppresses creation\n");

    /* Test 18: touch multiple files */
    reset_vfs();
    char *args_t_multi[] = {"touch", "a.txt", "b.txt", "c.txt", NULL};
    assert(touch_main(4, args_t_multi) == 0);
    assert(find_vfs_file("a.txt") >= 0);
    assert(find_vfs_file("b.txt") >= 0);
    assert(find_vfs_file("c.txt") >= 0);
    printf("  [PASS] Test 18: touch multiple files\n");

    /* Test 19: touch invalid option */
    reset_vfs();
    char *args_t_inv[] = {"touch", "-z", "foo", NULL};
    assert(touch_main(3, args_t_inv) == 1);
    assert(strstr(stderr_buf, "invalid option"));
    printf("  [PASS] Test 19: touch invalid option rejection\n");

    /* Test 20: touch permission denied */
    reset_vfs();
    char *args_t_denied[] = {"touch", "denied", NULL};
    assert(touch_main(2, args_t_denied) == 1);
    assert(strstr(stderr_buf, "permission denied"));
    printf("  [PASS] Test 20: touch permission denied handling\n");

    printf("\nALL CP AND TOUCH HOST TESTS PASSED (100%%)!\n");
    return 0;
}
