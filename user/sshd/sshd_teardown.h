#ifndef USER_SSHD_TEARDOWN_H
#define USER_SSHD_TEARDOWN_H

#include "types.h"
#include "sshd_admission.h"

/* Bounded timeout in milliseconds before SIGKILL escalation */
#define SSHD_TEARDOWN_TIMEOUT_MS 5000

/* Hookable operations table for host testing and kernel syscalls */
typedef struct {
    int (*kill)(int64_t pid, int sig);
    int64_t (*waitpid)(int64_t pid, uint64_t *status, int options);
    int (*close)(int fd);
    void (*sleep_ms)(uint32_t ms);
} sshd_teardown_ops_t;

/* Execute pinned daemon teardown sequence on SIGTERM/reboot:
 * 1. Receive SIGTERM (indicated by calling this function).
 * 2. Stop accepting new connections: close listening sock_fd immediately if >= 0.
 * 3. Send SIGTERM to each active worker (slots[i].pid).
 * 4. Each worker receives SIGTERM, exiting its poll loop and invoking pty_bridge_cleanup(),
 *    which closes master_fd and delivers SIGHUP to the session's controlling_sid.
 * 5. Monitor waits bounded (up to 5s / SSHD_TEARDOWN_TIMEOUT_MS) for worker children to exit,
 *    reaping them via waitpid(..., WNOHANG) in small sleep intervals.
 * 6. If any worker child is still alive after timeout_ms, escalate SIGKILL to worker PID
 *    and waitpid to reap.
 * 7. Close monitor IPC descriptors (p_mon_in, p_mon_out) and mark slots inactive.
 * Returns 0 on clean completion.
 */
int sshd_daemon_teardown(sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS],
                         long sock_fd,
                         uint32_t timeout_ms,
                         const sshd_teardown_ops_t *ops);

#endif /* USER_SSHD_TEARDOWN_H */
