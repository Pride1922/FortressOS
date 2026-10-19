#include "types.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "vfs.h"
#include "watchtower.h"
#include "watchtower_ctl.h"

#ifndef SYSCALL_ESRCH
#define SYSCALL_ESRCH 3
#endif

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

static size_t cli_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void cli_print_out(const char *s) {
    size_t len = cli_strlen(s);
    if (len > 0) {
        (void)sys_call(SYS_WRITE, 1, (uintptr_t)s, len, 0, 0, 0);
    }
}

static void cli_print_err(const char *s) {
    size_t len = cli_strlen(s);
    if (len > 0) {
        (void)sys_call(SYS_WRITE, 2, (uintptr_t)s, len, 0, 0, 0);
    }
}

static bool cli_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (char)(*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (char)(*s2 + 32) : *s2;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return *s1 == '\0' && *s2 == '\0';
}

static void cli_sleep_ms(uint32_t ms) {
    timespec_t ts;
    ts.tv_sec = (int64_t)(ms / 1000);
    ts.tv_nsec = (int64_t)((ms % 1000) * 1000000ULL);
    (void)sys_call(SYS_NANOSLEEP, (uintptr_t)&ts, 0, 0, 0, 0, 0);
}

static int64_t cli_get_supervisor_pid(void) {
    long fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_PID_FILE, VFS_O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;

    char buf[32];
    long n = sys_call(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int64_t pid = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            pid = pid * 10 + (buf[i] - '0');
        } else break;
    }
    return pid > 0 ? pid : -1;
}

static void cli_usage(void) {
    cli_print_err("usage: service <command> [service_name]\n\n"
                  "Commands:\n"
                  "  list             List all configured services and status\n"
                  "  status <name>    Show detailed status of a service\n"
                  "  start <name>     Start a service (requires root or wheel)\n"
                  "  stop <name>      Stop a service (requires root or wheel)\n"
                  "  restart <name>   Restart a service (requires root or wheel)\n"
                  "  reload <name>    Reload service configuration (requires root or wheel)\n"
                  "  enable <name>    Enable service on boot (requires root or wheel)\n"
                  "  disable <name>   Disable service on boot (requires root or wheel)\n"
                  "  check <name>     Check readiness of a service\n");
}

int service_main(int argc, char **argv) {
    if (argc < 2) {
        cli_usage();
        return 1;
    }

    const char *action = argv[1];
    if (cli_strcasecmp(action, "--help") || cli_strcasecmp(action, "-h") || cli_strcasecmp(action, "help")) {
        cli_usage();
        return 0;
    }

    const char *svc_name = (argc >= 3) ? argv[2] : "";

    /* Validate command syntax before contacting supervisor */
    bool is_list = cli_strcasecmp(action, "list");
    bool is_status = cli_strcasecmp(action, "status");
    bool is_start = cli_strcasecmp(action, "start");
    bool is_stop = cli_strcasecmp(action, "stop");
    bool is_restart = cli_strcasecmp(action, "restart");
    bool is_reload = cli_strcasecmp(action, "reload");
    bool is_enable = cli_strcasecmp(action, "enable");
    bool is_disable = cli_strcasecmp(action, "disable");
    bool is_check = cli_strcasecmp(action, "check");

    if (!is_list && !is_status && !is_start && !is_stop && !is_restart &&
        !is_reload && !is_enable && !is_disable && !is_check) {
        cli_print_err("service: unknown command '");
        cli_print_err(action);
        cli_print_err("'\n");
        cli_usage();
        return 1;
    }

    if (!is_list && !svc_name[0]) {
        cli_print_err("service: missing service name for '");
        cli_print_err(action);
        cli_print_err("'\n");
        return 1;
    }

    /* Check supervisor status */
    int64_t pid = cli_get_supervisor_pid();
    if (pid <= 0) {
        cli_print_err("service: watchtower supervisor is not running\n");
        return 2;
    }

    /* Verify PID is alive via signal 0 */
    long alive = sys_call(SYS_KILL, (uintptr_t)pid, 0, 0, 0, 0, 0);
    if (alive < 0) {
        cli_print_err("service: watchtower supervisor is not running (stale pid)\n");
        return 2;
    }

    /* Build request string */
    /* The CLI runs once per process; these bounded wire buffers must not
     * consume the one-page Ring 3 stack. */
    static char req[WATCHTOWER_CTL_MAX_REQ];
    size_t rlen = 0;
    for (size_t i = 0; action[i]; i++) req[rlen++] = action[i];
    if (svc_name[0]) {
        req[rlen++] = ' ';
        for (size_t i = 0; svc_name[i]; i++) req[rlen++] = svc_name[i];
    }
    req[rlen++] = '\n';
    req[rlen] = '\0';

    /* Acquire lock (bounded 500ms spin) */
    int lock_tries = 50;
    long lock_fd = -1;
    while (lock_tries-- > 0) {
        long check_fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_LOCK_FILE, VFS_O_RDONLY, 0, 0, 0, 0);
        if (check_fd >= 0) {
            (void)sys_call(SYS_CLOSE, (uintptr_t)check_fd, 0, 0, 0, 0, 0);
            cli_sleep_ms(10);
            continue;
        }
        lock_fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_LOCK_FILE,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0600, 0, 0, 0);
        if (lock_fd >= 0) break;
        cli_sleep_ms(10);
    }
    if (lock_fd < 0) {
        cli_print_err("service: control channel busy (could not acquire lock)\n");
        return 2;
    }
    (void)sys_call(SYS_CLOSE, (uintptr_t)lock_fd, 0, 0, 0, 0, 0);

    /* Write request */
    long req_fd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_REQ_FILE,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0600, 0, 0, 0);
    if (req_fd < 0) {
        sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_CTL_LOCK_FILE, 0, 0, 0, 0, 0);
        cli_print_err("service: failed to create request file\n");
        return 2;
    }
    (void)sys_call(SYS_WRITE, (uintptr_t)req_fd, (uintptr_t)req, rlen, 0, 0, 0);
    (void)sys_call(SYS_CLOSE, (uintptr_t)req_fd, 0, 0, 0, 0, 0);

    /* Wake supervisor via SIGHUP (signal 1) */
    sys_call(SYS_KILL, (uintptr_t)pid, SIGHUP, 0, 0, 0, 0);

    /* Wait for response (up to 2000 ms) */
    static char res[WATCHTOWER_CTL_MAX_RES];
    long res_read = -1;
    int wait_tries = 200; /* 200 * 10ms = 2.0s */

    while (wait_tries-- > 0) {
        cli_sleep_ms(10);
        long rfd = sys_call(SYS_OPEN, (uintptr_t)WATCHTOWER_CTL_RES_FILE, VFS_O_RDONLY, 0, 0, 0, 0);
        if (rfd >= 0) {
            res_read = sys_call(SYS_READ, (uintptr_t)rfd, (uintptr_t)res, sizeof(res) - 1, 0, 0, 0);
            (void)sys_call(SYS_CLOSE, (uintptr_t)rfd, 0, 0, 0, 0, 0);
            if (res_read >= 0) {
                res[res_read] = '\0';
                sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_CTL_RES_FILE, 0, 0, 0, 0, 0);
                break;
            }
        }
    }

    /* Release lock */
    sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_CTL_LOCK_FILE, 0, 0, 0, 0, 0);

    if (res_read < 0) {
        sys_call(SYS_UNLINK, (uintptr_t)WATCHTOWER_CTL_REQ_FILE, 0, 0, 0, 0, 0);
        cli_print_err("service: supervisor request timed out (no response in 2s)\n");
        return 2;
    }

    /* Process response and set exit code */
    if (res[0] == 'E' && res[1] == 'R' && res[2] == 'R') {
        cli_print_err(res);
        return 1;
    } else if (res[0] == 'W' && res[1] == 'A' && res[2] == 'R' && res[3] == 'N') {
        cli_print_out(res);
        return 1;
    } else {
        cli_print_out(res);
        return 0;
    }
}
