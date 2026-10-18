#ifndef USER_LOGROTATE_H
#define USER_LOGROTATE_H

#include "types.h"

#define LOGROTATE_MAX_ENTRIES       10
#define LOGROTATE_MAX_ROTATE        10
#define LOGROTATE_MAX_PATH          256
#define LOGROTATE_CONFIG_MAX_SIZE   4096
#define LOGROTATE_GZIP_TIMEOUT_MS   5000

#define LOGROTATE_CONFIG_PRIMARY    "/etc/logrotate.conf"
#define LOGROTATE_CONFIG_FALLBACK   "/mnt/.fortress/logrotate.conf"
#define LOGROTATE_LOCK_DIR          "/run/logrotate.lock"
#define LOGROTATE_LOCK_PID          "/run/logrotate.lock/pid"

#ifndef S_IFMT
#define S_IFMT   0170000u
#endif
#ifndef S_IFREG
#define S_IFREG  0100000u
#endif
#ifndef S_IFDIR
#define S_IFDIR  0040000u
#endif
#ifndef S_IFLNK
#define S_IFLNK  0120000u
#endif

typedef struct {
    char path[LOGROTATE_MAX_PATH];
    uint64_t size_limit;
    uint32_t rotate;
    bool compress;
    bool missingok;
    bool size_specified;
} logrotate_entry_t;

typedef struct {
    logrotate_entry_t entries[LOGROTATE_MAX_ENTRIES];
    size_t count;
} logrotate_config_t;

typedef struct {
    uint64_t size;
    uint32_t mode;
} logrotate_stat_t;

typedef struct {
    int (*lstat)(const char *path, logrotate_stat_t *st);
    int (*stat)(const char *path, logrotate_stat_t *st);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*unlink)(const char *path);
    int (*open)(const char *path, int flags, uint32_t mode);
    int (*close)(int fd);
    int64_t (*read)(int fd, void *buf, size_t count);
    int64_t (*write)(int fd, const void *buf, size_t count);
    int64_t (*spawn)(const char *path, const char *const argv[]);
    int64_t (*waitpid)(int64_t pid, uint64_t *status, int options);
    void (*sleep_ms)(uint32_t ms);
    void (*log_err)(const char *msg);
    int (*mkdir)(const char *path, uint32_t mode);
    int64_t (*getpid)(void);
    int (*kill)(int64_t pid, int sig);
} logrotate_ops_t;

void logrotate_config_init(logrotate_config_t *cfg);

int logrotate_parse_config(const char *buf, size_t len, logrotate_config_t *cfg,
                          char *err_msg, size_t err_cap);

int logrotate_load_config(const char *path, logrotate_config_t *cfg,
                         char *err_msg, size_t err_cap,
                         const logrotate_ops_t *ops);

int logrotate_rotate_entry(const logrotate_entry_t *entry, bool force,
                          const logrotate_ops_t *ops);

int logrotate_run_with_ops(int argc, char **argv, const logrotate_ops_t *ops);

int logrotate_main(int argc, char **argv);

#endif /* USER_LOGROTATE_H */
