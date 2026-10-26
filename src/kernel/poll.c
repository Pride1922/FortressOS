#include "poll.h"
#include "vfs.h"
#include "thread.h"
#include "syscall.h"
#include "syscall_abi.h"
#include "vmm.h"
#include "string.h"
#include "time.h"
#include "apic.h"
#include "pipe.h"
#include "pty.h"
#include "net_socket.h"
#include "input.h"
#include "mouse_dev.h"

static _Atomic uint64_t g_poll_wake_seq = 1;

void poll_wake_notify(void) {
    __atomic_add_fetch(&g_poll_wake_seq, 1, __ATOMIC_RELEASE);
    sched_wake_all((void *)time_get_sleep_channel());
}

typedef struct {
    uint64_t seq;
    uint64_t deadline_ticks;
    bool has_deadline;
} poll_sleep_arg_t;

static bool poll_wake_predicate(void *arg) {
    poll_sleep_arg_t *w = (poll_sleep_arg_t *)arg;
    if (w->has_deadline && apic_timer_get_bsp_ticks() >= w->deadline_ticks) {
        return true;
    }
    return __atomic_load_n(&g_poll_wake_seq, __ATOMIC_ACQUIRE) != w->seq;
}

static int poll_scan(tcb_t *curr, struct pollfd *k_fds, uint64_t nfds) {
    int ready = 0;
    for (uint64_t i = 0; i < nfds; ++i) {
        k_fds[i].revents = 0;
        int fd = k_fds[i].fd;
        if (fd < 0) {
            continue;
        }
        if (fd >= MAX_PROCESS_FDS) {
            k_fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        file_t *f = fd_get(curr, fd);
        if (!f || !f->node) {
            k_fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }
        short r = 0;
        if (is_pipe_node(f->node)) {
            r = pipe_poll(f->node, k_fds[i].events);
        } else if (is_pty_slave(f->node) || is_pty_master(f->node)) {
            r = pty_poll(f->node, k_fds[i].events);
        } else if (net_socket_file(f)) {
            r = net_socket_poll(f, k_fds[i].events);
        } else if (is_mouse_dev_node(f->node)) {
            r = 0;
            if ((k_fds[i].events & POLLIN) && mouse_dev_has_data()) r |= POLLIN;
            if (k_fds[i].events & POLLOUT) r |= POLLOUT;
        } else if (is_terminal_node(f->node)) {
            r = 0;
            if (k_fds[i].events & POLLOUT) r |= POLLOUT;
            if ((k_fds[i].events & POLLIN) && input_has_data()) r |= POLLIN;
        } else {
            /* Regular files, directories, devfs nodes are always immediately ready */
            r = (k_fds[i].events & (POLLIN | POLLOUT));
        }
        k_fds[i].revents = r;
        if (r != 0) {
            ready++;
        }
    }
    return ready;
}

int64_t sys_poll(uintptr_t user_fds, uint64_t nfds, int64_t timeout_ms) {
    if (nfds > MAX_PROCESS_FDS) {
        return SYSCALL_EINVAL;
    }

    tcb_t *curr = thread_current();
    if (!curr) {
        return SYSCALL_EBADF;
    }

    uint64_t *pml4 = vmm_get_active_pml4_virt();
    if (nfds > 0) {
        if (!vmm_validate_user_range(pml4, user_fds, nfds * sizeof(struct pollfd), true)) {
            return SYSCALL_EFAULT;
        }
    }

    struct pollfd k_fds[MAX_PROCESS_FDS];
    if (nfds > 0) {
        memcpy(k_fds, (const void *)user_fds, nfds * sizeof(struct pollfd));
    }

    bool has_deadline = (timeout_ms > 0);
    uint64_t deadline_ticks = 0;
    if (has_deadline) {
        uint64_t hz = apic_timer_get_frequency();
        if (hz > 0) {
            uint64_t ticks = (hz * (uint64_t)timeout_ms) / 1000ULL;
            if (ticks == 0 && timeout_ms > 0) ticks = 1;
            deadline_ticks = apic_timer_get_bsp_ticks() + ticks;
        }
    }

    for (;;) {
        if (process_signal_interrupt()) {
            return SYSCALL_EINTR;
        }

        uint64_t cur_seq = __atomic_load_n(&g_poll_wake_seq, __ATOMIC_ACQUIRE);
        int ready = poll_scan(curr, k_fds, nfds);
        if (ready > 0) {
            if (nfds > 0) {
                memcpy((void *)user_fds, k_fds, nfds * sizeof(struct pollfd));
            }
            return ready;
        }

        if (timeout_ms == 0) {
            if (nfds > 0) {
                memcpy((void *)user_fds, k_fds, nfds * sizeof(struct pollfd));
            }
            return 0;
        }

        if (has_deadline && apic_timer_get_bsp_ticks() >= deadline_ticks) {
            if (nfds > 0) {
                memcpy((void *)user_fds, k_fds, nfds * sizeof(struct pollfd));
            }
            return 0;
        }

        poll_sleep_arg_t wait_arg = {
            .seq = cur_seq,
            .deadline_ticks = deadline_ticks,
            .has_deadline = has_deadline
        };
        sched_wait_until(time_get_sleep_channel(), poll_wake_predicate, &wait_arg);
    }
}
