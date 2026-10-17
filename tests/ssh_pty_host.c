#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "user/sshd/channel.h"
#include "user/sshd/pty_bridge.h"
#include "src/include/syscall_abi.h"
#include "src/include/terminal.h"
#include "src/fs/vfs.h"

void crypto_wipe(void *p, size_t n) {
    if (!p) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* Mock kernel state */
typedef struct {
    bool master_open;
    bool slave_open;
    uint32_t master_flags;
    winsize_t winsize;
    uint64_t controlling_sid;
    uint64_t fg_pgid;
    bool sighup_delivered;
    bool sigwinch_delivered;

    /* Rings */
    uint8_t in_buf[4096];
    size_t in_head, in_tail, in_count;
    size_t in_limit; /* For testing backpressure */

    uint8_t out_buf[16384];
    size_t out_head, out_tail, out_count;

    /* Child process */
    int64_t child_pid;
    bool child_staged;
    bool child_running;
    bool child_exited;
    uint64_t child_status;
    bool sigterm_received;

    /* Sleep tracking */
    unsigned sleep_calls;
    int64_t last_sleep_ns;
} mock_kernel_t;

static mock_kernel_t g_k;

static void mock_kernel_init(void) {
    memset(&g_k, 0, sizeof(g_k));
    g_k.master_open = false;
    g_k.slave_open = false;
    g_k.in_limit = sizeof(g_k.in_buf);
}

static long mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e, uintptr_t f) {
    (void)d; (void)e; (void)f;
    switch (nr) {
    case SYS_PTY_PAIR: {
        int *fds = (int *)a;
        uint32_t flags = (uint32_t)b;
        if (!fds) return -VFS_EINVAL;
        g_k.master_open = true;
        g_k.slave_open = true;
        g_k.master_flags = flags;
        fds[0] = 10; /* master */
        fds[1] = 11; /* slave */
        return 0;
    }
    case SYS_TERMATTR: {
        int fd = (int)a;
        uint32_t op = (uint32_t)b;
        void *ptr = (void *)c;
        if (fd != 10 && fd != 11) return -VFS_EBADF;
        if (op == TERM_SET_WINSIZE) {
            winsize_t *ws = (winsize_t *)ptr;
            g_k.winsize = *ws;
            g_k.sigwinch_delivered = true;
            return 0;
        } else if (op == TERM_GET_WINSIZE) {
            winsize_t *ws = (winsize_t *)ptr;
            *ws = g_k.winsize;
            return 0;
        } else if (op == PTY_SET_SESSION) {
            uint64_t *spid = (uint64_t *)ptr;
            g_k.controlling_sid = *spid;
            g_k.fg_pgid = *spid;
            return 0;
        }
        return -VFS_EINVAL;
    }
    case SYS_SPAWN_EXT: {
        const char *path = (const char *)a;
        const spawn_opts_t *opts = (const spawn_opts_t *)b;
        if (!path || !opts) return -VFS_EINVAL;
        if ((opts->flags & (SPAWN_SETSID | SPAWN_STAGED)) != (SPAWN_SETSID | SPAWN_STAGED)) {
            return -VFS_EINVAL;
        }
        if (opts->action_count < 3 || !opts->fd_actions) return -VFS_EINVAL;
        const spawn_fd_action_t *acts = (const spawn_fd_action_t *)(uintptr_t)opts->fd_actions;
        if (acts[0].dst_fd != 0 || acts[0].src_fd != 11 ||
            acts[1].dst_fd != 1 || acts[1].src_fd != 11 ||
            acts[2].dst_fd != 2 || acts[2].src_fd != 11) {
            return -VFS_EINVAL;
        }
        g_k.child_pid = 501;
        g_k.child_staged = true;
        g_k.child_running = false;
        return g_k.child_pid;
    }
    case SYS_GROUP_RELEASE: {
        uint64_t pid = (uint64_t)a;
        uint32_t action = (uint32_t)b;
        if (pid != (uint64_t)g_k.child_pid) return -VFS_EINVAL;
        if (action == GROUP_RELEASE) {
            assert(g_k.child_staged);
            assert(g_k.controlling_sid == pid); /* Zero-race contract: bound before release */
            g_k.child_staged = false;
            g_k.child_running = true;
            return 0;
        } else if (action == GROUP_CANCEL) {
            g_k.child_staged = false;
            g_k.child_running = false;
            return 0;
        }
        return -VFS_EINVAL;
    }
    case SYS_CLOSE: {
        int fd = (int)a;
        if (fd == 10) {
            g_k.master_open = false;
            if (g_k.controlling_sid != 0) g_k.sighup_delivered = true;
            return 0;
        } else if (fd == 11) {
            /* Parent bridge closes its temporary slave descriptor; child still holds slave */
            return 0;
        }
        return -VFS_EBADF;
    }
    case SYS_READ: {
        int fd = (int)a;
        uint8_t *dst = (uint8_t *)b;
        size_t count = (size_t)c;
        if (fd != 10) return -VFS_EBADF;

        if (g_k.out_count > 0) {
            size_t n = count < g_k.out_count ? count : g_k.out_count;
            size_t first = sizeof(g_k.out_buf) - g_k.out_tail;
            if (first > n) first = n;
            memcpy(dst, g_k.out_buf + g_k.out_tail, first);
            memcpy(dst + first, g_k.out_buf, n - first);
            g_k.out_tail = (g_k.out_tail + n) % sizeof(g_k.out_buf);
            g_k.out_count -= n;
            return (long)n;
        }

        if (!g_k.slave_open) return 0; /* EOF */
        if (g_k.master_flags & VFS_O_NONBLOCK) return -VFS_EAGAIN;
        return -VFS_EAGAIN;
    }
    case SYS_WRITE: {
        int fd = (int)a;
        const uint8_t *src = (const uint8_t *)b;
        size_t count = (size_t)c;
        if (fd != 10) return -VFS_EBADF;
        if (!g_k.slave_open) return -VFS_EIO;

        size_t space = g_k.in_limit - g_k.in_count;
        if (space == 0) return -VFS_EAGAIN;

        size_t n = count < space ? count : space;
        size_t first = sizeof(g_k.in_buf) - g_k.in_head;
        if (first > n) first = n;
        memcpy(g_k.in_buf + g_k.in_head, src, first);
        memcpy(g_k.in_buf, src + first, n - first);
        g_k.in_head = (g_k.in_head + n) % sizeof(g_k.in_buf);
        g_k.in_count += n;
        return (long)n;
    }
    case SYS_WAITPID: {
        int64_t pid = (int64_t)a;
        uint64_t *status_out = (uint64_t *)b;
        uint32_t options = (uint32_t)c;
        if (pid != g_k.child_pid) return SYSCALL_ECHILD;
        if (g_k.child_exited) {
            if (status_out) *status_out = g_k.child_status;
            return g_k.child_pid;
        }
        if (options & WNOHANG) return 0;
        return 0;
    }
    case SYS_KILL: {
        int64_t pid = (int64_t)a;
        uint64_t sig = (uint64_t)b;
        if (pid == g_k.child_pid) {
            if (sig == SIGTERM || sig == SIGKILL) g_k.sigterm_received = true;
            return 0;
        }
        return SYSCALL_ESRCH;
    }
    case SYS_NANOSLEEP: {
        const timespec_t *req = (const timespec_t *)a;
        if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) return -VFS_EINVAL;
        g_k.sleep_calls++;
        g_k.last_sleep_ns = req->tv_sec * 1000000000ULL + req->tv_nsec;
        return 0;
    }
    default:
        return SYSCALL_ENOSYS;
    }
    return SYSCALL_ENOSYS;
}

static void test_pty_bridge_open_and_winsize(void) {
    printf("Running test_pty_bridge_open_and_winsize...\n");
    mock_kernel_init();
    pty_bridge_set_syscall_hook(mock_syscall);

    pty_bridge_t b;
    pty_bridge_init(&b, 1, 120, 40);
    assert(b.master_fd == -1);
    assert(b.slave_fd == -1);
    assert(b.cols == 120);
    assert(b.rows == 40);

    assert(pty_bridge_open(&b) == 0);
    assert(b.master_fd == 10);
    assert(b.slave_fd == 11);
    assert(b.pty_allocated == true);
    assert(g_k.master_open == true);
    assert(g_k.slave_open == true);
    assert(g_k.master_flags & VFS_O_NONBLOCK);
    assert(g_k.winsize.ws_col == 120);
    assert(g_k.winsize.ws_row == 40);

    /* Test dynamic window resize */
    g_k.sigwinch_delivered = false;
    assert(pty_bridge_set_winsize(&b, 140, 50) == 0);
    assert(g_k.winsize.ws_col == 140);
    assert(g_k.winsize.ws_row == 50);
    assert(g_k.sigwinch_delivered == true);

    pty_bridge_cleanup(&b);
    assert(g_k.master_open == false);
    assert(g_k.sighup_delivered == false); /* no session bound yet */
    printf("PASS: test_pty_bridge_open_and_winsize\n");
}

static void test_pty_bridge_zero_race_spawn(void) {
    printf("Running test_pty_bridge_zero_race_spawn...\n");
    mock_kernel_init();
    pty_bridge_set_syscall_hook(mock_syscall);

    pty_bridge_t b;
    pty_bridge_init(&b, 1, 80, 24);
    assert(pty_bridge_open(&b) == 0);

    char *argv[] = { "/bin/sh", NULL };
    char *envp[] = { "TERM=xterm-256color", NULL };
    assert(pty_bridge_spawn(&b, "/bin/sh", argv, envp, "/home/user") == 0);

    assert(b.child_pid == 501);
    assert(b.child_running == true);
    assert(b.slave_fd == -1); /* slave closed in bridge parent */
    assert(g_k.controlling_sid == 501);
    assert(g_k.fg_pgid == 501);
    assert(g_k.child_staged == false);
    assert(g_k.child_running == true);

    /* Cleanup terminates child with SIGTERM and closes master (delivers SIGHUP to session) */
    pty_bridge_cleanup(&b);
    assert(g_k.master_open == false);
    assert(g_k.sighup_delivered == true);
    assert(g_k.sigterm_received == true);
    assert(b.child_running == false);
    printf("PASS: test_pty_bridge_zero_race_spawn\n");
}

static void test_pty_bridge_duplex_io_and_backpressure(void) {
    printf("Running test_pty_bridge_duplex_io_and_backpressure...\n");
    mock_kernel_init();
    pty_bridge_set_syscall_hook(mock_syscall);

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    /* Open channel 0 with remote channel ID 42 */
    sshbuf_t req, reply;
    sshbuf_init(&req, 128, 512);
    sshbuf_init(&reply, 128, 512);
    sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN);
    sshbuf_put_cstring(&req, "session");
    sshbuf_put_u32(&req, 42);
    sshbuf_put_u32(&req, 32768);
    sshbuf_put_u32(&req, 16384);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    sshbuf_free(&req);
    sshbuf_free(&reply);

    pty_bridge_t b;
    pty_bridge_init(&b, 0, 80, 24);
    assert(pty_bridge_open(&b) == 0);
    char *argv[] = { "/bin/sh", NULL };
    assert(pty_bridge_spawn(&b, "/bin/sh", argv, NULL, NULL) == 0);

    sshbuf_t out_pkts;
    sshbuf_init(&out_pkts, 512, 65536);

    /* 1. Inbound network -> PTY master */
    ssh_channel_t *c = channel_lookup_local(&mgr, 0);
    assert(c != NULL);
    const char *test_input = "echo Hello FortressOS\n";
    size_t in_len = strlen(test_input);
    sshbuf_put_raw(&c->in_data, (const uint8_t *)test_input, in_len);
    assert(sshbuf_len(&c->in_data) == in_len);

    bool active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == true);
    assert(sshbuf_len(&c->in_data) == 0);
    assert(g_k.in_count == in_len);
    assert(memcmp(g_k.in_buf, test_input, in_len) == 0);

    /* 2. Inbound Backpressure / Short-write */
    g_k.in_limit = g_k.in_count + 5; /* Only 5 bytes of space available in PTY ring */
    const char *long_input = "1234567890";
    sshbuf_put_raw(&c->in_data, (const uint8_t *)long_input, 10);

    active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == true);
    /* 5 bytes written to PTY, 5 bytes remain buffered in c->in_data */
    assert(sshbuf_len(&c->in_data) == 5);
    assert(memcmp(sshbuf_ptr(&c->in_data), "67890", 5) == 0);

    /* Full ring: 0 bytes space */
    active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == false); /* no progress due to full buffer */
    assert(sshbuf_len(&c->in_data) == 5);

    /* Clear space in PTY ring */
    g_k.in_count = 0;
    g_k.in_head = 0;
    g_k.in_tail = 0;
    g_k.in_limit = sizeof(g_k.in_buf);
    active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == true);
    assert(sshbuf_len(&c->in_data) == 0);

    /* 3. Outbound PTY master -> network channel */
    const char *slave_output = "Welcome to FortressOS interactive shell!\r\n";
    size_t out_len = strlen(slave_output);
    memcpy(g_k.out_buf, slave_output, out_len);
    g_k.out_head = out_len;
    g_k.out_count = out_len;

    sshbuf_reset(&out_pkts);
    active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == true);
    assert(g_k.out_count == 0);

    /* Verify packet generated is SSH_MSG_CHANNEL_DATA */
    uint8_t ptype = 0;
    uint32_t r_id = 0;
    const uint8_t *pkt_data = NULL;
    size_t pkt_len = 0;
    assert(sshbuf_get_u8(&out_pkts, &ptype) == 0);
    assert(ptype == SSH_MSG_CHANNEL_DATA);
    assert(sshbuf_get_u32(&out_pkts, &r_id) == 0);
    assert(r_id == 42);
    assert(sshbuf_get_string_ptr(&out_pkts, &pkt_data, &pkt_len) == 0);
    assert(pkt_len == out_len);
    assert(memcmp(pkt_data, slave_output, out_len) == 0);

    /* 4. Child exit status and EOF propagation */
    g_k.slave_open = false; /* Child closed its slave descriptor upon exit */
    g_k.child_running = false;
    g_k.child_exited = true;
    g_k.child_status = (0 << 8); /* Exit status 0 */

    sshbuf_reset(&out_pkts);
    active = false;
    assert(pty_bridge_step(&b, &mgr, &out_pkts, &active) == 0);
    assert(active == true);
    assert(b.child_exited == true);
    assert(b.exit_status == 0);

    /* Verify out_pkts contains exit-status, EOF, and CLOSE */
    bool saw_exit_status = false, saw_eof = false, saw_close = false;
    while (sshbuf_len(&out_pkts) > 0) {
        uint8_t t = 0;
        assert(sshbuf_get_u8(&out_pkts, &t) == 0);
        if (t == SSH_MSG_CHANNEL_REQUEST) {
            uint32_t rec = 0;
            const uint8_t *req_name = NULL; size_t req_len = 0;
            assert(sshbuf_get_u32(&out_pkts, &rec) == 0);
            assert(sshbuf_get_string_ptr(&out_pkts, &req_name, &req_len) == 0);
            assert(req_len == 11 && memcmp(req_name, "exit-status", 11) == 0);
            uint8_t wr = 0;
            uint32_t code = 0;
            assert(sshbuf_get_u8(&out_pkts, &wr) == 0);
            assert(sshbuf_get_u32(&out_pkts, &code) == 0);
            assert(code == 0);
            saw_exit_status = true;
        } else if (t == SSH_MSG_CHANNEL_EOF) {
            uint32_t rec = 0;
            assert(sshbuf_get_u32(&out_pkts, &rec) == 0);
            assert(rec == 42);
            saw_eof = true;
        } else if (t == SSH_MSG_CHANNEL_CLOSE) {
            uint32_t rec = 0;
            assert(sshbuf_get_u32(&out_pkts, &rec) == 0);
            assert(rec == 42);
            saw_close = true;
        }
    }
    assert(saw_exit_status);
    assert(saw_eof);
    assert(saw_close);

    sshbuf_free(&out_pkts);
    pty_bridge_cleanup(&b);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_pty_bridge_duplex_io_and_backpressure\n");
}

static void test_pty_bridge_bounded_sleep(void) {
    printf("Running test_pty_bridge_bounded_sleep...\n");
    mock_kernel_init();
    pty_bridge_set_syscall_hook(mock_syscall);

    assert(g_k.sleep_calls == 0);
    pty_bridge_sleep_bounded(20);
    assert(g_k.sleep_calls == 1);
    assert(g_k.last_sleep_ns == 20000000ULL); /* Exactly 20ms */

    pty_bridge_sleep_bounded(50);
    assert(g_k.sleep_calls == 2);
    assert(g_k.last_sleep_ns == 50000000ULL);

    printf("PASS: test_pty_bridge_bounded_sleep\n");
}

int main(void) {
    printf("Starting SSH PTY Bridge & Job Control test suite...\n");
    test_pty_bridge_open_and_winsize();
    test_pty_bridge_zero_race_spawn();
    test_pty_bridge_duplex_io_and_backpressure();
    test_pty_bridge_bounded_sleep();

    printf("\nALL TESTS PASSED (tests/ssh_pty_host.c)\n");
    return 0;
}
