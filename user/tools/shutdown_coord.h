#ifndef USER_TOOLS_SHUTDOWN_COORD_H
#define USER_TOOLS_SHUTDOWN_COORD_H

#include "types.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "vfs.h"
#include "creds.h"

#define WATCHTOWER_PID_FILE "/run/watchtower/watchtower.pid"

#ifndef SYSCALL_EPERM
#define SYSCALL_EPERM 1
#endif
#ifndef SYSCALL_ESRCH
#define SYSCALL_ESRCH 3
#endif

#ifndef shutdown_coord_syscall
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c);
#define shutdown_coord_syscall tool_syscall
#endif

/* Execute coordinated system shutdown:
 * 1. Verify CAP_SYS_BOOT capability (matching kernel boundary; EUID 0 alone rejected).
 * 2. Detach coordinator from parent SSH session / process group via SYS_SETPGID(0, 0).
 * 3. Mask SIGHUP and SIGTERM to survive PTY closure.
 * 4. Signal Watchtower to stop services in reverse order.
 * 5. Bounded 15s wait verifying actual Watchtower exit via SYS_KILL(pid, 0) == -ESRCH.
 * 6. Issue SYS_REBOOT strictly last.
 */
static inline int shutdown_coordinate_and_reboot(int reboot_cmd) {
    /* 1. Check CAP_SYS_BOOT */
    long caps = shutdown_coord_syscall(SYS_CAPGET, 0, 0, 0);
    if (caps < 0 || !(caps & CAP_SYS_BOOT)) {
        return -SYSCALL_EPERM;
    }

    /* 2. Detach coordinator into its own process group (pgid = pid) */
    (void)shutdown_coord_syscall(SYS_SETPGID, 0, 0, 0);

    /* 3. Mask SIGHUP and SIGTERM so PTY hangup on master closure cannot kill coordinator */
    signal_action_t sa;
    for (size_t i = 0; i < sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.handler = SIG_IGN;
    (void)shutdown_coord_syscall(SYS_SIGACTION, SIGHUP, (uintptr_t)&sa, 0);
    (void)shutdown_coord_syscall(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0);

    /* 4. Find Watchtower supervisor PID from runfs */
    long fd = shutdown_coord_syscall(SYS_OPEN, (uintptr_t)WATCHTOWER_PID_FILE, VFS_O_RDONLY, 0);
    int64_t wt_pid = -1;
    if (fd >= 0) {
        char buf[32];
        long n = shutdown_coord_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1);
        (void)shutdown_coord_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        if (n > 0) {
            buf[n] = '\0';
            int64_t p = 0;
            for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
                p = p * 10 + (buf[i] - '0');
            }
            if (p > 0) wt_pid = p;
        }
    }

    /* 5. Signal Watchtower to stop services and wait for actual exit (ESRCH) */
    if (wt_pid > 0) {
        long kret = shutdown_coord_syscall(SYS_KILL, (uintptr_t)wt_pid, SIGTERM, 0);
        if (kret == 0) {
            /* Poll with bounded 15-second deadline (300 iterations * 50ms) */
            timespec_t ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000ULL;
            int tries = 300;
            while (tries-- > 0) {
                long chk = shutdown_coord_syscall(SYS_KILL, (uintptr_t)wt_pid, 0, 0);
                if (chk == -SYSCALL_ESRCH || chk == SYSCALL_ESRCH) {
                    /* Watchtower has completely exited! */
                    break;
                }
                (void)shutdown_coord_syscall(SYS_NANOSLEEP, (uintptr_t)&ts, 0, 0);
            }
        }
    }

    /* 6. Storage freeze, quiesce, and hardware power-off / reboot strictly last */
    return (int)shutdown_coord_syscall(SYS_REBOOT, (uintptr_t)reboot_cmd, 0, 0);
}

#endif /* USER_TOOLS_SHUTDOWN_COORD_H */
