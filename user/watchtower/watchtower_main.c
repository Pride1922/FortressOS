#include "types.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "vfs.h"
#include "watchtower.h"
#include "watchtower_config.h"
#include "watchtower_ctl.h"

#define WT_LOG_MAX_SIZE (1024 * 1024) /* 1 MiB */

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

static size_t wt_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void wt_strlcpy(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void wt_strlcat(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    size_t dlen = wt_strlen(dst);
    if (dlen >= dst_size) return;
    size_t i = 0;
    while (src && src[i] && dlen + i + 1 < dst_size) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static void wt_print(const char *s) {
    size_t len = wt_strlen(s);
    if (len > 0) {
        (void)sys_call(SYS_WRITE, 1, (uintptr_t)s, len, 0, 0, 0);
    }
}

static bool wt_file_exists(const char *path) {
    if (!path || !path[0]) return false;
    long fd = sys_call(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0, 0, 0, 0);
    if (fd >= 0) {
        (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return true;
    }
    return false;
}

static int64_t wt_spawn(const char *path, const char *const argv[]) {
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

static int64_t wt_spawn_with_pipe(const char *path, const char *const argv[],
                                  const char *const envp[], const char *cwd,
                                  int *out_pipe_fd) {
    if (!path || !argv || !out_pipe_fd) return -1;
    *out_pipe_fd = -1;

    int pipe_fds[2] = {-1, -1};
    long ret = sys_call(SYS_PIPE, (uintptr_t)pipe_fds, 0, 0, 0, 0, 0);
    if (ret != 0) {
        /* Fall back to regular spawn without pipe if pipe creation fails */
        return wt_spawn(path, argv);
    }

    /* Set read end non-blocking so supervisor never stalls */
    sys_call(SYS_FCNTL, (uintptr_t)pipe_fds[0], F_SETFL, VFS_O_NONBLOCK, 0, 0, 0);

    spawn_fd_action_t actions[4];
    for (size_t i = 0; i < sizeof(actions); i++) ((uint8_t *)actions)[i] = 0;
    size_t act_count = 0;

    /* Action 0: stdout -> pipe_fds[1] */
    actions[act_count].type = SPAWN_FD_ACTION_DUP2;
    actions[act_count].src_fd = pipe_fds[1];
    actions[act_count].dst_fd = 1;
    act_count++;

    /* Action 1: stderr -> pipe_fds[1] */
    actions[act_count].type = SPAWN_FD_ACTION_DUP2;
    actions[act_count].src_fd = pipe_fds[1];
    actions[act_count].dst_fd = 2;
    act_count++;

    /* Action 2: close read end in child */
    actions[act_count].type = SPAWN_FD_ACTION_CLOSE;
    actions[act_count].dst_fd = pipe_fds[0];
    act_count++;

    /* Action 3: if pipe_fds[1] > 2, close extra write fd in child */
    if (pipe_fds[1] > 2) {
        actions[act_count].type = SPAWN_FD_ACTION_CLOSE;
        actions[act_count].dst_fd = pipe_fds[1];
        act_count++;
    }

    spawn_opts_t opts;
    for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
    opts.size = sizeof(opts);
    opts.version = 2;
    opts.flags = SPAWN_SETPGROUP;
    opts.argv = (uint64_t)argv;
    opts.envp = (uint64_t)envp;
    if (cwd && cwd[0]) opts.cwd = (uint64_t)cwd;
    opts.fd_actions = (uint64_t)actions;
    opts.action_count = (uint32_t)act_count;

    long pid = sys_call(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts), 0, 0, 0);

    /* Close write end in parent */
    sys_call(SYS_CLOSE, (uintptr_t)pipe_fds[1], 0, 0, 0, 0, 0);

    if (pid <= 0) {
        sys_call(SYS_CLOSE, (uintptr_t)pipe_fds[0], 0, 0, 0, 0, 0);
        return -1;
    }

    *out_pipe_fd = pipe_fds[0];
    return (int64_t)pid;
}

static bool wt_is_alive(int64_t pid) {
    if (pid <= 0) return false;
    return sys_call(SYS_KILL, (uintptr_t)pid, 0, 0, 0, 0, 0) == 0;
}

static int64_t wt_read_pid_file(const char *path) {
    if (!path || !path[0]) return -1;
    long fd = sys_call(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    char buf[32];
    long n = sys_call(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';
    int64_t val = 0;
    int i = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i >= n || buf[i] < '0' || buf[i] > '9') return -1;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        val = val * 10 + (buf[i] - '0');
        i++;
    }
    return (val > 0) ? val : -1;
}

static int wt_kill(int64_t pid, int sig) {
    if (pid <= 0) return -1;
    /* First attempt to signal the entire process group (-pid) */
    long ret = sys_call(SYS_KILL, (uintptr_t)(-pid), (uintptr_t)sig, 0, 0, 0, 0);
    if (ret != 0) {
        ret = sys_call(SYS_KILL, (uintptr_t)pid, (uintptr_t)sig, 0, 0, 0, 0);
    }
    return (int)ret;
}

static uint64_t wt_monotonic_sec(void) {
    timespec_t ts = {0, 0};
    long ret = sys_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts, 0, 0, 0, 0);
    if (ret != 0) return 0;
    return (uint64_t)ts.tv_sec;
}

static void wt_sleep_ms(uint32_t ms) {
    timespec_t ts;
    ts.tv_sec = (int64_t)(ms / 1000);
    ts.tv_nsec = (int64_t)((ms % 1000) * 1000000ULL);
    (void)sys_call(SYS_NANOSLEEP, (uintptr_t)&ts, 0, 0, 0, 0, 0);
}

static volatile int g_term_requested = 0;
static volatile int g_hup_requested = 0;

static void wt_signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_term_requested = 1;
    } else if (sig == SIGHUP) {
        g_hup_requested = 1;
    }
}

static void wt_setup_signals(void) {
    signal_action_t sa = {
        .handler = (uintptr_t)wt_signal_handler,
        .mask = 0,
        .flags = 0,
        .reserved = 0
    };
    sys_call(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    sys_call(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);
    sys_call(SYS_SIGACTION, SIGHUP, (uintptr_t)&sa, 0, 0, 0, 0);

    /* Ignore SIGPIPE */
    signal_action_t sa_ign = {
        .handler = SIG_IGN,
        .mask = 0,
        .flags = 0,
        .reserved = 0
    };
    sys_call(SYS_SIGACTION, SIGPIPE, (uintptr_t)&sa_ign, 0, 0, 0, 0);
}

static void wt_write_pid_file(void) {
    proc_info_t pi;
    for (size_t i = 0; i < sizeof(pi); i++) ((uint8_t *)&pi)[i] = 0;
    long ret = sys_call(SYS_PROCINFO, PROC_INFO_SELF, (uintptr_t)&pi, 0, 0, 0, 0);
    if (ret < 0) return;

    char buf[32];
    size_t pos = 0;
    uint64_t pid = (uint64_t)pi.pid;
    if (pid == 0) buf[pos++] = '0';
    else {
        char rev[32];
        size_t rlen = 0;
        while (pid > 0) {
            rev[rlen++] = (char)('0' + (pid % 10));
            pid /= 10;
        }
        for (size_t i = 0; i < rlen; i++) buf[pos++] = rev[rlen - 1 - i];
    }
    buf[pos++] = '\n';

    long fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_PID_FILE,
                       VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
    if (fd >= 0) {
        (void)sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, pos, 0, 0, 0);
        (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    }
}

/* Logging and Rotation Subsystem */
static char g_log_dir[64] = "/run/watchtower";

static void wt_init_log_dir(void) {
    (void)sys_call(SYS_MKDIR, (uintptr_t)"/var/log", 0755, 0, 0, 0, 0);
    (void)sys_call(SYS_MKDIR, (uintptr_t)"/var/log/watchtower", 0755, 0, 0, 0, 0);

    const char *probe = "/var/log/watchtower/.probe";
    long fd = sys_call(SYS_OPEN, (uintptr_t)probe,
                       VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
    if (fd >= 0) {
        (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        (void)sys_call(SYS_UNLINK, (uintptr_t)probe, 0, 0, 0, 0, 0);
        wt_strlcpy(g_log_dir, "/var/log/watchtower", sizeof(g_log_dir));
    } else {
        (void)sys_call(SYS_MKDIR, (uintptr_t)"/run/watchtower", 0755, 0, 0, 0, 0);
        wt_strlcpy(g_log_dir, "/run/watchtower", sizeof(g_log_dir));
    }
}

static void wt_rotate_log(const char *log_path) {
    char p1[128], p1_gz[128], p2_gz[128], p3_gz[128];

    p3_gz[0] = '\0';
    wt_strlcpy(p3_gz, log_path, sizeof(p3_gz));
    wt_strlcat(p3_gz, ".3.gz", sizeof(p3_gz));

    p2_gz[0] = '\0';
    wt_strlcpy(p2_gz, log_path, sizeof(p2_gz));
    wt_strlcat(p2_gz, ".2.gz", sizeof(p2_gz));

    p1_gz[0] = '\0';
    wt_strlcpy(p1_gz, log_path, sizeof(p1_gz));
    wt_strlcat(p1_gz, ".1.gz", sizeof(p1_gz));

    p1[0] = '\0';
    wt_strlcpy(p1, log_path, sizeof(p1));
    wt_strlcat(p1, ".1", sizeof(p1));

    /* 1. Unlink oldest: .3.gz */
    (void)sys_call(SYS_UNLINK, (uintptr_t)p3_gz, 0, 0, 0, 0, 0);

    /* 2. Shift .2.gz -> .3.gz */
    (void)sys_call(SYS_RENAME, (uintptr_t)p2_gz, (uintptr_t)p3_gz, 0, 0, 0, 0);

    /* 3. Shift .1.gz -> .2.gz */
    (void)sys_call(SYS_RENAME, (uintptr_t)p1_gz, (uintptr_t)p2_gz, 0, 0, 0, 0);

    /* 4. Shift .log -> .1 */
    (void)sys_call(SYS_RENAME, (uintptr_t)log_path, (uintptr_t)p1, 0, 0, 0, 0);

    /* 5. Spawn /bin/gzip -f <p1> if /bin/gzip exists */
    if (wt_file_exists("/bin/gzip")) {
        const char *argv[] = {"/bin/gzip", "-f", p1, NULL};
        int64_t gpid = wt_spawn("/bin/gzip", argv);
        if (gpid > 0) {
            uint64_t st = 0;
            (void)sys_call(SYS_WAITPID, (uintptr_t)gpid, (uintptr_t)&st, 0, 0, 0, 0);
        }
    }
}

static void wt_append_log(const char *path, const char *data, size_t len, uint64_t *dropped_bytes) {
    if (!path || !data || len == 0) return;

    vfs_stat_t st;
    for (size_t i = 0; i < sizeof(st); i++) ((uint8_t *)&st)[i] = 0;
    long sret = sys_call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0, 0, 0, 0);
    if (sret == 0 && st.size >= WT_LOG_MAX_SIZE) {
        wt_rotate_log(path);
    }

    long fd = sys_call(SYS_OPEN, (uintptr_t)path,
                       VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0644, 0, 0, 0);
    if (fd < 0) {
        if (dropped_bytes) *dropped_bytes += len;
        return;
    }

    long written = sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)data, len, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);

    if (written < (long)len) {
        if (dropped_bytes) {
            size_t dropped = (written >= 0) ? (len - (size_t)written) : len;
            *dropped_bytes += dropped;
        }
    }
}

static void wt_log_event(const char *msg) {
    wt_print(msg);

    char path[128];
    path[0] = '\0';
    wt_strlcpy(path, g_log_dir, sizeof(path));
    wt_strlcat(path, "/watchtower.log", sizeof(path));

    uint64_t sec = wt_monotonic_sec();
    char line[512];
    size_t pos = 0;
    line[pos++] = '[';
    char num[32];
    size_t nlen = 0;
    uint64_t tmp = sec;
    if (tmp == 0) num[nlen++] = '0';
    else {
        while (tmp > 0) {
            num[nlen++] = (char)('0' + (tmp % 10));
            tmp /= 10;
        }
    }
    for (size_t i = 0; i < nlen; i++) line[pos++] = num[nlen - 1 - i];
    line[pos++] = ']';
    line[pos++] = ' ';
    size_t mlen = wt_strlen(msg);
    for (size_t i = 0; i < mlen && pos + 2 < sizeof(line); i++) {
        line[pos++] = msg[i];
    }
    line[pos] = '\0';

    wt_append_log(path, line, pos, NULL);
}

static void wt_drain_pipes(watchtower_table_t *tbl) {
    if (!tbl) return;
    /* Single supervisor loop; signals only set flags. Keep bulk pipe scratch
     * off the one-page Ring 3 stack. No nested drain invocation is allowed. */
    static char chunk[4096];
    char log_path[128];

    for (size_t i = 0; i < tbl->count; i++) {
        service_t *svc = &tbl->services[i];
        if (svc->pipe_fd < 0) continue;

        for (;;) {
            long n = sys_call(SYS_READ, (uintptr_t)svc->pipe_fd,
                              (uintptr_t)chunk, sizeof(chunk), 0, 0, 0);
            if (n > 0) {
                log_path[0] = '\0';
                wt_strlcpy(log_path, g_log_dir, sizeof(log_path));
                wt_strlcat(log_path, "/", sizeof(log_path));
                wt_strlcat(log_path, svc->name, sizeof(log_path));
                wt_strlcat(log_path, ".log", sizeof(log_path));

                wt_append_log(log_path, chunk, (size_t)n, &svc->dropped_bytes);
            } else if (n == 0) {
                /* EOF: child has closed write ends */
                if (svc->pid == 0 || svc->state == SVC_STATE_STOPPED || svc->state == SVC_STATE_FAILED) {
                    (void)sys_call(SYS_CLOSE, (uintptr_t)svc->pipe_fd, 0, 0, 0, 0, 0);
                    svc->pipe_fd = -1;
                }
                break;
            } else {
                /* n < 0: if EAGAIN (-11), no data ready. If other error, close pipe */
                if (n != -11 && n != 11) {
                    (void)sys_call(SYS_CLOSE, (uintptr_t)svc->pipe_fd, 0, 0, 0, 0, 0);
                    svc->pipe_fd = -1;
                }
                break;
            }
        }
    }
}

int watchtower_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Ensure /run/watchtower directory exists and initialize logging directory */
    sys_call(SYS_MKDIR, (uintptr_t)WATCHTOWER_RUN_DIR, 0755, 0, 0, 0, 0);
    wt_init_log_dir();
    wt_write_pid_file();
    wt_setup_signals();

    /* Redirect supervisor's own stderr to watchtower.log to capture unhandled panics */
    char wt_err_log[128];
    wt_err_log[0] = '\0';
    wt_strlcpy(wt_err_log, g_log_dir, sizeof(wt_err_log));
    wt_strlcat(wt_err_log, "/watchtower.log", sizeof(wt_err_log));
    long wt_log_fd = sys_call(SYS_OPEN, (uintptr_t)wt_err_log,
                              VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0644, 0, 0, 0);
    if (wt_log_fd >= 0) {
        (void)sys_call(SYS_DUP2, (uintptr_t)wt_log_fd, 2, 0, 0, 0, 0);
        (void)sys_call(SYS_CLOSE, (uintptr_t)wt_log_fd, 0, 0, 0, 0, 0);
    }

    wt_log_event("[watchtower] Starting service supervisor...\n");

    static const watchtower_sys_ops_t ops = {
        .spawn = wt_spawn,
        .spawn_with_pipe = wt_spawn_with_pipe,
        .kill = wt_kill,
        .file_exists = wt_file_exists,
        .monotonic_sec = wt_monotonic_sec,
        .is_alive = wt_is_alive,
        .read_pid_file = wt_read_pid_file
    };

    static watchtower_table_t table;
    watchtower_table_init(&table);

    /* Register Services: configuration files checked first with built-in fallbacks */
    char err[256];
    /* Startup records outlive no call but are too large for the user stack. */
    static service_t dhcp;
    int r_dhcp = watchtower_config_load_service(
        "/etc/watchtower/dhcpc.service",
        "/mnt/.fortress/watchtower/dhcpc.service",
        "dhcpc", &dhcp, err, sizeof(err));
    if (r_dhcp == 0) {
        watchtower_table_add_service(&table, &dhcp);
    } else if (wt_file_exists("/sbin/dhcpc")) {
        static service_t def_dhcp = {
            .order = 10,
            .enabled = true,
            .readiness_mode = READINESS_FILE,
            .restart_policy = RESTART_ON_FAILURE,
            .restart_limit = 3,
            .restart_delay_sec = 5,
            .timeout_start_sec = 5,
            .timeout_stop_sec = 5
        };
        for (size_t i = 0; i < sizeof(def_dhcp.name); i++) def_dhcp.name[i] = 0;
        for (size_t i = 0; i < sizeof(def_dhcp.exec_path); i++) def_dhcp.exec_path[i] = 0;
        for (size_t i = 0; i < sizeof(def_dhcp.ready_path); i++) def_dhcp.ready_path[i] = 0;
        const char *name = "dhcpc";
        for (size_t i = 0; name[i]; i++) def_dhcp.name[i] = name[i];
        const char *path = "/sbin/dhcpc";
        for (size_t i = 0; path[i]; i++) def_dhcp.exec_path[i] = path[i];
        const char *ready = "/run/dhcpc.lease";
        for (size_t i = 0; ready[i]; i++) def_dhcp.ready_path[i] = ready[i];
        const char *arg0 = "-f";
        for (size_t i = 0; arg0[i]; i++) def_dhcp.args[0][i] = arg0[i];
        def_dhcp.argc = 1;
        watchtower_table_add_service(&table, &def_dhcp);
    }

    static service_t ssh;
    int r_ssh = watchtower_config_load_service(
        "/etc/watchtower/sshd.service",
        "/mnt/.fortress/watchtower/sshd.service",
        "sshd", &ssh, err, sizeof(err));
    if (r_ssh == 0) {
        watchtower_table_add_service(&table, &ssh);
    } else if (wt_file_exists("/sbin/sshd")) {
        static service_t def_ssh = {
            .order = 20,
            .enabled = true,
            .readiness_mode = READINESS_FILE,
            .restart_policy = RESTART_ON_FAILURE,
            .restart_limit = 3,
            .restart_delay_sec = 5,
            .timeout_start_sec = 5,
            .timeout_stop_sec = 10
        };
        for (size_t i = 0; i < sizeof(def_ssh.name); i++) def_ssh.name[i] = 0;
        for (size_t i = 0; i < sizeof(def_ssh.exec_path); i++) def_ssh.exec_path[i] = 0;
        for (size_t i = 0; i < sizeof(def_ssh.ready_path); i++) def_ssh.ready_path[i] = 0;
        const char *name = "sshd";
        for (size_t i = 0; name[i]; i++) def_ssh.name[i] = name[i];
        const char *path = "/sbin/sshd";
        for (size_t i = 0; path[i]; i++) def_ssh.exec_path[i] = path[i];
        const char *ready = "/run/watchtower/sshd.ready";
        for (size_t i = 0; ready[i]; i++) def_ssh.ready_path[i] = ready[i];
        const char *arg0 = "-D";
        for (size_t i = 0; arg0[i]; i++) def_ssh.args[0][i] = arg0[i];
        def_ssh.argc = 1;
        watchtower_table_add_service(&table, &def_ssh);
    }

    static service_t crond_svc;
    int r_crond = watchtower_config_load_service(
        "/etc/watchtower/crond.service",
        "/mnt/.fortress/watchtower/crond.service",
        "crond", &crond_svc, err, sizeof(err));
    if (r_crond == 0) {
        watchtower_table_add_service(&table, &crond_svc);
    } else if (wt_file_exists("/sbin/crond")) {
        static service_t def_crond = {
            .order = 30,
            .enabled = true,
            .readiness_mode = READINESS_NONE,
            .restart_policy = RESTART_ON_FAILURE,
            .restart_limit = 3,
            .restart_delay_sec = 5,
            .timeout_start_sec = 5,
            .timeout_stop_sec = 2
        };
        for (size_t i = 0; i < sizeof(def_crond.name); i++) def_crond.name[i] = 0;
        for (size_t i = 0; i < sizeof(def_crond.exec_path); i++) def_crond.exec_path[i] = 0;
        for (size_t i = 0; i < sizeof(def_crond.ready_path); i++) def_crond.ready_path[i] = 0;
        const char *name = "crond";
        for (size_t i = 0; name[i]; i++) def_crond.name[i] = name[i];
        const char *path = "/sbin/crond";
        for (size_t i = 0; path[i]; i++) def_crond.exec_path[i] = path[i];
        const char *arg0 = "-f";
        for (size_t i = 0; arg0[i]; i++) def_crond.args[0][i] = arg0[i];
        def_crond.argc = 1;
        watchtower_table_add_service(&table, &def_crond);
    }

    /* Initial boot start in Order sequence */
    uint64_t now = wt_monotonic_sec();
    for (size_t i = 0; i < table.count; i++) {
        if (table.services[i].enabled) {
            char start_msg[128];
            start_msg[0] = '\0';
            wt_strlcpy(start_msg, "[watchtower] Starting service: ", sizeof(start_msg));
            wt_strlcat(start_msg, table.services[i].name, sizeof(start_msg));
            wt_strlcat(start_msg, "\n", sizeof(start_msg));
            wt_log_event(start_msg);

            watchtower_start_service(&table, i, now, &ops);
            wt_drain_pipes(&table);
        }
    }

    /* Main Supervisor Event Loop */
    while (!g_term_requested || !watchtower_all_stopped(&table)) {
        now = wt_monotonic_sec();

        /* Drain service pipe buffers */
        wt_drain_pipes(&table);

        /* Reap any terminated child processes */
        for (;;) {
            uint64_t status = 0;
            long pid = sys_call(SYS_WAITPID, (uintptr_t)-1, (uintptr_t)&status, WNOHANG, 0, 0, 0);
            if (pid <= 0) break;
            watchtower_handle_child_exit(&table, (int64_t)pid, (int)status, now);
        }

        /* Drain after reaping to catch last output before process demise */
        wt_drain_pipes(&table);

        /* Periodic tick: readiness, backoffs, timeouts */
        watchtower_tick(&table, now, &ops);

        /* Handle shutdown request */
        if (g_term_requested) {
            if (!table.stopping_all) {
                wt_log_event("[watchtower] Shutdown signal received, stopping services...\n");
                watchtower_stop_all(&table, now, &ops);
            }
        }

        /* Process incoming CLI control requests */
        long req_fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_REQ_FILE, VFS_O_RDONLY, 0, 0, 0, 0);
        if (req_fd >= 0) {
            stat_ext_v1_t st;
            for (size_t s = 0; s < sizeof(st); s++) ((uint8_t *)&st)[s] = 0;
            long sret = sys_call(SYS_FSTAT, (uintptr_t)req_fd, (uintptr_t)&st, 0, 0, 0, 0);
            uint32_t caller_uid = (sret == 0) ? st.uid : 0;
            uint32_t caller_gid = (sret == 0) ? st.gid : 0;

            static char req_buf[WATCHTOWER_CTL_MAX_REQ];
            long n = sys_call(SYS_READ, (uintptr_t)req_fd, (uintptr_t)req_buf, sizeof(req_buf) - 1, 0, 0, 0);
            (void)sys_call(SYS_CLOSE, (uintptr_t)req_fd, 0, 0, 0, 0, 0);
            (void)sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_CTL_REQ_FILE, 0, 0, 0, 0, 0);

            if (n > 0) {
                req_buf[n] = '\0';
                static char res_buf[WATCHTOWER_CTL_MAX_RES];
                watchtower_ctl_process_request(&table, req_buf, caller_uid, caller_gid,
                                               res_buf, sizeof(res_buf), &ops);
                long res_fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_RES_FILE,
                                       VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
                if (res_fd >= 0) {
                    (void)sys_call(SYS_WRITE, (uintptr_t)res_fd, (uintptr_t)res_buf, wt_strlen(res_buf), 0, 0, 0);
                    (void)sys_call(SYS_CLOSE, (uintptr_t)res_fd, 0, 0, 0, 0, 0);
                }
            }
        }

        wt_sleep_ms(50);
    }

    /* Final pipe drain and fd cleanup */
    wt_drain_pipes(&table);
    for (size_t i = 0; i < table.count; i++) {
        if (table.services[i].pipe_fd >= 0) {
            (void)sys_call(SYS_CLOSE, (uintptr_t)table.services[i].pipe_fd, 0, 0, 0, 0, 0);
            table.services[i].pipe_fd = -1;
        }
    }

    wt_log_event("[watchtower] All services stopped cleanly. Exiting.\n");
    sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_PID_FILE, 0, 0, 0, 0, 0);
    return 0;
}
