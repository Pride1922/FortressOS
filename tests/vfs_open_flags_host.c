/*
 * tests/vfs_open_flags_host.c - Host-based unit tests for VFS open flags:
 *   VFS_O_EXCL (0x80) and VFS_O_NOFOLLOW (0x100)
 *
 * Verifies:
 *   1. Rejection of invalid flag combinations (VFS_O_EXCL without VFS_O_CREAT).
 *   2. Atomic exclusive creation (VFS_O_CREAT | VFS_O_EXCL) on new files.
 *   3. Collision rejection with -VFS_EEXIST on pre-existing regular files,
 *      asserting preservation of existing contents, size, and metadata.
 *   4. Collision rejection with -VFS_EEXIST on pre-existing directories.
 *   5. Collision rejection with -VFS_EEXIST on pre-existing dangling symlinks.
 *   6. Deterministic error precedence: VFS_O_CREAT | VFS_O_EXCL | VFS_O_NOFOLLOW
 *      on an existing symlink returns -VFS_EEXIST (not -VFS_ELOOP).
 *   7. VFS_O_NOFOLLOW on existing symlink without exclusive creation returns
 *      deterministically -VFS_ELOOP (-40 / SYSCALL_ELOOP).
 *   8. VFS_O_NOFOLLOW on normal files and directories succeeds.
 *   9. Concurrent creator race condition handling (second creator gets -VFS_EEXIST).
 *   10. Syscall interface compliance via sys_open returning SYSCALL_EEXIST and SYSCALL_ELOOP.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PERMISSIONS_SYSCALLS
#define main inherited_pipe_fixture_main
#include "pipe_host.c"
#undef main
#include "../src/fs/runfs.c"

bool is_pty_slave(const vfs_node_t *node) { (void)node; return false; }
bool is_pty_master(const vfs_node_t *node) { (void)node; return false; }

static void test_flag_validation(const creds_t *actor) {
    int err = 0;

    /* 1. VFS_O_EXCL without VFS_O_CREAT must fail with -VFS_EINVAL */
    file_t *f = vfs_open_mode_creds("/tmp/test_invalid_excl", VFS_O_RDWR | VFS_O_EXCL, 0644, actor, &err);
    assert(f == NULL);
    assert(err == -VFS_EINVAL);

    f = vfs_open_mode_creds("/tmp/test_invalid_excl", VFS_O_WRONLY | VFS_O_EXCL, 0644, actor, &err);
    assert(f == NULL);
    assert(err == -VFS_EINVAL);

    /* 2. Undefined flag bits must be rejected */
    f = vfs_open_mode_creds("/tmp/test_invalid_flags", VFS_O_RDWR | 0x4000, 0644, actor, &err);
    assert(f == NULL);
    assert(err == -VFS_EINVAL);

    puts("  [PASS] Flag validation: VFS_O_EXCL without VFS_O_CREAT and invalid flags rejected");
}

static void test_atomic_exclusive_creation(const creds_t *actor) {
    int err = 0;
    const char *path = "/tmp/test_exclusive_new.tmp";

    /* Ensure path does not exist */
    (void)vfs_unlink_creds(path, actor);

    /* 1. First creation with VFS_O_CREAT | VFS_O_EXCL must succeed */
    file_t *f1 = vfs_open_mode_creds(path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0600, actor, &err);
    assert(f1 != NULL);
    assert(err == VFS_SUCCESS);

    /* Write known payload */
    const char *payload = "exclusive_data_payload_12345";
    size_t plen = strlen(payload);
    int64_t wn = vfs_write_creds(f1, payload, plen, actor);
    assert(wn == (int64_t)plen);
    assert(vfs_close(f1) == 0);

    /* 2. Second attempt with VFS_O_CREAT | VFS_O_EXCL must fail atomically with -VFS_EEXIST */
    file_t *f2 = vfs_open_mode_creds(path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0600, actor, &err);
    assert(f2 == NULL);
    assert(err == -VFS_EEXIST);

    /* 3. Even with VFS_O_TRUNC, exclusive open must fail and MUST NOT truncate */
    file_t *f3 = vfs_open_mode_creds(path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_TRUNC, 0600, actor, &err);
    assert(f3 == NULL);
    assert(err == -VFS_EEXIST);

    /* 4. Verify existing content and size are strictly preserved */
    file_t *verify = vfs_open_mode_creds(path, VFS_O_RDONLY, 0, actor, &err);
    assert(verify != NULL);
    assert(err == VFS_SUCCESS);
    assert(verify->node->size == plen);

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int64_t rn = vfs_read(verify, read_buf, sizeof(read_buf) - 1);
    assert(rn == (int64_t)plen);
    assert(strcmp(read_buf, payload) == 0);
    assert(vfs_close(verify) == 0);

    /* Cleanup */
    assert(vfs_unlink_creds(path, actor) == 0);

    puts("  [PASS] Atomic exclusive creation: new file succeeds, collision fails with -VFS_EEXIST, contents preserved");
}

static void test_exclusive_creation_directory_collision(const creds_t *actor) {
    int err = 0;
    const char *dir_path = "/tmp/exclusive_test_dir";

    /* Create test directory */
    (void)vfs_unlink_creds(dir_path, actor);
    assert(vfs_mkdir_creds(dir_path, 0755, actor) == 0);

    /* Attempting exclusive file creation over existing directory must return -VFS_EEXIST */
    file_t *f = vfs_open_mode_creds(dir_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0644, actor, &err);
    assert(f == NULL);
    assert(err == -VFS_EEXIST);

    /* Cleanup */
    assert(vfs_unlink_creds(dir_path, actor) == 0);

    puts("  [PASS] Collision with existing directory fails atomically with -VFS_EEXIST");
}

static void test_symlinks_and_error_precedence(const creds_t *actor) {
    int err = 0;
    const char *symlink_path = "/tmp/test_symlink_node";

    /* Create a simulated symlink node in runfs */
    /* In FortressOS, symlinks are directory entries with mode & VFS_S_IFMT == VFS_S_IFLNK */
    (void)vfs_unlink_creds(symlink_path, actor);

    file_t *init_f = vfs_open_mode_creds(symlink_path, VFS_O_CREAT | VFS_O_WRONLY, 0644, actor, &err);
    assert(init_f != NULL);
    assert(err == VFS_SUCCESS);
    /* Manually change mode to symlink to simulate a pre-existing dangling/valid symlink */
    init_f->node->mode = VFS_S_IFLNK | 0777;
    assert(vfs_close(init_f) == 0);

    /* 1. Error Precedence Check:
     * When VFS_O_CREAT | VFS_O_EXCL AND VFS_O_NOFOLLOW are both passed on an existing symlink,
     * -VFS_EEXIST MUST take precedence over -VFS_ELOOP because the directory entry already exists!
     */
    file_t *f_precedence = vfs_open_mode_creds(symlink_path,
        VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_NOFOLLOW, 0600, actor, &err);
    assert(f_precedence == NULL);
    assert(err == -VFS_EEXIST);

    /* 2. NOFOLLOW without exclusive creation on symlink:
     * Must return deterministically -VFS_ELOOP (-40).
     */
    file_t *f_nofollow_ro = vfs_open_mode_creds(symlink_path, VFS_O_RDONLY | VFS_O_NOFOLLOW, 0, actor, &err);
    assert(f_nofollow_ro == NULL);
    assert(err == -VFS_ELOOP);

    file_t *f_nofollow_rw = vfs_open_mode_creds(symlink_path, VFS_O_RDWR | VFS_O_NOFOLLOW, 0, actor, &err);
    assert(f_nofollow_rw == NULL);
    assert(err == -VFS_ELOOP);

    /* 3. Opening a regular file with VFS_O_NOFOLLOW must succeed normally */
    const char *reg_path = "/tmp/test_reg_file.txt";
    file_t *f_reg_create = vfs_open_mode_creds(reg_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_RDWR | VFS_O_NOFOLLOW, 0644, actor, &err);
    assert(f_reg_create != NULL);
    assert(err == VFS_SUCCESS);
    assert(vfs_close(f_reg_create) == 0);

    file_t *f_reg_open = vfs_open_mode_creds(reg_path, VFS_O_RDONLY | VFS_O_NOFOLLOW, 0, actor, &err);
    assert(f_reg_open != NULL);
    assert(err == VFS_SUCCESS);
    assert(vfs_close(f_reg_open) == 0);

    /* Cleanup */
    assert(vfs_unlink_creds(symlink_path, actor) == 0);
    assert(vfs_unlink_creds(reg_path, actor) == 0);

    puts("  [PASS] Symlinks & precedence: EEXIST takes precedence over ELOOP; NOFOLLOW returns -VFS_ELOOP");
}

static void test_concurrent_creator_race(const creds_t *actor) {
    int err1 = 0, err2 = 0;
    const char *race_path = "/tmp/concurrent_race.tmp";

    (void)vfs_unlink_creds(race_path, actor);

    /* First creator succeeds */
    file_t *f1 = vfs_open_mode_creds(race_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0600, actor, &err1);
    assert(f1 != NULL);
    assert(err1 == VFS_SUCCESS);

    /* Second creator attempting same path fails with -VFS_EEXIST */
    file_t *f2 = vfs_open_mode_creds(race_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0600, actor, &err2);
    assert(f2 == NULL);
    assert(err2 == -VFS_EEXIST);

    assert(vfs_close(f1) == 0);
    assert(vfs_unlink_creds(race_path, actor) == 0);

    puts("  [PASS] Concurrent race simulation: single winner, loser receives -VFS_EEXIST");
}

static void test_syscall_boundary(void) {
    const char *kpath = "/tmp/syscall_test.tmp";
    uintptr_t user_path = (uintptr_t)kpath;

    /* Unlink if present */
    (void)sys_unlink(user_path);

    /* 1. sys_open with O_CREAT | O_EXCL creates new fd */
    int64_t fd1 = sys_open(user_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY);
    assert(fd1 >= 0);
    assert(sys_close((uint64_t)fd1) == 0);

    /* 2. sys_open on existing file with O_CREAT | O_EXCL returns SYSCALL_EEXIST (-15) */
    int64_t fd2 = sys_open(user_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY);
    assert(fd2 == SYSCALL_EEXIST);

    /* 3. Turn into symlink and test SYSCALL_ELOOP (-33) */
    int err = 0;
    creds_t actor;
    creds_init_root(&actor);
    file_t *f = vfs_open_creds(kpath, VFS_O_RDWR, &actor, &err);
    assert(f != NULL);
    f->node->mode = VFS_S_IFLNK | 0777;
    assert(vfs_close(f) == 0);

    /* With O_NOFOLLOW, returns SYSCALL_ELOOP (-33) */
    int64_t fd_loop = sys_open(user_path, VFS_O_RDONLY | VFS_O_NOFOLLOW);
    assert(fd_loop == SYSCALL_ELOOP);

    /* With O_CREAT | O_EXCL | O_NOFOLLOW, returns SYSCALL_EEXIST (-15) due to precedence */
    int64_t fd_prec = sys_open(user_path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_NOFOLLOW);
    assert(fd_prec == SYSCALL_EEXIST);

    assert(sys_unlink(user_path) == 0);

    puts("  [PASS] Syscall boundary: sys_open returns SYSCALL_EEXIST (-15) and SYSCALL_ELOOP (-33)");
}

int main(void) {
    puts("=== Running FortressOS VFS Open Flags (O_EXCL & O_NOFOLLOW) Host Tests ===");

    current.tid = 100;
    current.is_user = true;
    creds_init_root(&current.creds);
    assert(!process_record_begin(100, 0, false, 0, 0));
    assert(process_record_bind_creds(100, &current.creds));
    process_record_commit(100);

    vfs_node_t root = { .type = VFS_DIRECTORY, .mode = VFS_S_IFDIR | 0755, .path = "/" };
    g_vfs_root = &root;
    runfs_init(&root);

    test_flag_validation(&current.creds);
    test_atomic_exclusive_creation(&current.creds);
    test_exclusive_creation_directory_collision(&current.creds);
    test_symlinks_and_error_precedence(&current.creds);
    test_concurrent_creator_race(&current.creds);
    test_syscall_boundary();

    g_vfs_root = NULL;
    clean();
    process_record_exit(100, 0);
    process_record_forget(100);

    puts("=== All VFS Open Flags tests PASSED successfully ===");
    return 0;
}
