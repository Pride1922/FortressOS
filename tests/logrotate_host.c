#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
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
    bool lock_collision;
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

static int mock_ops_mkdir(const char *path, uint32_t mode) {
    if (g_mock.lock_collision && strcmp(path, LOGROTATE_LOCK_DIR) == 0) {
        return -15;
    }
    mock_file_t *f = mock_find_file(path);
    if (f) return -15;
    mock_add_file(path, 0, S_IFDIR | mode, NULL);
    return 0;
}

static int64_t mock_ops_getpid(void) {
    return 1234;
}

static int mock_ops_kill(int64_t pid, int sig) {
    (void)pid;
    (void)sig;
    return 0;
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
    .log_err = mock_ops_log_err,
    .mkdir = mock_ops_mkdir,
    .getpid = mock_ops_getpid,
    .kill = mock_ops_kill
};

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static void test_rotate_mock_11mb_with_size_10m(void) {
    printf("[TEST] test_rotate_mock_11mb_with_size_10m...\n");
    mock_reset();

    const char *cfg_content =
        "/var/log/messages {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "    missingok\n"
        "}\n";

    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg_content);
    /* Active file size: 11 MB = 11 * 1024 * 1024 = 11534336 bytes */
    uint64_t size_11m = 11ULL * 1024ULL * 1024ULL;
    mock_add_file("/var/log/messages", size_11m, S_IFREG | 0640, NULL);

    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
    int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc == 0);

    /* Under delaycompress: file.1 is retained UNCOMPRESSED in first cycle */
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));
    assert(mock_has_file("/var/log/messages"));
    assert(mock_file_size("/var/log/messages") == 0);
    assert((mock_file_mode("/var/log/messages") & 07777) == 0640);

    /* Second cycle: refill active, now messages.1 shifts to messages.2 and compresses to messages.2.gz */
    mock_file_t *f = mock_find_file("/var/log/messages");
    assert(f != NULL);
    f->size = size_11m;
    int rc2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc2 == 0);
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(!mock_has_file("/var/log/messages.2"));
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));
    assert(mock_file_size("/var/log/messages") == 0);

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

    /* After 5 rotations with rotate 4 under delaycompress:
     * .1 is uncompressed (from run 5).
     * .2.gz, .3.gz, .4.gz are compressed.
     * .5.gz and .5 must NOT exist! */
    assert(mock_has_file("/var/log/test.log.1"));
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
    assert(!mock_has_file("/var/log/test.log.1"));
    assert(!mock_has_file("/var/log/test.log.1.gz"));
    assert(mock_file_size("/var/log/test.log") == 5ULL * 1024ULL * 1024ULL);

    /* Run with -f: should rotate despite size <= limit (delaycompress leaves .1 uncompressed) */
    char *argv_force[] = {"/sbin/logrotate", "-f", "/etc/logrotate.conf", NULL};
    int rc2 = logrotate_run_with_ops(3, argv_force, &g_mock_ops);
    assert(rc2 == 0);
    assert(mock_has_file("/var/log/test.log.1"));
    assert(!mock_has_file("/var/log/test.log.1.gz"));
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

    /* 2. Compression failure fallback under delaycompress */
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

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        /* Cycle 1: active -> .1 uncompressed */
        int rc1 = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc1 == 0);
        assert(mock_has_file("/var/log/comp_fail.log.1"));

        /* Cycle 2: refill active; make gzip spawn fail during .1 -> .2 compression */
        mock_find_file("/var/log/comp_fail.log")->size = 2ULL * 1024ULL * 1024ULL;
        g_mock.fail_gzip_spawn = true;

        int rc2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
        /* Rotation succeeds with warning */
        assert(rc2 == 0);

        /* .2 remains uncompressed */
        assert(mock_has_file("/var/log/comp_fail.log.2"));
        assert(!mock_has_file("/var/log/comp_fail.log.2.gz"));
        assert(mock_has_file("/var/log/comp_fail.log.1"));
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

static char g_real_lock_dir[LOGROTATE_MAX_PATH] = "/tmp/fortress_logrotate_test_lock";
static char g_real_lock_pid[LOGROTATE_MAX_PATH] = "/tmp/fortress_logrotate_test_lock/pid";

static int real_ops_mkdir(const char *path, uint32_t mode) {
    const char *target = path;
    if (strcmp(path, LOGROTATE_LOCK_DIR) == 0) target = g_real_lock_dir;
    if (mkdir(target, (mode_t)mode) == 0) return 0;
    if (errno == EEXIST) return -15;
    return -1;
}

static int real_ops_unlink(const char *path) {
    const char *target = path;
    if (strcmp(path, LOGROTATE_LOCK_DIR) == 0) target = g_real_lock_dir;
    else if (strcmp(path, LOGROTATE_LOCK_PID) == 0) target = g_real_lock_pid;
    if (unlink(target) == 0) return 0;
    if (errno == EISDIR || errno == EPERM) {
        return rmdir(target);
    }
    return -1;
}

static int real_ops_open(const char *path, int flags, uint32_t mode) {
    const char *target = path;
    if (strcmp(path, LOGROTATE_LOCK_PID) == 0) target = g_real_lock_pid;
    int posix_flags = 0;
    if ((flags & (VFS_O_RDWR | VFS_O_WRONLY)) == VFS_O_WRONLY) posix_flags |= O_WRONLY;
    else if ((flags & (VFS_O_RDWR | VFS_O_WRONLY)) == VFS_O_RDWR) posix_flags |= O_RDWR;
    else posix_flags |= O_RDONLY;
    if (flags & VFS_O_CREAT) posix_flags |= O_CREAT;
    if (flags & VFS_O_TRUNC) posix_flags |= O_TRUNC;
    return open(target, posix_flags, (mode_t)mode);
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

static int64_t real_ops_getpid(void) {
    return (int64_t)getpid();
}

static int real_ops_kill(int64_t pid, int sig) {
    return kill((pid_t)pid, sig);
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
    .log_err = real_ops_log_err,
    .mkdir = real_ops_mkdir,
    .getpid = real_ops_getpid,
    .kill = real_ops_kill
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

    /* Test 2b: Real rotation and delaycompress of 11MB file with size 10M */
    char real_cfg_content[512];
    snprintf(real_cfg_content, sizeof(real_cfg_content),
             "%s {\n size 10M\n rotate 4\n compress\n missingok\n}\n", log_path);
    cfd = open(cfg_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(cfd >= 0);
    wr = write(cfd, real_cfg_content, strlen(real_cfg_content));
    assert(wr == (ssize_t)strlen(real_cfg_content));
    close(cfd);

    /* Cycle 1: test.log -> test.log.1 (uncompressed under delaycompress) */
    char *argv_real[] = {"/sbin/logrotate", cfg_path, NULL};
    int rc_rot1 = logrotate_run_with_ops(2, argv_real, &g_real_ops);
    assert(rc_rot1 == 0);

    char p1_raw[LOGROTATE_MAX_PATH + 16];
    char p1_gz[LOGROTATE_MAX_PATH + 16];
    char p2_gz[LOGROTATE_MAX_PATH + 16];
    snprintf(p1_raw, sizeof(p1_raw), "%s.1", log_path);
    snprintf(p1_gz, sizeof(p1_gz), "%s.1.gz", log_path);
    snprintf(p2_gz, sizeof(p2_gz), "%s.2.gz", log_path);

    assert(stat(p1_raw, &pst) == 0);
    assert(stat(p1_gz, &pst) != 0); /* Must be uncompressed in cycle 1! */
    assert(stat(log_path, &pst) == 0);
    assert(pst.st_size == 0);
    assert((pst.st_mode & 0777) == 0640);

    /* Cycle 2: Refill test.log and rotate again -> test.log.1 shifts to test.log.2 and compresses to test.log.2.gz */
    fd = open(log_path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    off = lseek(fd, 11 * 1024 * 1024 - 1, SEEK_SET);
    assert(off == 11 * 1024 * 1024 - 1);
    w = write(fd, "\0", 1);
    assert(w == 1);
    close(fd);

    int rc_rot2 = logrotate_run_with_ops(2, argv_real, &g_real_ops);
    assert(rc_rot2 == 0);

    assert(stat(p2_gz, &pst) == 0);
    assert(pst.st_size > 0);
    assert(stat(p1_raw, &pst) == 0);
    assert(stat(p1_gz, &pst) != 0);
    assert(stat(log_path, &pst) == 0);
    assert(pst.st_size == 0);

    /* Clean up files */
    unlink(p2_gz);
    unlink(p1_raw);
    unlink(sym_path);
    unlink(log_path);
    unlink(cfg_path);
    rmdir(dir);

    printf("PASS: test_real_posix_filesystem\n");
}

static void test_delaycompress_lifecycle(void) {
    printf("[TEST] test_delaycompress_lifecycle...\n");
    mock_reset();

    const char *cfg =
        "/var/log/messages {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "}\n";
    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
    uint64_t sz = 11ULL * 1024ULL * 1024ULL;

    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};

    /* Cycle 1: active -> .1 (uncompressed) */
    mock_add_file("/var/log/messages", sz, S_IFREG | 0640, NULL);
    int rc1 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc1 == 0);
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));
    assert(mock_has_file("/var/log/messages"));
    assert(mock_file_size("/var/log/messages") == 0);

    /* Cycle 2: .1 -> .2 -> .2.gz; active -> .1 */
    mock_file_t *f = mock_find_file("/var/log/messages");
    assert(f != NULL);
    f->size = sz;
    int rc2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc2 == 0);
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(!mock_has_file("/var/log/messages.2"));
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));
    assert(mock_file_size("/var/log/messages") == 0);

    /* Cycle 3: .2.gz -> .3.gz; .1 -> .2 -> .2.gz; active -> .1 */
    f = mock_find_file("/var/log/messages");
    assert(f != NULL);
    f->size = sz;
    int rc3 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc3 == 0);
    assert(mock_has_file("/var/log/messages.3.gz"));
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));

    /* Cycle 4: .3.gz -> .4.gz; .2.gz -> .3.gz; .1 -> .2 -> .2.gz; active -> .1 */
    f = mock_find_file("/var/log/messages");
    assert(f != NULL);
    f->size = sz;
    int rc4 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc4 == 0);
    assert(mock_has_file("/var/log/messages.4.gz"));
    assert(mock_has_file("/var/log/messages.3.gz"));
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(mock_has_file("/var/log/messages.1"));

    /* Cycle 5: oldest .4.gz unlinked, shifted */
    f = mock_find_file("/var/log/messages");
    assert(f != NULL);
    f->size = sz;
    int rc5 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc5 == 0);
    assert(!mock_has_file("/var/log/messages.5.gz"));
    assert(!mock_has_file("/var/log/messages.5"));
    assert(mock_has_file("/var/log/messages.4.gz"));
    assert(mock_has_file("/var/log/messages.3.gz"));
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(mock_has_file("/var/log/messages.1"));

    printf("PASS: test_delaycompress_lifecycle\n");
}

static void test_boundary_rotate_0_1_and_uncompressed(void) {
    printf("[TEST] test_boundary_rotate_0_1_and_uncompressed...\n");

    /* Subtest A: rotate 0 clears active file with mode preserved, no archives created */
    {
        mock_reset();
        const char *cfg =
            "/var/log/rot0.log {\n"
            "    size 1M\n"
            "    rotate 0\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0640, cfg);
        mock_add_file("/var/log/rot0.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0640, NULL);

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);
        assert(mock_has_file("/var/log/rot0.log"));
        assert(mock_file_size("/var/log/rot0.log") == 0);
        assert((mock_file_mode("/var/log/rot0.log") & 07777) == 0640);
        assert(!mock_has_file("/var/log/rot0.log.1"));
        assert(!mock_has_file("/var/log/rot0.log.1.gz"));
    }

    /* Subtest B: rotate 1 retains active.1 UNCOMPRESSED even if compress specified */
    {
        mock_reset();
        const char *cfg =
            "/var/log/rot1.log {\n"
            "    size 1M\n"
            "    rotate 1\n"
            "    compress\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
        mock_add_file("/var/log/rot1.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0640, NULL);

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);
        assert(mock_has_file("/var/log/rot1.log.1"));
        assert(!mock_has_file("/var/log/rot1.log.1.gz")); /* Must remain uncompressed! */
        assert(mock_has_file("/var/log/rot1.log"));
        assert(mock_file_size("/var/log/rot1.log") == 0);

        /* Rotate a second time: old .1 unlinked, new active -> .1 uncompressed */
        mock_file_t *f = mock_find_file("/var/log/rot1.log");
        assert(f != NULL);
        f->size = 2ULL * 1024ULL * 1024ULL;
        int rc2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc2 == 0);
        assert(mock_has_file("/var/log/rot1.log.1"));
        assert(!mock_has_file("/var/log/rot1.log.1.gz"));
        assert(!mock_has_file("/var/log/rot1.log.2"));
    }

    /* Subtest C: rotate 2 without compress shifts uncompressed */
    {
        mock_reset();
        const char *cfg =
            "/var/log/uncomp.log {\n"
            "    size 1M\n"
            "    rotate 2\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
        mock_add_file("/var/log/uncomp.log", 2ULL * 1024ULL * 1024ULL, S_IFREG | 0644, NULL);

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);
        assert(mock_has_file("/var/log/uncomp.log.1"));
        assert(!mock_has_file("/var/log/uncomp.log.1.gz"));

        mock_file_t *f = mock_find_file("/var/log/uncomp.log");
        f->size = 2ULL * 1024ULL * 1024ULL;
        int rc2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc2 == 0);
        assert(mock_has_file("/var/log/uncomp.log.2"));
        assert(!mock_has_file("/var/log/uncomp.log.2.gz"));
        assert(mock_has_file("/var/log/uncomp.log.1"));
    }

    printf("PASS: test_boundary_rotate_0_1_and_uncompressed\n");
}

static void test_atomic_mkdir_lock_collision_and_skip_if_locked(void) {
    printf("[TEST] test_atomic_mkdir_lock_collision_and_skip_if_locked...\n");

    /* Scenario A: Clean run creates lock, records PID, and cleans up cleanly on exit */
    {
        mock_reset();
        const char *cfg =
            "/var/log/messages {\n"
            "    size 10M\n"
            "    rotate 2\n"
            "    missingok\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);

        char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc == 0);
        /* Lock directory and PID must be cleanly released */
        assert(!mock_has_file(LOGROTATE_LOCK_PID));
        assert(!mock_has_file(LOGROTATE_LOCK_DIR));
    }

    /* Scenario B: Collision when lock already held */
    {
        mock_reset();
        const char *cfg =
            "/var/log/messages {\n"
            "    size 10M\n"
            "    rotate 2\n"
            "    missingok\n"
            "}\n";
        mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);

        /* Pre-populate lock directory and PID file with PID 9999 */
        mock_add_file(LOGROTATE_LOCK_DIR, 0, S_IFDIR | 0700, NULL);
        mock_add_file(LOGROTATE_LOCK_PID, 0, S_IFREG | 0644, "9999\n");

        /* B1: Direct invocation without --skip-if-locked: exits 1 */
        char *argv1[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
        int rc1 = logrotate_run_with_ops(2, argv1, &g_mock_ops);
        assert(rc1 == 1);
        assert(strstr(g_mock.stderr_buf, "locked by /run/logrotate.lock (pid 9999); skipping") != NULL);
        /* Lock must NOT be unlinked on collision */
        assert(mock_has_file(LOGROTATE_LOCK_DIR));
        assert(mock_has_file(LOGROTATE_LOCK_PID));

        /* B2: Cron invocation with --skip-if-locked: exits 0 */
        g_mock.stderr_len = 0;
        g_mock.stderr_buf[0] = '\0';
        char *argv2[] = {"/sbin/logrotate", "--skip-if-locked", "/etc/logrotate.conf", NULL};
        int rc2 = logrotate_run_with_ops(3, argv2, &g_mock_ops);
        assert(rc2 == 0);
        assert(strstr(g_mock.stderr_buf, "locked by /run/logrotate.lock (pid 9999); skipping") != NULL);
        assert(mock_has_file(LOGROTATE_LOCK_DIR));
        assert(mock_has_file(LOGROTATE_LOCK_PID));
    }

    printf("PASS: test_atomic_mkdir_lock_collision_and_skip_if_locked\n");
}

static void test_writer_interleaving_residual_risk(void) {
    printf("[TEST] test_writer_interleaving_residual_risk...\n");
    mock_reset();

    const char *cfg =
        "/var/log/messages {\n"
        "    size 10M\n"
        "    rotate 4\n"
        "    compress\n"
        "}\n";
    mock_add_file("/etc/logrotate.conf", 0, S_IFREG | 0644, cfg);
    mock_add_file("/var/log/messages", 11ULL * 1024ULL * 1024ULL, S_IFREG | 0640, "initial log data\n");

    /* Scenario A: Writer opens file, write is paused across 1 rotation */
    int writer_fd = mock_ops_open("/var/log/messages", VFS_O_WRONLY | VFS_O_APPEND, 0640);
    assert(writer_fd > 0);

    /* Logrotate runs cycle 1: messages -> messages.1 (uncompressed), active recreated */
    char *argv[] = {"/sbin/logrotate", "/etc/logrotate.conf", NULL};
    int rc1 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(rc1 == 0);
    assert(mock_has_file("/var/log/messages.1"));
    assert(!mock_has_file("/var/log/messages.1.gz"));

    /* Writer resumes and writes through its open descriptor */
    const char *delayed_record = "delayed writer record\n";
    int64_t wn = mock_ops_write(writer_fd, delayed_record, strlen(delayed_record));
    assert(wn == (int64_t)strlen(delayed_record));
    mock_ops_close(writer_fd);

    /* Because messages.1 was NOT compressed during this cycle, the write succeeded into messages.1 */
    mock_file_t *f1 = mock_find_file("/var/log/messages.1");
    assert(f1 != NULL);
    assert(strstr(f1->content, "delayed writer record") != NULL);

    /* Scenario B: Residual Risk Assertion for writer paused across >= 2 rotations */
    int delayed_fd = mock_ops_open("/var/log/messages", VFS_O_WRONLY | VFS_O_APPEND, 0640);
    assert(delayed_fd > 0);
    mock_ops_write(delayed_fd, "pre-rotation write\n", 19);

    /* Rotation 1: shifts to .1 */
    mock_find_file("/var/log/messages")->size = 11ULL * 1024ULL * 1024ULL;
    int r1 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(r1 == 0);

    /* Rotation 2: shifts .1 -> .2 and compresses to .2.gz */
    mock_find_file("/var/log/messages")->size = 11ULL * 1024ULL * 1024ULL;
    int r2 = logrotate_run_with_ops(2, argv, &g_mock_ops);
    assert(r2 == 0);
    assert(mock_has_file("/var/log/messages.2.gz"));
    assert(!mock_has_file("/var/log/messages.2")); /* unlinked after compression */

    /* Now delayed writer wakes up after 2 full cycles */
    mock_ops_write(delayed_fd, "late write after 2 cycles\n", 26);
    mock_ops_close(delayed_fd);

    /* The compressed archive exists intact */
    assert(mock_has_file("/var/log/messages.2.gz"));

    printf("PASS: test_writer_interleaving_residual_risk\n");
}

static void test_reserved_path_rejection(void) {
    printf("[TEST] test_reserved_path_rejection...\n");

    /* Subtest A: Reject /var/log/watchtower/ */
    {
        mock_reset();
        const char *cfg =
            "/var/log/watchtower/watchtower.log {\n"
            "    size 10M\n"
            "    rotate 4\n"
            "}\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, cfg);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "reserved for watchtower internal rotation") != NULL);
    }

    /* Subtest B: Reject /run/watchtower/ */
    {
        mock_reset();
        const char *cfg =
            "/run/watchtower/dhcpc.log {\n"
            "    size 10M\n"
            "    rotate 4\n"
            "}\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, cfg);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "reserved for watchtower internal rotation") != NULL);
    }

    /* Subtest C: Reject /var/log/boot.log */
    {
        mock_reset();
        const char *cfg =
            "/var/log/boot.log {\n"
            "    size 10M\n"
            "    rotate 4\n"
            "}\n";
        mock_add_file("/etc/bad.conf", 0, S_IFREG | 0644, cfg);
        char *argv[] = {"/sbin/logrotate", "/etc/bad.conf", NULL};
        int rc = logrotate_run_with_ops(2, argv, &g_mock_ops);
        assert(rc != 0);
        assert(strstr(g_mock.stderr_buf, "reserved for boot-log retention") != NULL);
    }

    printf("PASS: test_reserved_path_rejection\n");
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
    test_delaycompress_lifecycle();
    test_boundary_rotate_0_1_and_uncompressed();
    test_atomic_mkdir_lock_collision_and_skip_if_locked();
    test_writer_interleaving_residual_risk();
    test_reserved_path_rejection();

    printf("\nALL 14 LOGROTATE HOST TESTS PASSED!\n");
    return 0;
}

