#ifndef USER_CROND_H
#define USER_CROND_H

#include "types.h"

#define CROND_MAX_TASKS        16
#define CROND_MAX_ARGS         16
#define CROND_MAX_ARG_LEN      128
#define CROND_MAX_PATH_LEN     128
#define CROND_MAX_LINE_LEN     512
#define CROND_CONFIG_MAX_SIZE  4096

#define CROND_CONFIG_PRIMARY   "/etc/crontab"
#define CROND_CONFIG_FALLBACK  "/mnt/.fortress/crontab"

#define CROND_TICK_SECONDS     60

typedef struct {
    uint32_t interval_minutes;
    char exec_path[CROND_MAX_PATH_LEN];
    char args[CROND_MAX_ARGS][CROND_MAX_ARG_LEN];
    int argc;
    uint64_t last_run_minute;
} crond_task_t;

typedef struct {
    crond_task_t tasks[CROND_MAX_TASKS];
    size_t task_count;
} crond_table_t;

typedef struct {
    int64_t (*spawn)(const char *path, const char *const argv[]);
    uint64_t (*get_uptime_sec)(void);
    void (*sleep_sec)(uint32_t sec);
    void (*log_err)(const char *msg);
    int (*read_file)(const char *path, char *buf, size_t buf_cap, size_t *out_len);
    void (*reap_zombies)(void);
} crond_ops_t;

void crond_table_init(crond_table_t *table);

int crond_parse_config(const char *buf, size_t len, crond_table_t *table,
                       char *err_msg, size_t err_cap);

int crond_load_config(const char *primary_path, const char *fallback_path,
                      crond_table_t *table, char *err_msg, size_t err_cap,
                      const crond_ops_t *ops);

bool crond_task_should_run(const crond_task_t *task, uint64_t uptime_minutes);

int crond_tick(crond_table_t *table, uint64_t uptime_minutes, const crond_ops_t *ops);

int crond_run_loop(crond_table_t *table, const crond_ops_t *ops, volatile int *stop_flag);

int crond_main(int argc, char **argv);

#endif /* USER_CROND_H */
