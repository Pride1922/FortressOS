#include "pty_bridge.h"
#include "vfs.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
static pty_bridge_syscall_fn g_syscall_hook = NULL;

void pty_bridge_set_syscall_hook(pty_bridge_syscall_fn fn) {
    g_syscall_hook = fn;
}

static inline long do_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f) {
    if (g_syscall_hook) {
        return g_syscall_hook(nr, a, b, c, d, e, f);
    }
    return -1;
}
#else
static inline long do_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif

void pty_bridge_init(pty_bridge_t *b, uint32_t channel_id, uint16_t cols, uint16_t rows) {
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->master_fd = -1;
    b->slave_fd = -1;
    b->child_pid = -1;
    b->channel_id = channel_id;
    b->cols = cols ? cols : 80;
    b->rows = rows ? rows : 24;
}

int pty_bridge_open(pty_bridge_t *b) {
    if (!b) return -1;
    int fds[2] = { -1, -1 };
    /* Only the duplicated slave stdio may reach the shell. An inherited
     * master prevents the worker's close from delivering session SIGHUP. */
    long r = do_syscall(SYS_PTY_PAIR, (uintptr_t)fds, VFS_O_NONBLOCK | VFS_O_CLOEXEC, 0, 0, 0, 0);
    if (r < 0) return (int)r;

    b->master_fd = fds[0];
    b->slave_fd = fds[1];
    b->pty_allocated = true;

    winsize_t ws = { .ws_row = b->rows, .ws_col = b->cols, .ws_xpixel = 0, .ws_ypixel = 0 };
    (void)do_syscall(SYS_TERMATTR, (uintptr_t)b->master_fd, TERM_SET_WINSIZE, (uintptr_t)&ws, sizeof(ws), 0, 0);
    return 0;
}

int pty_bridge_set_winsize(pty_bridge_t *b, uint16_t cols, uint16_t rows) {
    if (!b) return -1;
    b->cols = cols ? cols : 80;
    b->rows = rows ? rows : 24;
    if (b->master_fd >= 0) {
        winsize_t ws = { .ws_row = b->rows, .ws_col = b->cols, .ws_xpixel = 0, .ws_ypixel = 0 };
        long r = do_syscall(SYS_TERMATTR, (uintptr_t)b->master_fd, TERM_SET_WINSIZE, (uintptr_t)&ws, sizeof(ws), 0, 0);
        if (r < 0) return (int)r;
    }
    return 0;
}

int pty_bridge_spawn(pty_bridge_t *b, const char *path, char *const argv[],
                     char *const envp[], const char *cwd) {
    if (!b || b->slave_fd < 0 || !path) return -1;

    spawn_fd_action_t actions[3];
    memset(actions, 0, sizeof(actions));
    actions[0] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 0, .src_fd = b->slave_fd };
    actions[1] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 1, .src_fd = b->slave_fd };
    actions[2] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 2, .src_fd = b->slave_fd };

    spawn_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.size = sizeof(opts);
    opts.version = 2;
    opts.flags = SPAWN_SETSID | SPAWN_STAGED;
    opts.argv = (uint64_t)(uintptr_t)argv;
    opts.envp = (uint64_t)(uintptr_t)envp;
    opts.cwd = (uint64_t)(uintptr_t)cwd;
    opts.fd_actions = (uint64_t)(uintptr_t)actions;
    opts.action_count = 3;

    long pid = do_syscall(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts), 0, 0, 0);
    if (pid <= 0) return (int)pid;

    /* Zero-race binding: bind session leader while child is staged */
    uint64_t child_pid_val = (uint64_t)pid;
    long bind_err = do_syscall(SYS_TERMATTR, (uintptr_t)b->master_fd, PTY_SET_SESSION,
                               (uintptr_t)&child_pid_val, sizeof(child_pid_val), 0, 0);
    if (bind_err != 0) {
        do_syscall(SYS_GROUP_RELEASE, (uintptr_t)pid, GROUP_CANCEL, 0, 0, 0, 0);
        return (int)bind_err;
    }

    /* Release staged child to execute */
    do_syscall(SYS_GROUP_RELEASE, (uintptr_t)pid, GROUP_RELEASE, 0, 0, 0, 0);

    b->child_pid = pid;
    b->child_running = true;

    /* Close slave_fd in bridge parent; only child owns slave endpoints now */
    do_syscall(SYS_CLOSE, (uintptr_t)b->slave_fd, 0, 0, 0, 0, 0);
    b->slave_fd = -1;

    return 0;
}

int pty_bridge_step(pty_bridge_t *b, ssh_channel_mgr_t *mgr,
                    sshbuf_t *out_packets, bool *out_active) {
    if (!b || !mgr || !out_packets) return -1;
    if (out_active) *out_active = false;

    ssh_channel_t *c = channel_lookup_local(mgr, b->channel_id);
    if (!c) return 0;

    /* 1. Check child process status */
    if (b->child_running && !b->child_exited) {
        uint64_t status = 0;
        long ret = do_syscall(SYS_WAITPID, (uintptr_t)b->child_pid, (uintptr_t)&status, WNOHANG, 0, 0, 0);
        if (ret == b->child_pid) {
            b->child_running = false;
            b->child_exited = true;
            b->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
            channel_send_exit_status(mgr, b->channel_id, (uint32_t)b->exit_status, out_packets);
            if (out_active) *out_active = true;
        }
    }

    /* 2. Inbound network -> PTY master */
    if (b->master_fd >= 0 && sshbuf_len(&c->in_data) > 0) {
        const uint8_t *in_ptr = sshbuf_ptr(&c->in_data);
        size_t in_len = sshbuf_len(&c->in_data);
        long w = do_syscall(SYS_WRITE, (uintptr_t)b->master_fd, (uintptr_t)in_ptr, in_len, 0, 0, 0);
        if (w > 0) {
            sshbuf_consume(&c->in_data, (size_t)w);
            if (out_active) *out_active = true;
        }
    }

    /* 3. Outbound PTY master -> network channel */
    if (b->master_fd >= 0 && (c->state == SSH_CHANNEL_STATE_OPEN || c->state == SSH_CHANNEL_STATE_EOF_REMOTE)) {
        if (c->remote_window > 0) {
            static uint8_t s_pty_read_buf[4096];
            size_t max_read = sizeof(s_pty_read_buf);
            if (max_read > c->remote_window) max_read = c->remote_window;
            if (max_read > c->remote_maxpacket) max_read = c->remote_maxpacket;

            long r = do_syscall(SYS_READ, (uintptr_t)b->master_fd, (uintptr_t)s_pty_read_buf, max_read, 0, 0, 0);
            if (r > 0) {
                channel_send_data(mgr, b->channel_id, s_pty_read_buf, (size_t)r, out_packets);
                if (out_active) *out_active = true;
            } else if (r == 0 || (b->child_exited && r < 0)) {
                /* EOF on master read or child exited: slave closed and all buffered output drained */
                channel_send_eof(mgr, b->channel_id, out_packets);
                channel_send_close(mgr, b->channel_id, out_packets);
                if (out_active) *out_active = true;
            }
        }
    }

    return 0;
}

void pty_bridge_sleep_bounded(uint32_t timeout_ms) {
    if (timeout_ms == 0) return;
    timespec_t req;
    req.tv_sec = (int64_t)(timeout_ms / 1000);
    req.tv_nsec = (int64_t)((timeout_ms % 1000) * 1000000);
    (void)do_syscall(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0, 0, 0, 0);
}

void pty_bridge_cleanup(pty_bridge_t *b) {
    if (!b) return;
    if (b->slave_fd >= 0) {
        (void)do_syscall(SYS_CLOSE, (uintptr_t)b->slave_fd, 0, 0, 0, 0, 0);
        b->slave_fd = -1;
    }
    if (b->master_fd >= 0) {
        /* Closing master_fd delivers SIGHUP to the session */
        (void)do_syscall(SYS_CLOSE, (uintptr_t)b->master_fd, 0, 0, 0, 0, 0);
        b->master_fd = -1;
    }
    if (b->child_running) {
        (void)do_syscall(SYS_KILL, (uintptr_t)b->child_pid, SIGTERM, 0, 0, 0, 0);
        uint64_t status = 0;
        (void)do_syscall(SYS_WAITPID, (uintptr_t)b->child_pid, (uintptr_t)&status, WNOHANG, 0, 0, 0);
        b->child_running = false;
    }
    b->pty_allocated = false;
}
