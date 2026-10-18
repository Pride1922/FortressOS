#ifndef USER_WATCHTOWER_H
#define USER_WATCHTOWER_H

#include "types.h"

#define WATCHTOWER_MAX_SERVICES       16
#define WATCHTOWER_MAX_NAME_LEN       32
#define WATCHTOWER_MAX_ARGS           16
#define WATCHTOWER_MAX_ARG_LEN        128
#define WATCHTOWER_MAX_PATH_LEN       128
#define WATCHTOWER_MAX_ID_LEN         32

#define WATCHTOWER_RUN_DIR            "/run/watchtower"
#define WATCHTOWER_PID_FILE           "/run/watchtower/watchtower.pid"
#define WATCHTOWER_INSTALLED_LOG_DIR  "/var/log/watchtower"
#define WATCHTOWER_LIVE_LOG_DIR       "/run/watchtower"

#define WATCHTOWER_MAX_ENV            16
#define WATCHTOWER_MAX_ENV_LEN        128

typedef enum {
    SVC_TYPE_SIMPLE = 0,
    SVC_TYPE_FORKING = 1
} service_type_t;

typedef enum {
    SVC_STATE_STOPPED = 0,
    SVC_STATE_STARTING = 1,
    SVC_STATE_RUNNING = 2,
    SVC_STATE_STOPPING = 3,
    SVC_STATE_FAILED = 4
} service_state_t;

typedef enum {
    RESTART_NEVER = 0,
    RESTART_ON_FAILURE = 1,
    RESTART_ALWAYS = 2
} restart_policy_t;

typedef enum {
    READINESS_NONE = 0,
    READINESS_FILE = 1,
    READINESS_COMMAND = 2
} readiness_mode_t;

typedef struct {
    char name[WATCHTOWER_MAX_NAME_LEN];
    char exec_path[WATCHTOWER_MAX_PATH_LEN];
    char args[WATCHTOWER_MAX_ARGS][WATCHTOWER_MAX_ARG_LEN];
    int argc;
    service_type_t type;          /* SVC_TYPE_SIMPLE (default) or SVC_TYPE_FORKING */
    char working_dir[WATCHTOWER_MAX_PATH_LEN];
    char pid_file[WATCHTOWER_MAX_PATH_LEN];
    char env_file[WATCHTOWER_MAX_PATH_LEN];
    char env[WATCHTOWER_MAX_ENV][WATCHTOWER_MAX_ENV_LEN];
    int envc;
    int order;                    /* Startup order: e.g. 1..999 (default: 100) */
    bool enabled;                 /* Autostart at boot */
    char user[WATCHTOWER_MAX_ID_LEN];
    char group[WATCHTOWER_MAX_ID_LEN];
    restart_policy_t restart_policy;
    uint32_t restart_limit;       /* Max restarts within 60s window (default: 3) */
    uint32_t restart_delay_sec;   /* Seconds to wait before restart (default: 5) */
    uint32_t timeout_start_sec;   /* Startup/readiness timeout (default: 5) */
    uint32_t timeout_stop_sec;    /* Shutdown timeout before SIGKILL (default: 5) */
    readiness_mode_t readiness_mode;
    char ready_path[WATCHTOWER_MAX_PATH_LEN];

    /* Dynamic runtime tracking */
    service_state_t state;
    int64_t pid;                  /* 0 if not running (or daemon PID for Type=forking) */
    int64_t initial_pid;          /* Initial launcher PID for Type=forking */
    uint64_t start_time_sec;      /* Monotonic seconds at start */
    uint64_t exit_time_sec;       /* Monotonic seconds at exit / stop request */
    int last_exit_code;
    int last_term_sig;
    uint32_t restart_count;
    uint64_t restart_window_start_sec;
    uint64_t next_restart_sec;    /* Monotonic deadline if pending restart */
    bool ready_verified;
    bool manual_stop;
    uint64_t dropped_bytes;
    int pipe_fd;                  /* Non-blocking stdout/stderr pipe fd (or -1) */
} service_t;

typedef struct {
    service_t services[WATCHTOWER_MAX_SERVICES];
    size_t count;
    bool stopping_all;
    uint64_t shutdown_start_sec;
} watchtower_table_t;

/* System operations abstraction for testing and kernel syscalls */
typedef struct {
    int64_t (*spawn)(const char *path, const char *const argv[]);
    int64_t (*spawn_with_pipe)(const char *path, const char *const argv[],
                               const char *const envp[], const char *cwd,
                               int *out_pipe_fd);
    int (*kill)(int64_t pid, int sig);
    bool (*file_exists)(const char *path);
    uint64_t (*monotonic_sec)(void);
    bool (*is_alive)(int64_t pid);
    int64_t (*read_pid_file)(const char *path);
} watchtower_sys_ops_t;

/* Core Service Table APIs */
void watchtower_table_init(watchtower_table_t *tbl);
int watchtower_table_add_service(watchtower_table_t *tbl, const service_t *svc);
void watchtower_table_sort_by_order(watchtower_table_t *tbl);
service_t *watchtower_find_service(watchtower_table_t *tbl, const char *name);
service_t *watchtower_find_service_by_pid(watchtower_table_t *tbl, int64_t pid);

/* Service Lifecycle State Transitions */
int watchtower_start_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops);
int watchtower_stop_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops);
int watchtower_restart_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops);
void watchtower_handle_child_exit(watchtower_table_t *tbl, int64_t pid, int status, uint64_t now_sec);

/* Periodic Tick: checks readiness, stopping timeouts, pending restarts */
void watchtower_tick(watchtower_table_t *tbl, uint64_t now_sec, const watchtower_sys_ops_t *ops);

/* Shutdown: stops all services in reverse Order */
int watchtower_stop_all(watchtower_table_t *tbl, uint64_t now_sec, const watchtower_sys_ops_t *ops);
bool watchtower_all_stopped(const watchtower_table_t *tbl);

const char *watchtower_state_name(service_state_t state);

#endif /* USER_WATCHTOWER_H */
