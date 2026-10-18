#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "vfs.h"
#include "../user/logrotate/logrotate.h"

static void host_str_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void host_str_cat(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = strlen(dst);
    if (len >= cap) return;
    size_t i = 0;
    if (src) {
        while (src[i] && len + 1 < cap) {
            dst[len++] = src[i++];
        }
    }
    dst[len] = '\0';
}

/* =========================================================================
 * Mock In-Memory Environment
 * ========================================================================= */

#define MOCK_MAX_FILES 32

typedef struct {
    char path[LOGROTATE_MAX_PATH];
    uint64_t size;
    uint32_t mode;
    char content[LOGROTATE_CONFIG_MAX_SIZE + 64];
    size_t content_len;
    bool exists;
} mock_file_t;

typedef struct {
    mock_file_t files[MOCK_MAX_FILES];
    char stderr_buf[8192];
    size_t stderr_len;
    bool fail_gzip_spawn;
    int gzip_exit_code;
    int gzip_call_count;
} mock_env_t;

static mock_env_t g_mock;

static void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));
}

static mock_file_t *mock_find_file(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (g_mock.files[i].exists && strcmp(g_mock.files[i].path, path) == 0) {
            return &g_mock.files[i];
        }
    }
    return NULL;
}

static mock_file_t *mock_add_file(const char *path, uint64_t size, uint32_t mode, const char *content) {
    if (!path) return NULL;
    mock_file_t *f = mock_find_file(path);
    if (!f) {
        for (int i = 0; i < MOCK_MAX_FILES; i++) {
            if (!g_mock.files[i].exists) {
                f = &g_mock.files[i];
                memset(f, 0, sizeof(*f));
                host_str_copy(f->path, path, sizeof(f->path));
                f->exists = true;
                break;
            }
        }
    }
    if (!f) return NULL;
    f->size = size;
    f->mode = mode;
    if (content) {
        size_t clen = strlen(content);
        if (clen >= sizeof(f->content)) clen = sizeof(f->content) - 1;
        memcpy(f->content, content, clen);
        f->content[clen] = '\0';
        f->content_len = clen;
        if (size == 0) f->size = clen;
    } else {
        f->content[0] = '\0';
        f->content_len = 0;
    }
    return f;
}

static bool mock_has_file(const char *path) {
    return mock_find_file(path) != NULL;
}

static uint64_t mock_file_size(const char *path) {
    mock_file_t *f = mock_find_file(path);
    return f ? f->size : 0;
}

static uint32_t mock_file_mode(const char *path) {
    mock_file_t *f = mock_find_file(path);
    return f ? f->mode : 0;
}

/* Mock operations callbacks */
static int mock_ops_lstat(const char *path, logrotate_stat_t *st) {
    mock_file_t *f = mock_find_file(path);
    if (!f) return -1;
    st->size = f->size;
    st->mode = f->mode;
    return 0;
}

static int mock_ops_stat(const char *path, logrotate_stat_t *st) {
    return mock_ops_lstat(path, st);
}

static int mock_ops_rename(const char *oldpath, const char *newpath) {
    mock_file_t *f = mock_find_file(oldpath);
    if (!f) return -1;
    mock_file_t *target = mock_find_file(newpath);
    if (target && target != f) {
        target->exists = false;
    }
    host_str_copy(f->path, newpath, sizeof(f->path));
    return 0;
}

static int mock_ops_unlink(const char *path) {
    mock_file_t *f = mock_find_file(path);
    if (!f) return -1;
    f->exists = false;
    return 0;
}

static int mock_ops_open(const char *path, int flags, uint32_t mode) {
    mock_file_t *f = mock_find_file(path);
    if (!f) {
        if (flags & VFS_O_CREAT) {
            f = mock_add_file(path, 0, mode ? mode : (S_IFREG | 0644), NULL);
            if (!f) return -1;
        } else {
            return -1;
        }
    } else {
        if (flags & VFS_O_TRUNC) {
            if (mode != 0) {
                f->mode = mode;
            }
            f->size = 0;
            f->content[0] = '\0';
            f->content_len = 0;
        }
    }
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (&g_mock.files[i] == f) return i + 1;
    }
    return -1;
}

static int mock_ops_close(int fd) {
    (void)fd;
    return 0;
}

static int64_t mock_ops_read(int fd, void *buf, size_t count) {
    int idx = fd - 1;
    if (idx < 0 || idx >= MOCK_MAX_FILES || !g_mock.files[idx].exists) return -1;
    mock_file_t *f = &g_mock.files[idx];
    size_t to_read = count;
    if (to_read > f->content_len) to_read = f->content_len;
    memcpy(buf, f->content, to_read);
    return (int64_t)to_read;
}

static int64_t mock_ops_write(int fd, const void *buf, size_t count) {
    int idx = fd - 1;
    if (idx < 0 || idx >= MOCK_MAX_FILES || !g_mock.files[idx].exists) return -1;
    mock_file_t *f = &g_mock.files[idx];
    size_t to_write = count;
    if (to_write >= sizeof(f->content)) to_write = sizeof(f->content) - 1;
    memcpy(f->content, buf, to_write);
    f->content[to_write] = '\0';
    f->content_len = to_write;
    f->size = to_write;
    return (int64_t)to_write;
}

static int64_t mock_ops_spawn(const char *path, const char *const argv[]) {
    if (strcmp(path, "/bin/gzip") == 0) {
        g_mock.gzip_call_count++;
        if (g_mock.fail_gzip_spawn) return -1;
        /* Emulate gzip -f <target>:
         * target is argv[2]. Compress into target.gz and remove target. */
        if (argv && argv[1] && strcmp(argv[1], "-f") == 0 && argv[2]) {
            const char *target = argv[2];
            mock_file_t *f = mock_find_file(target);
            if (f) {
                char gz_path[LOGROTATE_MAX_PATH + 16];
                snprintf(gz_path, sizeof(gz_path), "%s.gz", target);
                mock_add_file(gz_path, f->size / 2 + 1, S_IFREG | 0644, NULL);
                f->exists = false; /* gzip deletes original file */
            }
        }
        return 4200; /* PID */
    }
    return -1;
}

static int64_t mock_ops_waitpid(int64_t pid, uint64_t *status, int options) {
    (void)options;
    if (pid == 4200) {
        if (status) {
            *status = ((uint64_t)(g_mock.gzip_exit_code & 0xff)) << 8;
        }
        return pid;
    }
    return -1;
}

static void mock_ops_sleep_ms(uint32_t ms) {
    (void)ms;
}

static void mock_ops_log_err(const char *msg) {
    if (!msg) return;
    size_t len = strlen(msg);
    if (g_mock.stderr_len + len < sizeof(g_mock.stderr_buf)) {
        memcpy(g_mock.stderr_buf + g_mock.stderr_len, msg, len);
        g_mock.stderr_len += len;
        g_mock.stderr_buf[g_mock.stderr_len] = '\0';
    }
}

static logrotate_ops_t g_mock_ops = {
    .lstat = mock_ops_lstat,
    .stat = mock_ops_stat,
    .rename = mock_ops_rename,
    .unlink = mock_ops_unlink,
    .open = mock_ops_open,
    .close = mock_ops_close,
    .read = mock_ops_read,
    .write = mock_ops_write,
    .spawn = mock_ops_spawn,
    .waitpid = mock_ops_waitpid,
    .sleep_ms = mock_ops_sleep_ms,
    .log_err = mock_ops_log_err
};

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static void test_rotate_mock_11mb_with_size_10m(void) {
    printf("[TEST] test_rotate_mock_11mb_with_size_10m...\n");
    mock_reset();

    const char *cfg_content =
        "/var/log/watchtower/watchtower.log {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "    missingok\n"
        "}\n";

    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg_content);
    /* Active file size: 11 MB = 11 * 1024 * 1024 = 11534336 bytes */
    uint64_t size_11m = 11ULL * 1024ULL * 1024ULL;
    mock_add_file("/var/log/watchtower/watchtower.log", size_11m, S_IFREG | 0640, NULL);

    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
    int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc == 0);

    /* Verify file.1.gz exists and active file exists and is empty */
    assert(mock_has_file("/var/log/watchtower/watchtower.log.1.gz"));
    assert(!mock_has_file("/var/log/watchtower/watchtower.log.1")); /* Deleted by gzip */
    assert(mock_has_file("/var/log/watchtower/watchtower.log"));
    assert(mock_file_size("/var/log/watchtower/watchtower.log") == 0);
    assert((mock_file_mode("/var/log/watchtower/watchtower.log") & 07777) == 0640);

    printf("PASS: test_rotate_mock_11mb_with_size_10m\n");
}

static void test_run_5_times_rotate_4_archives_deleted(void) {
    printf("[TEST] test_run_5_times_rotate_4_archives_deleted...\n");
    mock_reset();

    const char *cfg_content =
        "/var/log/test.log {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "    missingok\n"
        "}\n";

    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg_content);
    uint64_t size_11m = 11ULL * 1024ULL * 1024ULL;

    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};

    for (int run = 1; run <= 5; run++) {
        /* Refill active file to 11MB */
        mock_file_t *f = mock_find_file("/var/log/test.log");
        if (f) {
            f->size = size_11m;
        } else {
            mock_add_file("/var/log/test.log", size_11m, S_IFREG | 0644, NULL);
        }

        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);
    }

    /* After 5 rotations with rotate 4:
     * Only 4 archives should exist (.1.gz, .2.gz, .3.gz, .4.gz).
     * .5.gz must NOT exist! */
    assert(mock_has_file("/var/log/test.log.1.gz"));
    assert(mock_has_file("/var/log/test.log.2.gz"));
    assert(mock_has_file("/var/log/test.log.3.gz"));
    assert(mock_has_file("/var/log/test.log.4.gz"));
    assert(!mock_has_file("/var/log/test.log.5.gz"));
    assert(!mock_has_file("/var/log/test.log.5"));

    /* Active file is empty */
    assert(mock_has_file("/var/log/test.log"));
    assert(mock_file_size("/var/log/test.log") == 0);

    printf("PASS: test_run_5_times_rotate_4_archives_deleted\n");
}

static void test_symlink_rejection(void) {
    printf("[TEST] test_symlink_rejection...\n");
    mock_reset();

    const char *cfg_content =
        "/var/log/symlink.log {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "}\n";

    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg_content);
    /* Target is a symlink */
    mock_add_file("/var/log/symlink.log", 11ULL * 1024ULL * 1024ULL, S_IFLNK | 0777, NULL);

    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
    int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc != 0); /* Rejected! */

    /* Verify error logged */
    assert(strstr(g_mock.stderr_buf, "symlink") != NULL);

    /* Verify symlink was NOT rotated */
    assert(!mock_has_file("/var/log/symlink.log.1"));
    assert(!mock_has_file("/var/log/symlink.log.1.gz"));
    assert(mock_has_file("/var/log/symlink.log"));

    printf("PASS: test_symlink_rejection\n");
}

static void test_missing_file_handling(void) {
    printf("[TEST] test_missing_file_handling...\n");
    mock_reset();

    /* 1. With missingok: no error */
    const char *cfg_missingok =
        "/var/log/nonexistent.log {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    missingok\n"
        "}\n";
    mock_add_file("/etc/logrotate_mok.conf", 0, S_IFREG | 0644, cfg_missingok);
    char *argv1[] = {"/sbin/logrotate", "/etc/logrotate_mok.conf", NULL};
    int rc1 = logrotate_run_with_ops(2, argv1, &g_mock_ops);
    assert(rc1 == 0);
    assert(g_mock.stderr_len == 0);

    /* 2. Without missingok: error */
    const char *cfg_strict =
        "/var/log/nonexistent.log {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "}\n";
    mock_add_file("/etc/logrotate_strict.conf", 0, S_IFREG | 0644, cfg_strict);
    char *argv2[] = {"/sbin/logrotate", "/etc/logrotate_strict.conf", NULL};
    int rc2 = logrotate_run_with_ops(2, argv2, &g_mock_ops);
    assert(rc2 != 0);
    assert(strstr(g_mock.stderr_buf, "does not exist") != NULL);

    printf("PASS: test_missing_file_handling\n");
}

static void test_malformed_config_fail_closed(void) {
    printf("[TEST] test_malformed_config_fail_closed...\n");

    /* Subtest: Relative path */
    {
        mock_reset();
        const char *bad = "var/log/test.log {\n size 10M\n rotate 4\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "path must be absolute") != NULL);
    }

    /* Subtest: Rotate count > 10 */
    {
        mock_reset();
        const char *bad = "/var/log/test.log {\n size 10M\n rotate 11\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "rotate count cannot exceed 10") != NULL);
    }

    /* Subtest: Rotate negative / non-integer */
    {
        mock_reset();
        const char *bad = "/var/log/test.log {\n size 10M\n rotate -1\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
    }

    /* Subtest: Invalid size suffix */
    {
        mock_reset();
        const char *bad = "/var/log/test.log {\n size 10X\n rotate 4\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "invalid size suffix") != NULL);
    }

    /* Subtest: Unknown directive */
    {
        mock_reset();
        const char *bad = "/var/log/test.log {\n bogus_directive 123\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "unknown directive") != NULL);
    }

    /* Subtest: Unclosed block */
    {
        mock_reset();
        const char *bad = "/var/log/test.log {\n size 10M\n rotate 4\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "unclosed block") != NULL);
    }

    /* Subtest: Missing opening brace */
    {
        mock_reset();
        const char *bad = "/var/log/test.log\n size 10M\n rotate 4\n }\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, bad);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "expected '{'") != NULL);
    }

    /* Subtest: Exceeded 10 entries */
    {
        mock_reset();
        char buf[2048];
        buf[0] = '\0';
        for (int i = 0; i < 11; i++) {
            char entry[128];
            snprintf(entry, sizeof(entry), "/var/log/svc%d.log {\n size 1M\n rotate 2\n}\n", i);
            host_str_cat(buf, entry, sizeof(buf));
        }
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, buf);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "exceeded max 10 entries") != NULL);
    }

    printf("PASS: test_malformed_config_fail_closed\n");
}

static void test_force_flag_and_size_threshold(void) {
    printf("[TEST] test_force_flag_and_size_threshold...\n");
    mock_reset();

    const char *cfg =
        "/var/log/test.log {\n"
        "    size 10M\n"
        "    rotate 2\n"
        "    compress\n"
        "}\n";
    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);

    /* File size: 5 MB (below 10M limit) */
    mock_add_file("/var/log/test.log", 5ULL * 1024ULL * 1024ULL, S_IFREG | 0644, NULL);

    /* Run without -f: should NOT rotate */
    char *argv_normal[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
    int rc1 = logrotate_run_with_ops(2, argv_normal, &g_mock_ops);
    assert(rc1 == 0);
    assert(!mock_has_file("/var/log/test.log.1.gz"));
    assert(mock_file_size("/var/log/test.log") == 5ULL * 1024ULL * 1024ULL);

    /* Run with -f: should rotate despite size <= limit */
    char *argv_force[] = {"/sbin/logrotate", "-f", "/etc/logrotate.conf", NULL};
    int rc2 = logrotate_run_with_ops(3, argv_force, &g_mock_ops);
    assert(rc2 == 0);
    assert(mock_has_file("/var/log/test.log.1.gz"));
    assert(mock_file_size("/var/log/test.log") == 0);

    printf("PASS: test_force_flag_and_size_threshold\n");
}

static void test_uncompressed_and_compression_failure(void) {
    printf("[TEST] test_uncompressed_and_compression_failure...\n");

    /* 1. Rotation without compress */
    {
        mock_reset();
        const char *cfg =
            "/var/log/uncomp.log {\n"
            "    size 1M\n"
            "    rotate 3\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
        mock_add_file("/var/log/uncomp.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0644, NULL);

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);

        assert(mock_has_file("/var/log/uncomp.log.1"));
        assert(!mock_has_file("/var/log/uncomp.log.1.gz"));
        assert(mock_file_size("/var/log/uncomp.log") == 0);
    }

    /* 2. Compression failure fallback */
    {
        mock_reset();
        const char *cfg =
            "/var/log/comp_fail.log {\n"
            "    size 1M\n"
            "    rotate 3\n"
            "    compress\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
        mock_add_file("/var/log/comp_fail.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0644, NULL);

        /* Make gzip spawn fail */
        g_mock.fail_gzip_spawn = true;

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        /* Rotation succeeds with warning */
        assert(rc == 0);

        /* .1 remains uncompressed */
        assert(mock_has_file("/var/log/comp_fail.log.1"));
        assert(!mock_has_file("/var/log/comp_fail.log.1.gz"));
        assert(mock_file_size("/var/log/comp_fail.log") == 0);
        assert(strstr(g_mock.stderr_buf, "warning: left archive uncompressed") != NULL);
    }

    printf("PASS: test_uncompressed_and_compression_failure\n");
}

static void test_config_fallback_path(void) {
    printf("[TEST] test_config_fallback_path...\n");
    mock_reset();

    /* Primary does not exist; fallback exists */
    const char *cfg =
        "/var/log/fallback.log {\n"
        "    size 1M\n"
        "    rotate 2\n"
        "    missingok\n"
        "}\n";
    mock_add_file(LOGROTATE_CONFIG_FALLBACK, 0, S_IFREG | 0644, cfg);
    mock_add_file("/var/log/fallback.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0644, NULL);

    /* Run without config arg */
    char *argv[] = {"/sbin/logrotate", NULL};
    int rc = logrotate_run_with_ops(1, argv, &g_mock_ops);
    assert(rc == 0);

    assert(mock_has_file("/var/log/fallback.log.1"));
    assert(mock_file_size("/var/log/fallback.log") == 0);

    printf("PASS: test_config_fallback_path\n");
}

/* =========================================================================
 * Real POSIX Host Filesystem Integration Test
 * ========================================================================= */

static int real_ops_lstat(const char *path, logrotate_stat_t *st) {
    struct stat pst;
    if (lstat(path, &pst) != 0) return -1;
    st->size = (uint64_t)pst.st_size;
    st->mode = (uint32_t)pst.st_mode;
    return 0;
}

static int real_ops_stat(const char *path, logrotate_stat_t *st) {
    struct stat pst;
    if (stat(path, &pst) != 0) return -1;
    st->size = (uint64_t)pst.st_size;
    st->mode = (uint32_t)pst.st_mode;
    return 0;
}

static int real_ops_rename(const char *oldpath, const char *newpath) {
    return rename(oldpath, newpath);
}

static int real_ops_unlink(const char *path) {
    return unlink(path);
}

static int real_ops_open(const char *path, int flags, uint32_t mode) {
    int posix_flags = 0;
    if ((flags & (VFS_O_RDWR | VFS_O_WRONLY)) == VFS_O_WRONLY) posix_flags |= O_WRONLY;
    else if ((flags & (VFS_O_RDWR | VFS_O_WRONLY)) == VFS_O_RDWR) posix_flags |= O_RDWR;
    else posix_flags |= O_RDONLY;
    if (flags & VFS_O_CREAT) posix_flags |= O_CREAT;
    if (flags & VFS_O_TRUNC) posix_flags |= O_TRUNC;
    return open(path, posix_flags, (mode_t)mode);
}

static int real_ops_close(int fd) {
    return close(fd);
}

static int64_t real_ops_read(int fd, void *buf, size_t count) {
    return (int64_t)read(fd, buf, count);
}

static int64_t real_ops_write(int fd, const void *buf, size_t count) {
    return (int64_t)write(fd, buf, count);
}

static int64_t real_ops_spawn(const char *path, const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(path, (char *const *)argv);
        _exit(127);
    }
    return (int64_t)pid;
}

static int64_t real_ops_waitpid(int64_t pid, uint64_t *status, int options) {
    int st = 0;
    pid_t r = waitpid((pid_t)pid, &st, options);
    if (r > 0 && status) {
        *status = (uint64_t)st;
    }
    return (int64_t)r;
}

static void real_ops_sleep_ms(uint32_t ms) {
    usleep(ms * 1000);
}

static char g_real_stderr[4096];
static size_t g_real_stderr_len = 0;

static void real_ops_log_err(const char *msg) {
    if (!msg) return;
    size_t len = strlen(msg);
    if (g_real_stderr_len + len < sizeof(g_real_stderr)) {
        memcpy(g_real_stderr + g_real_stderr_len, msg, len);
        g_real_stderr_len += len;
        g_real_stderr[g_real_stderr_len] = '\0';
    }
}

static logrotate_ops_t g_real_ops = {
    .lstat = real_ops_lstat,
    .stat = real_ops_stat,
    .rename = real_ops_rename,
    .unlink = real_ops_unlink,
    .open = real_ops_open,
    .close = real_ops_close,
    .read = real_ops_read,
    .write = real_ops_write,
    .spawn = real_ops_spawn,
    .waitpid = real_ops_waitpid,
    .sleep_ms = real_ops_sleep_ms,
    .log_err = real_ops_log_err
};

static void test_real_posix_filesystem(void) {
    printf("[TEST] test_real_posix_filesystem...\n");

    char tmppath[] = "/tmp/logrotate_test_XXXXXX";
    char *dir = mkdtemp(tmppath);
    assert(dir != NULL);

    char log_path[LOGROTATE_MAX_PATH];
    char sym_path[LOGROTATE_MAX_PATH];
    char cfg_path[LOGROTATE_MAX_PATH];
    snprintf(log_path, sizeof(log_path), "%s/test.log", dir);
    snprintf(sym_path, sizeof(sym_path), "%s/symlink.log", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/test.conf", dir);

    /* 1. Create sparse 11MB file */
    int fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0640);
    assert(fd >= 0);
    off_t off = lseek(fd, 11 * 1024 * 1024 - 1, SEEK_SET);
    assert(off == 11 * 1024 * 1024 - 1);
    ssize_t w = write(fd, "\0", 1);
    assert(w == 1);
    close(fd);

    /* Verify size */
    struct stat pst;
    assert(stat(log_path, &pst) == 0);
    assert(pst.st_size == 11 * 1024 * 1024);

    /* 2. Create symlink */
    int sym_rc = symlink(log_path, sym_path);
    assert(sym_rc == 0);

    /* Test 2a: Symlink rejection with real host symlink */
    char sym_cfg_content[512];
    snprintf(sym_cfg_content, sizeof(sym_cfg_content),
             "%s {\n size 10M\n rotate 4\n}\n", sym_path);
    int cfd = open(cfg_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(cfd >= 0);
    ssize_t wr = write(cfd, sym_cfg_content, strlen(sym_cfg_content));
    assert(wr == (ssize_t)strlen(sym_cfg_content));
    close(cfd);

    g_real_stderr_len = 0;
    char *argv_sym[] = {"/sbin/logrotate", cfg_path, NULL};
    int rc_sym = logrotate_run_with_ops(2, argv_sym, &g_real_ops);
    assert(rc_sym != 0); /* Symlink rejected! */
    assert(strstr(g_real_stderr, "symlink") != NULL);

    /* Test 2b: Real rotation and compression of 11MB file with size 10M */
    char real_cfg_content[512];
    snprintf(real_cfg_content, sizeof(real_cfg_content),
             "%s {\n size 10M\n rotate 4\n compress\n missingok\n}\n", log_path);
    cfd = open(cfg_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(cfd >= 0);
    wr = write(cfd, real_cfg_content, strlen(real_cfg_content));
    assert(wr == (ssize_t)strlen(real_cfg_content));
    close(cfd);

    char *argv_real[] = {"/sbin/logrotate", cfg_path, NULL};
    int rc_rot = logrotate_run_with_ops(2, argv_real, &g_real_ops);
    assert(rc_rot == 0);

    /* Verify file.1.gz exists and test.log is empty with mode 0640 */
    char p1_gz[LOGROTATE_MAX_PATH + 16];
    snprintf(p1_gz, sizeof(p1_gz), "%s.1.gz", log_path);
    assert(stat(p1_gz, &pst) == 0);
    assert(pst.st_size > 0);

    assert(stat(log_path, &pst) == 0);
    assert(pst.st_size == 0);
    assert((pst.st_mode & 0777) == 0640);

    /* Clean up files */
    unlink(p1_gz);
    unlink(sym_path);
    unlink(log_path);
    unlink(cfg_path);
    rmdir(dir);

    printf("PASS: test_real_posix_filesystem\n");
}

int main(void) {
    printf("===================================================\n");
    printf("FortressOS /sbin/logrotate Host Test Suite (ASan/UBSan)\n");
    printf("===================================================\n");

    test_rotate_mock_11mb_with_size_10m();
    test_run_5_times_rotate_4_archives_deleted();
    test_symlink_rejection();
    test_missing_file_handling();
    test_malformed_config_fail_closed();
    test_force_flag_and_size_threshold();
    test_uncompressed_and_compression_failure();
    test_config_fallback_path();
    test_real_posix_filesystem();

    printf("\nALL 9 LOGROTATE HOST TESTS PASSED!\n");
    return 0;
}

