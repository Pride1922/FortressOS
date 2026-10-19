#include "crond.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "vfs.h"

static bool crond_isblank(char c) {
    return (c == ' ' || c == '\t' || c == '\r');
}

static bool crond_isdigit(char c) {
    return (c >= '0' && c <= '9');
}

static size_t crond_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool crond_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void crond_strlcpy(char *dst, const char *src, size_t cap) {
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

static void crond_append(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = crond_strlen(dst);
    if (len >= cap) return;
    size_t i = 0;
    if (src) {
        while (src[i] && len + 1 < cap) {
            dst[len++] = src[i++];
        }
    }
    dst[len] = '\0';
}

static void crond_append_u64(char *dst, uint64_t val, size_t cap) {
    char tmp[32];
    int idx = 0;
    if (val == 0) {
        tmp[idx++] = '0';
    } else {
        while (val > 0) {
            tmp[idx++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    char rev[32];
    for (int i = 0; i < idx; i++) {
        rev[i] = tmp[idx - 1 - i];
    }
    rev[idx] = '\0';
    crond_append(dst, rev, cap);
}

static void crond_append_i64(char *dst, int64_t val, size_t cap) {
    if (val < 0) {
        crond_append(dst, "-", cap);
        crond_append_u64(dst, (uint64_t)(-val), cap);
    } else {
        crond_append_u64(dst, (uint64_t)val, cap);
    }
}

static void crond_format_err(char *err_msg, size_t err_cap, const char *msg, uint32_t line_num) {
    if (!err_msg || err_cap == 0) return;
    err_msg[0] = '\0';
    crond_append(err_msg, "crond: error at line ", err_cap);
    crond_append_u64(err_msg, line_num, err_cap);
    crond_append(err_msg, ": ", err_cap);
    crond_append(err_msg, msg, err_cap);
}

static void crond_format_spawn_log(char *buf, size_t cap, const char *path, int64_t pid) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    crond_append(buf, "crond: spawned ", cap);
    crond_append(buf, path, cap);
    crond_append(buf, " (pid ", cap);
    crond_append_i64(buf, pid, cap);
    crond_append(buf, ")\n", cap);
}

static void crond_format_fail_log(char *buf, size_t cap, const char *path) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    crond_append(buf, "crond: failed to spawn ", cap);
    crond_append(buf, path, cap);
    crond_append(buf, "\n", cap);
}

void crond_table_init(crond_table_t *table) {
    if (!table) return;
    for (size_t i = 0; i < sizeof(crond_table_t); i++) {
        ((uint8_t *)table)[i] = 0;
    }
    table->task_count = 0;
}

int crond_parse_config(const char *buf, size_t len, crond_table_t *table,
                       char *err_msg, size_t err_cap) {
    if (!buf || !table) return -1;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    crond_table_init(table);

    size_t pos = 0;
    uint32_t line_num = 1;

    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && buf[pos] != '\n') {
            pos++;
        }
        size_t line_end = pos;
        if (pos < len && buf[pos] == '\n') {
            pos++;
        }

        /* Skip leading whitespace */
        size_t i = line_start;
        while (i < line_end && crond_isblank(buf[i])) {
            i++;
        }

        /* Empty line or comment */
        if (i >= line_end || buf[i] == '#') {
            line_num++;
            continue;
        }

        /* Check maximum task capacity */
        if (table->task_count >= CROND_MAX_TASKS) {
            crond_format_err(err_msg, err_cap, "task count exceeds maximum limit (16)", line_num);
            return -1;
        }

        crond_task_t *task = &table->tasks[table->task_count];
        for (size_t k = 0; k < sizeof(crond_task_t); k++) {
            ((uint8_t *)task)[k] = 0;
        }

        /* 1. Parse interval_minutes */
        if (!crond_isdigit(buf[i])) {
            crond_format_err(err_msg, err_cap, "invalid interval (must start with digit)", line_num);
            return -1;
        }

        uint64_t interval = 0;
        while (i < line_end && crond_isdigit(buf[i])) {
            uint32_t d = (uint32_t)(buf[i] - '0');
            interval = interval * 10 + d;
            if (interval > 0xFFFFFFFFULL) {
                crond_format_err(err_msg, err_cap, "interval exceeds 32-bit integer limit", line_num);
                return -1;
            }
            i++;
        }

        if (interval == 0) {
            crond_format_err(err_msg, err_cap, "invalid interval (must be > 0)", line_num);
            return -1;
        }

        if (i < line_end && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r') {
            crond_format_err(err_msg, err_cap, "invalid interval format (non-digit characters)", line_num);
            return -1;
        }

        task->interval_minutes = (uint32_t)interval;

        /* Skip whitespace after interval */
        while (i < line_end && crond_isblank(buf[i])) {
            i++;
        }

        if (i >= line_end) {
            crond_format_err(err_msg, err_cap, "missing executable command path", line_num);
            return -1;
        }

        /* 2. Parse absolute_command_path */
        char quote = 0;
        if (buf[i] == '"' || buf[i] == '\'') {
            quote = buf[i++];
        }

        if (i >= line_end || buf[i] != '/') {
            crond_format_err(err_msg, err_cap, "command path must be absolute (must start with '/')", line_num);
            return -1;
        }

        size_t p = 0;
        while (i < line_end) {
            if (quote) {
                if (buf[i] == quote) {
                    i++;
                    break;
                }
            } else {
                if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r') {
                    break;
                }
            }
            if (p + 1 >= CROND_MAX_PATH_LEN) {
                crond_format_err(err_msg, err_cap, "command path exceeds maximum length (128)", line_num);
                return -1;
            }
            task->exec_path[p++] = buf[i++];
        }
        task->exec_path[p] = '\0';

        /* 3. Parse optional arguments [args...] */
        task->argc = 0;
        while (i < line_end) {
            while (i < line_end && crond_isblank(buf[i])) {
                i++;
            }
            if (i >= line_end) break;

            if (task->argc >= CROND_MAX_ARGS) {
                crond_format_err(err_msg, err_cap, "argument count exceeds maximum limit (16)", line_num);
                return -1;
            }

            quote = 0;
            if (buf[i] == '"' || buf[i] == '\'') {
                quote = buf[i++];
            }

            size_t a = 0;
            while (i < line_end) {
                if (quote) {
                    if (buf[i] == quote) {
                        i++;
                        break;
                    }
                } else {
                    if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r') {
                        break;
                    }
                }
                if (a + 1 >= CROND_MAX_ARG_LEN) {
                    crond_format_err(err_msg, err_cap, "argument exceeds maximum length (128)", line_num);
                    return -1;
                }
                task->args[task->argc][a++] = buf[i++];
            }
            task->args[task->argc][a] = '\0';
            task->argc++;
        }

        task->last_run_minute = 0;
        table->task_count++;
        line_num++;
    }

    return 0;
}

int crond_load_config(const char *primary_path, const char *fallback_path,
                      crond_table_t *table, char *err_msg, size_t err_cap,
                      const crond_ops_t *ops) {
    if (!table || !ops || !ops->read_file) {
        if (err_msg && err_cap > 0) {
            crond_strlcpy(err_msg, "crond: invalid parameters to load_config", err_cap);
        }
        return -1;
    }

    static char s_crontab_buf[CROND_CONFIG_MAX_SIZE];
    size_t file_len = 0;

    if (primary_path && ops->read_file(primary_path, s_crontab_buf, sizeof(s_crontab_buf), &file_len) == 0) {
        /* Primary successfully read */
    } else if (fallback_path && ops->read_file(fallback_path, s_crontab_buf, sizeof(s_crontab_buf), &file_len) == 0) {
        /* Fallback successfully read */
    } else {
        /* Both missing: handle gracefully per requirements */
        const char *create_path = primary_path ? primary_path : CROND_CONFIG_PRIMARY;
        static const char default_crontab[] =
            "# /etc/crontab - FortressOS crond configuration\n"
            "# <interval_minutes> <absolute_command_path> [args...]\n"
            "60 /sbin/logrotate --skip-if-locked /etc/logrotate.conf\n";
        size_t dlen = sizeof(default_crontab) - 1;

        if (ops->write_file && ops->write_file(create_path, default_crontab, dlen) == 0) {
            if (ops->log_err) {
                ops->log_err("crond: created default /etc/crontab\n");
            }
            return crond_parse_config(default_crontab, dlen, table, err_msg, err_cap);
        } else {
            /* Root is read-only or write failed */
            if (ops->log_err) {
                ops->log_err("crond: no crontab found; running idle\n");
            }
            crond_table_init(table);
            return 0;
        }
    }

    int ret = crond_parse_config(s_crontab_buf, file_len, table, err_msg, err_cap);
    if (ret != 0) {
        if (ops->log_err) {
            char lbuf[256];
            lbuf[0] = '\0';
            crond_append(lbuf, "crond: invalid config: ", sizeof(lbuf));
            if (err_msg && err_msg[0]) {
                crond_append(lbuf, err_msg, sizeof(lbuf));
            }
            crond_append(lbuf, "\n", sizeof(lbuf));
            ops->log_err(lbuf);
        }
        return -1;
    }

    return 0;
}

bool crond_task_should_run(const crond_task_t *task, uint64_t uptime_minutes) {
    if (!task || task->interval_minutes == 0) return false;
    if (uptime_minutes == 0) return false;
    return (uptime_minutes % task->interval_minutes) == 0;
}

int crond_tick(crond_table_t *table, uint64_t uptime_minutes, const crond_ops_t *ops) {
    if (!table || !ops) return -1;

    /* Reap dead child processes non-blockingly to avoid zombie buildup */
    if (ops->reap_zombies) {
        ops->reap_zombies();
    }

    int spawned_count = 0;
    for (size_t i = 0; i < table->task_count; i++) {
        crond_task_t *task = &table->tasks[i];
        if (crond_task_should_run(task, uptime_minutes)) {
            if (task->last_run_minute != uptime_minutes) {
                task->last_run_minute = uptime_minutes;

                const char *argv[CROND_MAX_ARGS + 2];
                argv[0] = task->exec_path;
                int a = 0;
                for (; a < task->argc && a < CROND_MAX_ARGS; a++) {
                    argv[a + 1] = task->args[a];
                }
                argv[a + 1] = NULL;

                if (ops->spawn) {
                    int64_t pid = ops->spawn(task->exec_path, argv);
                    if (pid > 0) {
                        char msg[256];
                        crond_format_spawn_log(msg, sizeof(msg), task->exec_path, pid);
                        if (ops->log_err) ops->log_err(msg);
                        spawned_count++;
                    } else {
                        char msg[256];
                        crond_format_fail_log(msg, sizeof(msg), task->exec_path);
                        if (ops->log_err) ops->log_err(msg);
                    }
                }
            }
        }
    }

    return spawned_count;
}

int crond_run_loop(crond_table_t *table, const crond_ops_t *ops, volatile int *stop_flag) {
    if (!table || !ops || !stop_flag) return -1;

    while (!(*stop_flag)) {
        if (ops->sleep_sec) {
            ops->sleep_sec(CROND_TICK_SECONDS);
        }

        if (*stop_flag) break;

        uint64_t uptime_sec = ops->get_uptime_sec ? ops->get_uptime_sec() : 0;
        uint64_t uptime_min = uptime_sec / 60;

        crond_tick(table, uptime_min, ops);
    }

    if (ops->log_err) {
        ops->log_err("crond: stopping daemon loop\n");
    }

    return 0;
}

int crond_run_with_ops(int argc, char **argv, const crond_ops_t *ops, volatile int *stop_flag) {
    if (!ops || !stop_flag) return 1;

    bool foreground = false;
    for (int i = 1; i < argc; i++) {
        if (crond_streq(argv[i], "-f") || crond_streq(argv[i], "--foreground")) {
            foreground = true;
        } else if (crond_streq(argv[i], "-h") || crond_streq(argv[i], "--help")) {
            if (ops->log_err) {
                ops->log_err("usage: crond [-f|--foreground] [-h|--help]\n");
            }
            return 0;
        }
    }

    if (!foreground) {
        /* Run in background via spawn -f */
        const char *cron_argv[] = {"/sbin/crond", "-f", NULL};
        if (ops->spawn) {
            int64_t pid = ops->spawn("/sbin/crond", cron_argv);
            if (pid > 0) {
                return 0;
            }
        }
        /* Fall back to foreground if background spawn is unavailable */
    }

    static crond_table_t table;
    char err[256];
    err[0] = '\0';
    int rc = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &table, err, sizeof(err), ops);
    if (rc != 0) {
        /* Config error: fail closed */
        return 1;
    }

    if (ops->log_err) {
        ops->log_err("crond: started task scheduler daemon\n");
    }

    return crond_run_loop(&table, ops, stop_flag);
}

#ifndef CROND_HOST_TEST

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

static int64_t crond_prod_spawn(const char *path, const char *const argv[]) {
    if (!path || !argv) return -1;
    spawn_opts_t opts;
    for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
    opts.size = sizeof(opts);
    opts.version = 2;
    opts.flags = SPAWN_SETPGROUP;
    opts.argv = (uint64_t)argv;
    long pid = sys_call(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts), 0, 0, 0);
    return (int64_t)pid;
}

static uint64_t crond_prod_get_uptime_sec(void) {
    timespec_t ts = {0, 0};
    long ret = sys_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts, 0, 0, 0, 0);
    if (ret != 0) return 0;
    return (uint64_t)ts.tv_sec;
}

static void crond_prod_sleep_sec(uint32_t sec) {
    timespec_t req;
    req.tv_sec = (int64_t)sec;
    req.tv_nsec = 0;
    timespec_t rem;
    (void)sys_call(SYS_NANOSLEEP, (uintptr_t)&req, (uintptr_t)&rem, 0, 0, 0, 0);
}

static void crond_prod_log_err(const char *msg) {
    if (!msg) return;
    size_t len = crond_strlen(msg);
    if (len > 0) {
        (void)sys_call(SYS_WRITE, 2, (uintptr_t)msg, len, 0, 0, 0);
    }
}

static int crond_prod_read_file(const char *path, char *buf, size_t buf_cap, size_t *out_len) {
    if (!path || !buf || buf_cap == 0 || !out_len) return -1;
    long fd = sys_call(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    long n = sys_call(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, buf_cap - 1, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (n < 0) return -1;
    buf[n] = '\0';
    *out_len = (size_t)n;
    return 0;
}

static int crond_prod_write_file(const char *path, const char *buf, size_t len) {
    if (!path || !buf) return -1;
    long fd = sys_call(SYS_OPEN, (uintptr_t)path,
                       VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) return -1;
    long n = sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, len, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (n < (long)len) return -1;
    return 0;
}

static void crond_prod_reap_zombies(void) {
    for (;;) {
        uint64_t status = 0;
        long pid = sys_call(SYS_WAITPID, (uintptr_t)-1, (uintptr_t)&status, WNOHANG, 0, 0, 0);
        if (pid <= 0) break;
    }
}

static volatile int g_prod_crond_stop = 0;

static void crond_sig_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_prod_crond_stop = 1;
    }
}

static void crond_setup_signals(void) {
    signal_action_t sa = {
        .handler = (uintptr_t)crond_sig_handler,
        .mask = 0,
        .flags = 0,
        .reserved = 0
    };
    sys_call(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    sys_call(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);

    signal_action_t sa_ign = {
        .handler = SIG_IGN,
        .mask = 0,
        .flags = 0,
        .reserved = 0
    };
    sys_call(SYS_SIGACTION, SIGPIPE, (uintptr_t)&sa_ign, 0, 0, 0, 0);
}

int crond_main(int argc, char **argv) {
    crond_setup_signals();

    static crond_ops_t ops;
    ops.spawn = crond_prod_spawn;
    ops.get_uptime_sec = crond_prod_get_uptime_sec;
    ops.sleep_sec = crond_prod_sleep_sec;
    ops.log_err = crond_prod_log_err;
    ops.read_file = crond_prod_read_file;
    ops.write_file = crond_prod_write_file;
    ops.reap_zombies = crond_prod_reap_zombies;

    return crond_run_with_ops(argc, argv, &ops, &g_prod_crond_stop);
}

#endif /* !CROND_HOST_TEST */
