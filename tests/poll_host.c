/* FortressOS sys_poll and Subsystem Readiness Host Verification Suite */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"

/* --- Mock kernel structures and environments --- */
typedef struct spinlock {
    uint32_t val;
} spinlock_t;

static inline uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }

/* Pipe polling test adapter */
#define PIPE_CAPACITY (64 * 1024)
#define PIPE_BUF 4096
typedef struct pipe {
    spinlock_t lock;
    uintptr_t buffer_phys;
    uint8_t *buffer;
    size_t head, tail, count;
    _Atomic uint32_t readers, writers;
    _Atomic uint32_t data_bytes, space_bytes;
    _Atomic uint32_t active_endpoints;
    vfs_node_t *read_node, *write_node;
} pipe_t;

static short test_pipe_poll(vfs_node_t *node, short events) {
    if (!node || !node->fs_private) return POLLNVAL;
    pipe_t *p = (pipe_t *)node->fs_private;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    short revents = 0;
    if (node == p->read_node) {
        if ((events & POLLIN) && p->count > 0) {
            revents |= POLLIN;
        }
        if (__atomic_load_n(&p->writers, __ATOMIC_ACQUIRE) == 0) {
            revents |= POLLHUP;
            if (p->count > 0) revents |= POLLIN;
        }
    }
    if (node == p->write_node) {
        if (__atomic_load_n(&p->readers, __ATOMIC_ACQUIRE) == 0) {
            revents |= POLLERR;
        } else if ((events & POLLOUT) && (PIPE_CAPACITY - p->count) > 0) {
            revents |= POLLOUT;
        }
    }
    spin_unlock_irqrestore(&p->lock, flags);
    return revents;
}

#define PTY_IN_CAPACITY   4096
#define PTY_OUT_CAPACITY  16384
typedef struct {
    spinlock_t lock;
    size_t in_count;
    size_t out_count;
    bool master_open;
    bool slave_open;
    vfs_node_t *master_node;
    vfs_node_t *slave_node;
} test_pty_t;

static short test_pty_poll(test_pty_t *p, vfs_node_t *node, short events) {
    if (!node || !p) return POLLNVAL;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    short revents = 0;
    if (node == p->slave_node) {
        if ((events & POLLIN) && p->in_count > 0) {
            revents |= POLLIN;
        }
        if (!__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE)) {
            revents |= POLLHUP;
            if (p->in_count > 0) revents |= POLLIN;
        }
        if (events & POLLOUT) {
            if (!__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE)) {
                revents |= POLLERR;
            } else if ((PTY_OUT_CAPACITY - p->out_count) > 0) {
                revents |= POLLOUT;
            }
        }
    } else if (node == p->master_node) {
        if ((events & POLLIN) && p->out_count > 0) {
            revents |= POLLIN;
        }
        if (!__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE)) {
            revents |= POLLHUP;
            if (p->out_count > 0) revents |= POLLIN;
        }
        if (events & POLLOUT) {
            if (!__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE)) {
                revents |= POLLERR;
            } else if ((PTY_IN_CAPACITY - p->in_count) > 0) {
                revents |= POLLOUT;
            }
        }
    } else {
        revents = POLLNVAL;
    }
    spin_unlock_irqrestore(&p->lock, irq);
    return revents;
}

int main(void) {
    printf("[1] Testing Pipe Readiness Transitions...\n");
    pipe_t pipe;
    memset(&pipe, 0, sizeof(pipe));
    vfs_node_t r_node;
    vfs_node_t w_node;
    memset(&r_node, 0, sizeof(r_node));
    memset(&w_node, 0, sizeof(w_node));
    r_node.fs_private = &pipe;
    w_node.fs_private = &pipe;

    pipe.read_node = &r_node;
    pipe.write_node = &w_node;
    pipe.readers = 1;
    pipe.writers = 1;
    pipe.count = 0;

    /* Empty pipe */
    assert(test_pipe_poll(&r_node, POLLIN) == 0);
    assert(test_pipe_poll(&w_node, POLLOUT) == POLLOUT);

    /* Pipe with 100 bytes */
    pipe.count = 100;
    assert(test_pipe_poll(&r_node, POLLIN) == POLLIN);
    assert(test_pipe_poll(&w_node, POLLOUT) == POLLOUT);

    /* Pipe full */
    pipe.count = PIPE_CAPACITY;
    assert(test_pipe_poll(&r_node, POLLIN) == POLLIN);
    assert(test_pipe_poll(&w_node, POLLOUT) == 0);

    /* Writer closed with data in pipe */
    pipe.writers = 0;
    assert((test_pipe_poll(&r_node, POLLIN) & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP));

    /* Writer closed and pipe drained */
    pipe.count = 0;
    assert(test_pipe_poll(&r_node, POLLIN) == POLLHUP);

    /* Reader closed */
    pipe.readers = 0;
    assert(test_pipe_poll(&w_node, POLLOUT) == POLLERR);
    printf("    -> Pipe readiness verified.\n");

    printf("[2] Testing PTY Master/Slave Readiness Transitions...\n");
    test_pty_t pty;
    memset(&pty, 0, sizeof(pty));
    vfs_node_t m_node;
    vfs_node_t s_node;
    memset(&m_node, 0, sizeof(m_node));
    memset(&s_node, 0, sizeof(s_node));
    pty.master_node = &m_node;
    pty.slave_node = &s_node;
    pty.master_open = true;
    pty.slave_open = true;

    /* Initial state: both writable, neither readable */
    assert(test_pty_poll(&pty, &m_node, POLLIN | POLLOUT) == POLLOUT);
    assert(test_pty_poll(&pty, &s_node, POLLIN | POLLOUT) == POLLOUT);

    /* Master sends 10 bytes to slave */
    pty.in_count = 10;
    assert(test_pty_poll(&pty, &s_node, POLLIN | POLLOUT) == (POLLIN | POLLOUT));
    assert(test_pty_poll(&pty, &m_node, POLLIN | POLLOUT) == POLLOUT);

    /* Slave sends 20 bytes to master */
    pty.out_count = 20;
    assert(test_pty_poll(&pty, &m_node, POLLIN | POLLOUT) == (POLLIN | POLLOUT));

    /* Master closes */
    pty.master_open = false;
    assert((test_pty_poll(&pty, &s_node, POLLIN | POLLOUT) & POLLHUP) != 0);
    assert((test_pty_poll(&pty, &s_node, POLLIN | POLLOUT) & POLLERR) != 0);
    printf("    -> PTY readiness verified.\n");

    printf("[3] Testing Poll ABI & Constants...\n");
    assert(sizeof(struct pollfd) == 8);
    assert(POLLIN == 0x0001);
    assert(POLLPRI == 0x0002);
    assert(POLLOUT == 0x0004);
    assert(POLLERR == 0x0008);
    assert(POLLHUP == 0x0010);
    assert(POLLNVAL == 0x0020);
    assert(SYS_POLL == 88);
    printf("    -> ABI constants verified.\n");

    printf("ALL POLL HOST TESTS PASSED (100%%).\n");
    return 0;
}
