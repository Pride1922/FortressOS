#include "sshd_teardown.h"
#include "syscall_abi.h"

int sshd_daemon_teardown(sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS],
                         long sock_fd,
                         uint32_t timeout_ms,
                         const sshd_teardown_ops_t *ops) {
    if (!slots || !ops) return -1;

    /* Step 2: Stop accepting new connections -> close listening socket immediately */
    if (sock_fd >= 0 && ops->close) {
        (void)ops->close((int)sock_fd);
    }

    /* Step 3: Send SIGTERM to each active worker */
    for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
        if (slots[i].active && slots[i].pid > 0 && ops->kill) {
            (void)ops->kill(slots[i].pid, SIGTERM);
        }
    }

    /* Step 5: Wait bounded (up to timeout_ms, e.g. 5000ms) for workers to exit */
    uint32_t elapsed = 0;
    const uint32_t poll_interval_ms = 50;

    for (;;) {
        bool any_active = false;
        for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
            if (slots[i].active && slots[i].pid > 0) {
                uint64_t status = 0;
                long w = ops->waitpid ? ops->waitpid(slots[i].pid, &status, WNOHANG) : -1;
                if (w == slots[i].pid) {
                    slots[i].active = false;
                    if (slots[i].p_mon_in >= 0 && ops->close) {
                        (void)ops->close(slots[i].p_mon_in);
                        slots[i].p_mon_in = -1;
                    }
                    if (slots[i].p_mon_out >= 0 && ops->close) {
                        (void)ops->close(slots[i].p_mon_out);
                        slots[i].p_mon_out = -1;
                    }
                } else {
                    any_active = true;
                }
            }
        }

        if (!any_active) {
            break; /* All workers exited cleanly before timeout */
        }

        if (elapsed >= timeout_ms) {
            /* Timeout exceeded: Step 6 Escalate SIGKILL to worker process groups */
            for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
                if (slots[i].active && slots[i].pid > 0 && ops->kill) {
                    (void)ops->kill(slots[i].pid, SIGKILL);
                }
            }

            /* Final non-blocking or blocking reap attempt */
            for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
                if (slots[i].active && slots[i].pid > 0) {
                    uint64_t status = 0;
                    if (ops->waitpid) {
                        (void)ops->waitpid(slots[i].pid, &status, 0);
                    }
                    slots[i].active = false;
                    if (slots[i].p_mon_in >= 0 && ops->close) {
                        (void)ops->close(slots[i].p_mon_in);
                        slots[i].p_mon_in = -1;
                    }
                    if (slots[i].p_mon_out >= 0 && ops->close) {
                        (void)ops->close(slots[i].p_mon_out);
                        slots[i].p_mon_out = -1;
                    }
                }
            }
            break;
        }

        if (ops->sleep_ms) {
            ops->sleep_ms(poll_interval_ms);
        }
        elapsed += poll_interval_ms;
    }

    return 0;
}
