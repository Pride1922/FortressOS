/* Host tests for FortressOS tar tool under AddressSanitizer and UndefinedBehaviorSanitizer.
 * Verifies byte-exact extraction, listing, all rejection cases, Option A changed-archive
 * policies, and failure reporting with retained partial destination.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "digest.h"

#define MOCK_MAX_FILES 300
#define MOCK_MAX_DATA  (2 * 1024 * 1024)

typedef struct {
    char path[256];
    uint8_t *data;
    size_t size;
    size_t cap;
    bool is_dir;
    bool exists;
    uint32_t mode;
} mock_node_t;

typedef struct {
    bool open;
    int node_idx;
    size_t pos;
    int flags;
} mock_fd_t;

static mock_node_t mock_nodes[MOCK_MAX_FILES];
static size_t mock_node_count = 0;
static mock_fd_t mock_fds[32];

static char captured_stdout[65536];
static size_t stdout_len = 0;
static char captured_stderr[65536];
static size_t stderr_len = 0;

static int open_count = 0;
static int close_count = 0;

/* Fault injection hooks */
static int fail_write_after_bytes = -1;
static size_t total_written_bytes = 0;
static int archive_change_on_open = -1;
static int mutate_archive_node = -1;

static void mock_reset(void) {
    for (size_t i = 0; i < mock_node_count; i++) {
        if (mock_nodes[i].data) free(mock_nodes[i].data);
    }
    memset(mock_nodes, 0, sizeof(mock_nodes));
    mock_node_count = 0;
    memset(mock_fds, 0, sizeof(mock_fds));
    stdout_len = 0;
    stderr_len = 0;
    captured_stdout[0] = '\0';
    captured_stderr[0] = '\0';
    open_count = 0;
    close_count = 0;
    fail_write_after_bytes = -1;
    total_written_bytes = 0;
    archive_change_on_open = -1;
    mutate_archive_node = -1;
}

static int mock_find_node(const char *path) {
    for (size_t i = 0; i < mock_node_count; i++) {
        if (mock_nodes[i].exists && strcmp(mock_nodes[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int mock_add_file(const char *path, const void *data, size_t size, uint32_t mode) {
    int idx = mock_find_node(path);
    if (idx < 0) {
        assert(mock_node_count < MOCK_MAX_FILES);
        idx = (int)mock_node_count++;
    } else {
        if (mock_nodes[idx].data) free(mock_nodes[idx].data);
    }
    mock_node_t *n = &mock_nodes[idx];
    size_t plen = strlen(path);
    if (plen >= sizeof(n->path)) plen = sizeof(n->path) - 1;
    memcpy(n->path, path, plen);
    n->path[plen] = '\0';
    n->is_dir = false;
    n->exists = true;
    n->mode = mode;
    n->size = size;
    n->cap = size > 64 ? size : 64;
    n->data = (uint8_t *)malloc(n->cap);
    if (size > 0 && data != NULL) {
        memcpy(n->data, data, size);
    }
    return idx;
}

static int mock_add_dir(const char *path) {
    int idx = mock_find_node(path);
    if (idx >= 0) return idx;
    assert(mock_node_count < MOCK_MAX_FILES);
    idx = (int)mock_node_count++;
    mock_node_t *n = &mock_nodes[idx];
    size_t plen = strlen(path);
    if (plen >= sizeof(n->path)) plen = sizeof(n->path) - 1;
    memcpy(n->path, path, plen);
    n->path[plen] = '\0';
    n->is_dir = true;
    n->exists = true;
    n->mode = 0755;
    n->size = 0;
    n->data = NULL;
    return idx;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        int idx = mock_find_node(path);
        if (idx < 0) return SYSCALL_ENOENT;
        st->size = mock_nodes[idx].size;
        st->type = mock_nodes[idx].is_dir ? VFS_DIRECTORY : VFS_FILE;
        st->mode = mock_nodes[idx].mode;
        return 0;
    }
    if (nr == SYS_MKDIR) {
        const char *path = (const char *)a;
        int idx = mock_find_node(path);
        if (idx >= 0) return SYSCALL_EEXIST;
        mock_add_dir(path);
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        int flags = (int)b;
        open_count++;
        if (archive_change_on_open == open_count && mutate_archive_node >= 0) {
            /* Tamper with archive */
            mock_node_t *an = &mock_nodes[mutate_archive_node];
            if (an->size > 200) {
                an->data[100] ^= 0xff;
            }
        }
        int idx = mock_find_node(path);
        if (flags & VFS_O_CREAT) {
            if (idx >= 0) {
                mock_nodes[idx].size = 0;
            } else {
                idx = mock_add_file(path, NULL, 0, (uint32_t)c);
            }
        } else {
            if (idx < 0) return SYSCALL_ENOENT;
            if (mock_nodes[idx].is_dir) return SYSCALL_EISDIR;
        }
        for (int i = 3; i < 32; i++) {
            if (!mock_fds[i].open) {
                mock_fds[i].open = true;
                mock_fds[i].node_idx = idx;
                mock_fds[i].pos = 0;
                mock_fds[i].flags = flags;
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        if (fd >= 3 && fd < 32 && mock_fds[fd].open) {
            mock_fds[fd].open = false;
            close_count++;
            return 0;
        }
        return SYSCALL_EBADF;
    }
    if (nr == SYS_READ) {
        int fd = (int)a;
        if (fd < 3 || fd >= 32 || !mock_fds[fd].open) return SYSCALL_EBADF;
        mock_fd_t *f = &mock_fds[fd];
        mock_node_t *n = &mock_nodes[f->node_idx];
        size_t avail = (f->pos < n->size) ? (n->size - f->pos) : 0;
        size_t to_read = (avail < c) ? avail : c;
        if (to_read > 0) {
            memcpy((void *)b, n->data + f->pos, to_read);
            f->pos += to_read;
        }
        return (long)to_read;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        if (fd == 1) {
            size_t n = c;
            if (stdout_len + n >= sizeof(captured_stdout)) n = sizeof(captured_stdout) - stdout_len - 1;
            memcpy(captured_stdout + stdout_len, (const void *)b, n);
            stdout_len += n;
            captured_stdout[stdout_len] = '\0';
            return (long)c;
        }
        if (fd == 2) {
            size_t n = c;
            if (stderr_len + n >= sizeof(captured_stderr)) n = sizeof(captured_stderr) - stderr_len - 1;
            memcpy(captured_stderr + stderr_len, (const void *)b, n);
            stderr_len += n;
            captured_stderr[stderr_len] = '\0';
            return (long)c;
        }
        if (fd < 3 || fd >= 32 || !mock_fds[fd].open) return SYSCALL_EBADF;
        if (fail_write_after_bytes >= 0 && (long)(total_written_bytes + c) > fail_write_after_bytes) {
            return SYSCALL_EIO;
        }
        mock_fd_t *f = &mock_fds[fd];
        mock_node_t *n = &mock_nodes[f->node_idx];
        if (f->pos + c > n->cap) {
            size_t ncap = (f->pos + c) * 2;
            n->data = (uint8_t *)realloc(n->data, ncap);
            n->cap = ncap;
        }
        memcpy(n->data + f->pos, (const void *)b, c);
        f->pos += c;
        if (f->pos > n->size) n->size = f->pos;
        total_written_bytes += c;
        return (long)c;
    }
    assert(!"unhandled syscall in mock");
    return SYSCALL_ENOSYS;
}

/* Helper to build USTAR archives */
typedef struct {
    uint8_t *buf;
    size_t size;
    size_t cap;
} archive_builder_t;

static void ab_init(archive_builder_t *ab) {
    ab->cap = 65536;
    ab->size = 0;
    ab->buf = (uint8_t *)malloc(ab->cap);
}

static void ab_free(archive_builder_t *ab) {
    if (ab->buf) free(ab->buf);
    ab->buf = NULL;
    ab->size = 0;
    ab->cap = 0;
}

static void ab_append(archive_builder_t *ab, const void *data, size_t len) {
    if (ab->size + len > ab->cap) {
        while (ab->size + len > ab->cap) ab->cap *= 2;
        ab->buf = (uint8_t *)realloc(ab->buf, ab->cap);
    }
    memcpy(ab->buf + ab->size, data, len);
    ab->size += len;
}

static void build_header(uint8_t *hdr, const char *name, const char *prefix,
                         uint32_t mode, uint64_t size, char typeflag) {
    memset(hdr, 0, 512);
    if (name) {
        size_t nl = strlen(name);
        if (nl > 100) nl = 100;
        memcpy(hdr, name, nl);
    }
    snprintf((char *)hdr + 100, 8, "%07o", mode);
    snprintf((char *)hdr + 108, 8, "%07o", 0);
    snprintf((char *)hdr + 116, 8, "%07o", 0);
    snprintf((char *)hdr + 124, 12, "%011llo", (unsigned long long)size);
    snprintf((char *)hdr + 136, 12, "%011o", 0);
    hdr[156] = typeflag;
    memcpy(hdr + 257, "ustar\0", 6);
    memcpy(hdr + 263, "00", 2);
    if (prefix) {
        size_t pl = strlen(prefix);
        if (pl > 155) pl = 155;
        memcpy(hdr + 345, prefix, pl);
    }

    /* Checksum: 8 spaces at 148..155 */
    memset(hdr + 148, ' ', 8);
    uint32_t chk = 0;
    for (size_t i = 0; i < 512; i++) chk += hdr[i];
    snprintf((char *)hdr + 148, 7, "%06o", chk);
    hdr[154] = '\0';
    hdr[155] = ' ';
}

static void ab_add_file(archive_builder_t *ab, const char *name, const char *prefix,
                        const void *content, size_t size, uint32_t mode) {
    uint8_t hdr[512];
    build_header(hdr, name, prefix, mode, size, '0');
    ab_append(ab, hdr, 512);
    if (size > 0 && content != NULL) {
        ab_append(ab, content, size);
    }
    size_t pad = (512 - (size % 512)) % 512;
    if (pad > 0) {
        uint8_t zeros[512] = {0};
        ab_append(ab, zeros, pad);
    }
}

static void ab_add_dir(archive_builder_t *ab, const char *name, const char *prefix, uint32_t mode) {
    uint8_t hdr[512];
    build_header(hdr, name, prefix, mode, 0, '5');
    ab_append(ab, hdr, 512);
}

static void ab_finalize(archive_builder_t *ab) {
    uint8_t zeros[1024] = {0};
    ab_append(ab, zeros, 1024);
}

static void test_help_and_usage(void) {
    mock_reset();
    char *argv_help[] = {"tar", "--help"};
    int r = tar_main(2, argv_help);
    assert(r == 0);
    assert(strstr(captured_stdout, "Usage: tar") != NULL);

    /* Missing mode */
    mock_reset();
    char *argv_no_mode[] = {"tar", "-f", "archive.tar"};
    r = tar_main(3, argv_no_mode);
    assert(r == 2);

    /* Both -t and -x */
    mock_reset();
    char *argv_both[] = {"tar", "-t", "-x", "-f", "archive.tar"};
    r = tar_main(5, argv_both);
    assert(r == 2);

    /* Missing archive */
    mock_reset();
    char *argv_no_f[] = {"tar", "-tf"};
    r = tar_main(2, argv_no_f);
    assert(r == 2);

    /* Extract without -C */
    mock_reset();
    char *argv_no_c[] = {"tar", "-xf", "archive.tar"};
    r = tar_main(3, argv_no_c);
    assert(r == 2);

    /* List with -C */
    mock_reset();
    char *argv_list_c[] = {"tar", "-tf", "archive.tar", "-C", "dest"};
    r = tar_main(5, argv_list_c);
    assert(r == 2);

    /* Unknown flag */
    mock_reset();
    char *argv_bad_flag[] = {"tar", "-zxf", "archive.tar", "-C", "dest"};
    r = tar_main(5, argv_bad_flag);
    assert(r == 2);
}

static void test_valid_archive_list_and_extract(void) {
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_dir(&ab, "dir1/", NULL, 0755);
    ab_add_file(&ab, "file1.txt", NULL, "Hello FortressOS!\n", 18, 0644);
    ab_add_dir(&ab, "dir1/nested/", NULL, 0755);
    ab_add_file(&ab, "dir1/nested/data.bin", NULL, "Binary123456", 12, 0600);
    ab_finalize(&ab);

    /* 1. Test listing (-tf) */
    mock_reset();
    mock_add_file("archive.tar", ab.buf, ab.size, 0644);
    char *argv_list[] = {"tar", "-tf", "archive.tar"};
    int r = tar_main(3, argv_list);
    assert(r == 0);
    assert(strstr(captured_stdout, "dir1/\n") != NULL);
    assert(strstr(captured_stdout, "file1.txt\n") != NULL);
    assert(strstr(captured_stdout, "dir1/nested/\n") != NULL);
    assert(strstr(captured_stdout, "dir1/nested/data.bin\n") != NULL);

    /* 2. Test byte-exact extraction (-xf ARCHIVE -C NEW_DIR) */
    mock_reset();
    mock_add_file("archive.tar", ab.buf, ab.size, 0644);
    char *argv_extract[] = {"tar", "-xf", "archive.tar", "-C", "extracted"};
    r = tar_main(5, argv_extract);
    assert(r == 0);

    /* Verify files and directories in mock filesystem */
    int f1 = mock_find_node("extracted/file1.txt");
    assert(f1 >= 0 && !mock_nodes[f1].is_dir);
    assert(mock_nodes[f1].size == 18);
    assert(memcmp(mock_nodes[f1].data, "Hello FortressOS!\n", 18) == 0);

    int f2 = mock_find_node("extracted/dir1/nested/data.bin");
    assert(f2 >= 0 && !mock_nodes[f2].is_dir);
    assert(mock_nodes[f2].size == 12);
    assert(memcmp(mock_nodes[f2].data, "Binary123456", 12) == 0);

    int d1 = mock_find_node("extracted/dir1");
    assert(d1 >= 0 && mock_nodes[d1].is_dir);
    int d2 = mock_find_node("extracted/dir1/nested");
    assert(d2 >= 0 && mock_nodes[d2].is_dir);

    ab_free(&ab);
}

static void test_reject_existing_destination(void) {
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_file(&ab, "hello.txt", NULL, "hello", 5, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("archive.tar", ab.buf, ab.size, 0644);
    mock_add_dir("already_exists");

    char *argv[] = {"tar", "-xf", "archive.tar", "-C", "already_exists"};
    int r = tar_main(5, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "destination already exists:") != NULL);
    assert(strstr(captured_stderr, "already_exists") != NULL);

    /* Verify hello.txt was not written into already_exists */
    assert(mock_find_node("already_exists/hello.txt") < 0);

    ab_free(&ab);
}

static void test_changed_archive_pass2_abort_before_create(void) {
    /* If archive changes between validation pass 1 and pass 2:
     * MUST abort before creating destination.
     * Destination directory MUST NOT exist.
     */
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_file(&ab, "test.txt", NULL, "content", 7, 0644);
    ab_finalize(&ab);

    mock_reset();
    int arc_idx = mock_add_file("archive.tar", ab.buf, ab.size, 0644);

    /* Open 1 is pass 1; Open 2 is pass 2. Tamper archive on Open 2. */
    archive_change_on_open = 2;
    mutate_archive_node = arc_idx;

    char *argv[] = {"tar", "-xf", "archive.tar", "-C", "new_dest"};
    int r = tar_main(5, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "archive changed between validation passes") != NULL);

    /* Verify destination directory was NOT created */
    assert(mock_find_node("new_dest") < 0);
    assert(mock_find_node("new_dest/test.txt") < 0);

    ab_free(&ab);
}

static void test_extraction_failure_retained_destination_reported(void) {
    /* If extraction fails partway (Option A):
     * Destination exists and is retained.
     * Tool reports retained partial directory and exits nonzero.
     */
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_file(&ab, "first.txt", NULL, "first file content\n", 19, 0644);
    ab_add_file(&ab, "second.txt", NULL, "second file content\n", 20, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("archive.tar", ab.buf, ab.size, 0644);

    /* Fail write after 10 bytes written */
    fail_write_after_bytes = 10;

    char *argv[] = {"tar", "-xf", "archive.tar", "-C", "partial_dest"};
    int r = tar_main(5, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "retained partial directory:") != NULL);
    assert(strstr(captured_stderr, "partial_dest") != NULL);

    /* Verify destination directory still exists (was retained, not deleted) */
    assert(mock_find_node("partial_dest") >= 0);

    ab_free(&ab);
}

static void test_rejection_symlink(void) {
    archive_builder_t ab;
    ab_init(&ab);
    uint8_t hdr[512];
    build_header(hdr, "symlink_file", NULL, 0777, 0, '2');
    ab_append(&ab, hdr, 512);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("symlink.tar", ab.buf, ab.size, 0644);
    char *argv[] = {"tar", "-xf", "symlink.tar", "-C", "dest"};
    int r = tar_main(5, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "symlinks not supported") != NULL);
    assert(mock_find_node("dest") < 0);
    ab_free(&ab);
}

static void test_rejection_hardlink(void) {
    archive_builder_t ab;
    ab_init(&ab);
    uint8_t hdr[512];
    build_header(hdr, "hardlink_file", NULL, 0777, 0, '1');
    ab_append(&ab, hdr, 512);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("hardlink.tar", ab.buf, ab.size, 0644);
    char *argv[] = {"tar", "-xf", "hardlink.tar", "-C", "dest"};
    int r = tar_main(5, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "hard links not supported") != NULL);
    assert(mock_find_node("dest") < 0);
    ab_free(&ab);
}

static void test_rejection_devices_and_fifos(void) {
    for (char type = '3'; type <= '6'; type++) {
        if (type == '5') continue; /* '5' is directory */
        archive_builder_t ab;
        ab_init(&ab);
        uint8_t hdr[512];
        build_header(hdr, "special_file", NULL, 0600, 0, type);
        ab_append(&ab, hdr, 512);
        ab_finalize(&ab);

        mock_reset();
        mock_add_file("special.tar", ab.buf, ab.size, 0644);
        char *argv[] = {"tar", "-tf", "special.tar"};
        int r = tar_main(3, argv);
        assert(r != 0);
        ab_free(&ab);
    }
}

static void test_rejection_extensions_and_compressed(void) {
    /* PAX / GNU extensions */
    for (const char *t = "xgLK"; *t; t++) {
        archive_builder_t ab;
        ab_init(&ab);
        uint8_t hdr[512];
        build_header(hdr, "ext_file", NULL, 0644, 0, *t);
        ab_append(&ab, hdr, 512);
        ab_finalize(&ab);

        mock_reset();
        mock_add_file("ext.tar", ab.buf, ab.size, 0644);
        char *argv[] = {"tar", "-tf", "ext.tar"};
        int r = tar_main(3, argv);
        assert(r != 0);
        ab_free(&ab);
    }

    /* Compressed header: gzip */
    uint8_t gzip_data[512] = {0x1f, 0x8b, 0x08, 0x00};
    mock_reset();
    mock_add_file("archive.tar.gz", gzip_data, sizeof(gzip_data), 0644);
    char *argv_gz[] = {"tar", "-tf", "archive.tar.gz"};
    int r = tar_main(3, argv_gz);
    assert(r != 0);
    assert(strstr(captured_stderr, "gzip detected") != NULL);
}

static void test_rejection_traversal_and_absolute(void) {
    const char *bad_paths[] = {
        "/absolute/path.txt",
        "\\absolute\\win.txt",
        "../parent.txt",
        "dir/../../escape.txt",
        "dir/./dot.txt",
        "dir//empty_comp.txt"
    };
    for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); i++) {
        archive_builder_t ab;
        ab_init(&ab);
        ab_add_file(&ab, bad_paths[i], NULL, "data", 4, 0644);
        ab_finalize(&ab);

        mock_reset();
        mock_add_file("bad.tar", ab.buf, ab.size, 0644);
        char *argv[] = {"tar", "-xf", "bad.tar", "-C", "dest"};
        int r = tar_main(5, argv);
        assert(r != 0);
        assert(mock_find_node("dest") < 0);
        ab_free(&ab);
    }
}

static void test_rejection_oversized_and_caps(void) {
    /* 1. Oversized file: header declares > 16 MiB */
    archive_builder_t ab;
    ab_init(&ab);
    uint8_t hdr[512];
    build_header(hdr, "large.bin", NULL, 0644, (16ULL * 1024 * 1024) + 1, '0');
    ab_append(&ab, hdr, 512);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("large.tar", ab.buf, ab.size, 0644);
    char *argv[] = {"tar", "-tf", "large.tar"};
    int r = tar_main(3, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "16 MiB cap") != NULL);
    ab_free(&ab);

    /* 2. Path depth > 16 */
    ab_init(&ab);
    ab_add_file(&ab, "1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/deep.txt", NULL, "x", 1, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("deep.tar", ab.buf, ab.size, 0644);
    char *argv_deep[] = {"tar", "-tf", "deep.tar"};
    r = tar_main(3, argv_deep);
    assert(r != 0);
    assert(strstr(captured_stderr, "depth exceeds 16") != NULL);
    ab_free(&ab);

    /* 3. Component name >= 64 bytes */
    char long_name[70];
    memset(long_name, 'a', 65);
    long_name[65] = '\0';
    ab_init(&ab);
    ab_add_file(&ab, long_name, NULL, "x", 1, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("long_comp.tar", ab.buf, ab.size, 0644);
    char *argv_comp[] = {"tar", "-tf", "long_comp.tar"};
    r = tar_main(3, argv_comp);
    assert(r != 0);
    assert(strstr(captured_stderr, "exceeds 63 bytes") != NULL);
    ab_free(&ab);
}

static void test_rejection_duplicate_and_prefix_conflicts(void) {
    /* 1. Duplicate member path */
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_file(&ab, "dup.txt", NULL, "one", 3, 0644);
    ab_add_file(&ab, "dup.txt", NULL, "two", 3, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("dup.tar", ab.buf, ab.size, 0644);
    char *argv[] = {"tar", "-tf", "dup.tar"};
    int r = tar_main(3, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "duplicate member path") != NULL);
    ab_free(&ab);

    /* 2. File as prefix to directory/file */
    ab_init(&ab);
    ab_add_file(&ab, "a/b", NULL, "file b", 6, 0644);
    ab_add_file(&ab, "a/b/c", NULL, "file c", 6, 0644);
    ab_finalize(&ab);

    mock_reset();
    mock_add_file("conflict.tar", ab.buf, ab.size, 0644);
    char *argv_conf[] = {"tar", "-tf", "conflict.tar"};
    r = tar_main(3, argv_conf);
    assert(r != 0);
    assert(strstr(captured_stderr, "prefix conflict") != NULL);
    ab_free(&ab);
}

static void test_rejection_truncated_and_nonzero_padding(void) {
    /* 1. Truncated before two zero end blocks */
    archive_builder_t ab;
    ab_init(&ab);
    ab_add_file(&ab, "file.txt", NULL, "hello", 5, 0644);
    /* No zero end blocks appended */

    mock_reset();
    mock_add_file("trunc.tar", ab.buf, ab.size, 0644);
    char *argv[] = {"tar", "-tf", "trunc.tar"};
    int r = tar_main(3, argv);
    assert(r != 0);
    assert(strstr(captured_stderr, "missing two zero end blocks") != NULL);
    ab_free(&ab);

    /* 2. Non-zero trailing data */
    ab_init(&ab);
    ab_add_file(&ab, "file.txt", NULL, "hello", 5, 0644);
    ab_finalize(&ab);
    uint8_t garbage[512] = {0};
    garbage[10] = 0x42;
    ab_append(&ab, garbage, 512);

    mock_reset();
    mock_add_file("trailing.tar", ab.buf, ab.size, 0644);
    char *argv_trail[] = {"tar", "-tf", "trailing.tar"};
    r = tar_main(3, argv_trail);
    assert(r != 0);
    assert(strstr(captured_stderr, "non-zero data after end") != NULL);
    ab_free(&ab);
}

int main(void) {
    printf("[HOST TEST] Starting USTAR tar host verification suite...\n");
    test_help_and_usage();
    test_valid_archive_list_and_extract();
    test_reject_existing_destination();
    test_changed_archive_pass2_abort_before_create();
    test_extraction_failure_retained_destination_reported();
    test_rejection_symlink();
    test_rejection_hardlink();
    test_rejection_devices_and_fifos();
    test_rejection_extensions_and_compressed();
    test_rejection_traversal_and_absolute();
    test_rejection_oversized_and_caps();
    test_rejection_duplicate_and_prefix_conflicts();
    test_rejection_truncated_and_nonzero_padding();

    mock_reset();
    printf("PASS: All host USTAR tar unit and integration tests passed under ASan/UBSan.\n");
    return 0;
}
