#include "pty.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "process_table.h"
#include "signal_abi.h"

/* Scheduler predicates under rank 1: atomic load only */
static bool pty_in_ready(void *arg) {
    pty_t *p = arg;
    return __atomic_load_n(&p->in_data_bytes, __ATOMIC_ACQUIRE) > 0 ||
           !__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE);
}

static bool pty_in_space_ready(void *arg) {
    pty_t *p = arg;
    return __atomic_load_n(&p->in_space_bytes, __ATOMIC_ACQUIRE) > 0 ||
           !__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE);
}

static bool pty_out_ready(void *arg) {
    pty_t *p = arg;
    return __atomic_load_n(&p->out_data_bytes, __ATOMIC_ACQUIRE) > 0 ||
           !__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE);
}

static bool pty_out_space_ready(void *arg) {
    pty_t *p = arg;
    return __atomic_load_n(&p->out_space_bytes, __ATOMIC_ACQUIRE) > 0 ||
           !__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE);
}

bool is_pty_master(const vfs_node_t *node) {
    return node && node->read == (int64_t (*)(vfs_node_t *, uint64_t, void *, size_t))pty_master_read;
}

bool is_pty_slave(const vfs_node_t *node) {
    return node && node->read == (int64_t (*)(vfs_node_t *, uint64_t, void *, size_t))pty_slave_read;
}

int pty_create(vfs_node_t **out_master, vfs_node_t **out_slave) {
    if (!out_master || !out_slave || out_master == out_slave) return -VFS_EINVAL;
    *out_master = NULL;
    *out_slave = NULL;

    pty_t *p = kmalloc(sizeof(*p));
    if (!p) return -VFS_ENOMEM;
    memset(p, 0, sizeof(*p));

    p->lock = (spinlock_t)SPINLOCK_RANKED(2, "pty");
    p->in_space_bytes = PTY_IN_CAPACITY;
    p->out_space_bytes = PTY_OUT_CAPACITY;
    p->master_open = true;
    p->slave_open = true;
    p->active_endpoints = 2;

    /* Default terminal attributes: ICANON | ECHO | ISIG */
    p->attrs = (terminal_attrs_t){
        .version = 1,
        .input_flags = TERM_ISIG | TERM_ECHO | TERM_ICANON,
        .vintr = 0x03, /* Ctrl+C */
        .vsusp = 0x1a  /* Ctrl+Z */
    };
    p->cols = 80;
    p->rows = 24;

    p->master_node = kmalloc(sizeof(vfs_node_t));
    p->slave_node = kmalloc(sizeof(vfs_node_t));
    if (!p->master_node || !p->slave_node) {
        if (p->master_node) kfree(p->master_node);
        if (p->slave_node) kfree(p->slave_node);
        kfree(p);
        return -VFS_ENOMEM;
    }

    *p->master_node = (vfs_node_t){
        .type = VFS_STREAM,
        .mode = VFS_S_IFCHR | 0620,
        .is_stream = true,
        .fs_private = p,
        .read = (int64_t (*)(vfs_node_t *, uint64_t, void *, size_t))pty_master_read,
        .write = (int64_t (*)(vfs_node_t *, uint64_t *, bool, const void *, size_t))pty_master_write,
        .close = pty_close_endpoint
    };

    *p->slave_node = (vfs_node_t){
        .type = VFS_STREAM,
        .mode = VFS_S_IFCHR | 0620,
        .is_stream = true,
        .fs_private = p,
        .read = (int64_t (*)(vfs_node_t *, uint64_t, void *, size_t))pty_slave_read,
        .write = (int64_t (*)(vfs_node_t *, uint64_t *, bool, const void *, size_t))pty_slave_write,
        .close = pty_close_endpoint
    };

    *out_master = p->master_node;
    *out_slave = p->slave_node;
    return 0;
}

void pty_close_endpoint(vfs_node_t *node) {
    if (!node || !node->fs_private) return;
    pty_t *p = node->fs_private;
    uint64_t csid = 0;
    process_group_ref_t to_release = {0};

    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (node == p->master_node) {
        p->master_open = false;
        csid = p->controlling_sid;
    } else {
        p->slave_open = false;
    }
    node->fs_private = NULL;
    if (!p->master_open && !p->slave_open) {
        to_release = p->foreground;
        p->foreground = (process_group_ref_t){0};
    }
    spin_unlock_irqrestore(&p->lock, flags);

    if (to_release.generation) {
        (void)process_group_release_ref(&to_release);
    }

    /* Wake all waiting readers and writers in both directions */
    sched_wake_all(&p->in_data_bytes);
    sched_wake_all(&p->in_space_bytes);
    sched_wake_all(&p->out_data_bytes);
    sched_wake_all(&p->out_space_bytes);

    /* Deliver SIGHUP to the entire session if master closed */
    if (csid != 0) {
        process_signal_send_session(csid, SIGHUP);
    }

    if (__atomic_sub_fetch(&p->active_endpoints, 1, __ATOMIC_ACQ_REL) == 0) {
        kfree(p->master_node);
        kfree(p->slave_node);
        kfree(p);
    }
}

int64_t pty_master_read(vfs_node_t *node, uint64_t flags, void *buf, size_t count) {
    if (!node || !buf || count == 0) return 0;
    pty_t *p = node->fs_private;
    if (!p) return -VFS_EBADF;

    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);

        if (p->out_count > 0) {
            size_t n = count < p->out_count ? count : p->out_count;
            size_t first = PTY_OUT_CAPACITY - p->out_tail;
            if (first > n) first = n;
            memcpy(buf, p->out_buf + p->out_tail, first);
            memcpy((uint8_t *)buf + first, p->out_buf, n - first);

            p->out_tail = (p->out_tail + n) % PTY_OUT_CAPACITY;
            p->out_count -= n;

            __atomic_store_n(&p->out_data_bytes, (uint32_t)p->out_count, __ATOMIC_RELEASE);
            __atomic_store_n(&p->out_space_bytes, (uint32_t)(PTY_OUT_CAPACITY - p->out_count), __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&p->lock, irq);

            sched_wake_all(&p->out_space_bytes);
            return (int64_t)n;
        }

        if (!__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, irq);
            return 0; /* EOF */
        }

        if (flags & VFS_O_NONBLOCK) {
            spin_unlock_irqrestore(&p->lock, irq);
            return -VFS_EAGAIN;
        }

        spin_unlock_irqrestore(&p->lock, irq);
        sched_wait_until(&p->out_data_bytes, pty_out_ready, p);
    }
}

int64_t pty_master_write(vfs_node_t *node, uint64_t *flags_ptr, bool append, const void *buf, size_t count) {
    (void)append;
    if (!node || !buf || count == 0) return 0;
    pty_t *p = node->fs_private;
    if (!p) return -VFS_EBADF;

    uint64_t flags = flags_ptr ? *flags_ptr : 0;
    const uint8_t *src = buf;

    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);

        if (!__atomic_load_n(&p->slave_open, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, irq);
            return -VFS_EIO;
        }

        size_t space = PTY_IN_CAPACITY - p->in_count;
        if (space == 0) {
            if (flags & VFS_O_NONBLOCK) {
                spin_unlock_irqrestore(&p->lock, irq);
                return -VFS_EAGAIN;
            }
            spin_unlock_irqrestore(&p->lock, irq);
            sched_wait_until(&p->in_space_bytes, pty_in_space_ready, p);
            continue;
        }

        size_t to_write = count < space ? count : space;
        size_t first = PTY_IN_CAPACITY - p->in_head;
        if (first > to_write) first = to_write;
        memcpy(p->in_buf + p->in_head, src, first);
        memcpy(p->in_buf, src + first, to_write - first);

        p->in_head = (p->in_head + to_write) % PTY_IN_CAPACITY;
        p->in_count += to_write;

        /* Keystroke signal detection */
        uint64_t sig_fg = 0;
        unsigned sig_type = 0;
        if (p->attrs.input_flags & TERM_ISIG) {
            for (size_t i = 0; i < to_write; i++) {
                if (p->attrs.vintr && src[i] == p->attrs.vintr) {
                    sig_fg = p->fg_pgid;
                    sig_type = SIGINT;
                } else if (p->attrs.vsusp && src[i] == p->attrs.vsusp) {
                    sig_fg = p->fg_pgid;
                    sig_type = SIGTSTP;
                }
            }
        }

        /* Line-discipline echo */
        bool did_echo = false;
        if ((p->attrs.input_flags & (TERM_ECHO | TERM_ICANON)) == (TERM_ECHO | TERM_ICANON)) {
            for (size_t i = 0; i < to_write; i++) {
                uint8_t ch = src[i];
                if (p->out_count < PTY_OUT_CAPACITY) {
                    if (ch == '\r') {
                        p->out_buf[p->out_head] = '\r';
                        p->out_head = (p->out_head + 1) % PTY_OUT_CAPACITY;
                        p->out_count++;
                        if (p->out_count < PTY_OUT_CAPACITY) {
                            p->out_buf[p->out_head] = '\n';
                            p->out_head = (p->out_head + 1) % PTY_OUT_CAPACITY;
                            p->out_count++;
                        }
                    } else if (ch >= 0x20 || ch == '\t' || ch == '\n' || ch == 0x7f) {
                        p->out_buf[p->out_head] = ch;
                        p->out_head = (p->out_head + 1) % PTY_OUT_CAPACITY;
                        p->out_count++;
                    }
                    did_echo = true;
                }
            }
        }

        __atomic_store_n(&p->in_data_bytes, (uint32_t)p->in_count, __ATOMIC_RELEASE);
        __atomic_store_n(&p->in_space_bytes, (uint32_t)(PTY_IN_CAPACITY - p->in_count), __ATOMIC_RELEASE);
        if (did_echo) {
            __atomic_store_n(&p->out_data_bytes, (uint32_t)p->out_count, __ATOMIC_RELEASE);
            __atomic_store_n(&p->out_space_bytes, (uint32_t)(PTY_OUT_CAPACITY - p->out_count), __ATOMIC_RELEASE);
        }

        spin_unlock_irqrestore(&p->lock, irq);

        sched_wake_all(&p->in_data_bytes);
        if (did_echo) sched_wake_all(&p->out_data_bytes);

        if (sig_fg != 0 && sig_type != 0) {
            process_signal_send_group(sig_fg, sig_type);
        }

        return (int64_t)to_write;
    }
}

int64_t pty_slave_read(vfs_node_t *node, uint64_t flags, void *buf, size_t count) {
    if (!node || !buf || count == 0) return 0;
    pty_t *p = node->fs_private;
    if (!p) return -VFS_EBADF;

    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);

        if (p->in_count > 0) {
            size_t n = count < p->in_count ? count : p->in_count;
            size_t first = PTY_IN_CAPACITY - p->in_tail;
            if (first > n) first = n;
            memcpy(buf, p->in_buf + p->in_tail, first);
            memcpy((uint8_t *)buf + first, p->in_buf, n - first);

            p->in_tail = (p->in_tail + n) % PTY_IN_CAPACITY;
            p->in_count -= n;

            __atomic_store_n(&p->in_data_bytes, (uint32_t)p->in_count, __ATOMIC_RELEASE);
            __atomic_store_n(&p->in_space_bytes, (uint32_t)(PTY_IN_CAPACITY - p->in_count), __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&p->lock, irq);

            sched_wake_all(&p->in_space_bytes);
            return (int64_t)n;
        }

        if (!__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, irq);
            return 0; /* EOF */
        }

        if (flags & VFS_O_NONBLOCK) {
            spin_unlock_irqrestore(&p->lock, irq);
            return -VFS_EAGAIN;
        }

        spin_unlock_irqrestore(&p->lock, irq);
        sched_wait_until(&p->in_data_bytes, pty_in_ready, p);
    }
}

int64_t pty_slave_write(vfs_node_t *node, uint64_t *flags_ptr, bool append, const void *buf, size_t count) {
    (void)append;
    if (!node || !buf || count == 0) return 0;
    pty_t *p = node->fs_private;
    if (!p) return -VFS_EBADF;

    uint64_t flags = flags_ptr ? *flags_ptr : 0;
    const uint8_t *src = buf;

    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);

        if (!__atomic_load_n(&p->master_open, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, irq);
            return -VFS_EIO;
        }

        size_t space = PTY_OUT_CAPACITY - p->out_count;
        if (space == 0) {
            if (flags & VFS_O_NONBLOCK) {
                spin_unlock_irqrestore(&p->lock, irq);
                return -VFS_EAGAIN;
            }
            spin_unlock_irqrestore(&p->lock, irq);
            sched_wait_until(&p->out_space_bytes, pty_out_space_ready, p);
            continue;
        }

        size_t to_write = count < space ? count : space;
        size_t first = PTY_OUT_CAPACITY - p->out_head;
        if (first > to_write) first = to_write;
        memcpy(p->out_buf + p->out_head, src, first);
        memcpy(p->out_buf, src + first, to_write - first);

        p->out_head = (p->out_head + to_write) % PTY_OUT_CAPACITY;
        p->out_count += to_write;

        __atomic_store_n(&p->out_data_bytes, (uint32_t)p->out_count, __ATOMIC_RELEASE);
        __atomic_store_n(&p->out_space_bytes, (uint32_t)(PTY_OUT_CAPACITY - p->out_count), __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&p->lock, irq);

        sched_wake_all(&p->out_data_bytes);
        return (int64_t)to_write;
    }
}

int64_t pty_set_session(vfs_node_t *master_node, uint64_t child_pid) {
    if (!is_pty_master(master_node) || !child_pid) return SYSCALL_EINVAL;
    pty_t *p = master_node->fs_private;
    if (!p) return SYSCALL_EBADF;

    process_group_ref_t ref = {0};
    (void)process_group_acquire(child_pid, child_pid, &ref);

    uint64_t irq = spin_lock_irqsave(&p->lock);
    process_group_ref_t old = p->foreground;
    p->controlling_sid = child_pid;
    p->fg_pgid = child_pid;
    p->foreground = ref;
    spin_unlock_irqrestore(&p->lock, irq);

    if (old.generation) (void)process_group_release_ref(&old);
    return SYSCALL_SUCCESS;
}

int64_t pty_tcgetpgrp(vfs_node_t *slave_node, int64_t *out_pgid) {
    if (!is_pty_slave(slave_node) || !out_pgid) return SYSCALL_EINVAL;
    pty_t *p = slave_node->fs_private;
    if (!p) return SYSCALL_EBADF;

    tcb_t *curr = thread_current();
    if (!curr || !curr->is_user) return SYSCALL_ENOTTY;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    uint64_t csid = p->controlling_sid;
    uint64_t fg = p->fg_pgid;
    spin_unlock_irqrestore(&p->lock, irq);

    if (curr->sid != csid) return SYSCALL_ENOTTY;
    *out_pgid = (int64_t)fg;
    return SYSCALL_SUCCESS;
}

int64_t pty_tcsetpgrp(vfs_node_t *slave_node, uint64_t pgid) {
    if (!is_pty_slave(slave_node) || !pgid || pgid > 0x7fffffffffffffffULL) return SYSCALL_EINVAL;
    pty_t *p = slave_node->fs_private;
    if (!p) return SYSCALL_EBADF;

    tcb_t *curr = thread_current();
    if (!curr || !curr->is_user) return SYSCALL_ENOTTY;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    uint64_t csid = p->controlling_sid;
    uint64_t fg = p->fg_pgid;
    spin_unlock_irqrestore(&p->lock, irq);

    if (curr->sid != csid) return SYSCALL_ENOTTY;

    /* Background process guard (SIGTTOU):
     * If caller is in background, deliver SIGTTOU unless blocked or ignored. */
    if (curr->pgid != fg) {
        uint64_t unavailable = __atomic_load_n(&curr->signals.blocked_mask, __ATOMIC_ACQUIRE) |
                               __atomic_load_n(&curr->signals.ignored_mask, __ATOMIC_ACQUIRE);
        if (!(unavailable & SIGNAL_BIT(SIGTTOU))) {
            process_signal_send_group(curr->pgid, SIGTTOU);
            return SYSCALL_EINTR;
        }
    }

    process_group_ref_t ref = {0};
    int err = process_group_acquire(csid, pgid, &ref);
    if (err) return err;

    irq = spin_lock_irqsave(&p->lock);
    process_group_ref_t old = p->foreground;
    p->fg_pgid = pgid;
    p->foreground = ref;
    spin_unlock_irqrestore(&p->lock, irq);

    if (old.generation) (void)process_group_release_ref(&old);
    return SYSCALL_SUCCESS;
}

int64_t pty_set_winsize(vfs_node_t *node, uint16_t cols, uint16_t rows) {
    if (!node || !node->fs_private) return SYSCALL_EBADF;
    pty_t *p = node->fs_private;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    p->cols = cols;
    p->rows = rows;
    uint64_t target_group = p->fg_pgid ? p->fg_pgid : p->controlling_sid;
    spin_unlock_irqrestore(&p->lock, irq);

    if (target_group != 0) {
        process_signal_send_group(target_group, SIGWINCH);
    }
    return SYSCALL_SUCCESS;
}

int64_t pty_get_winsize(vfs_node_t *node, uint16_t *cols, uint16_t *rows) {
    if (!node || !node->fs_private || !cols || !rows) return SYSCALL_EBADF;
    pty_t *p = node->fs_private;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    *cols = p->cols ? p->cols : 80;
    *rows = p->rows ? p->rows : 24;
    spin_unlock_irqrestore(&p->lock, irq);
    return SYSCALL_SUCCESS;
}

int64_t pty_set_termattr(vfs_node_t *node, const terminal_attrs_t *attrs) {
    if (!node || !node->fs_private || !attrs) return SYSCALL_EBADF;
    pty_t *p = node->fs_private;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    p->attrs.input_flags = attrs->input_flags;
    if (attrs->vintr) p->attrs.vintr = attrs->vintr;
    if (attrs->vsusp) p->attrs.vsusp = attrs->vsusp;
    spin_unlock_irqrestore(&p->lock, irq);
    return SYSCALL_SUCCESS;
}

int64_t pty_get_termattr(vfs_node_t *node, terminal_attrs_t *attrs) {
    if (!node || !node->fs_private || !attrs) return SYSCALL_EBADF;
    pty_t *p = node->fs_private;

    uint64_t irq = spin_lock_irqsave(&p->lock);
    *attrs = p->attrs;
    spin_unlock_irqrestore(&p->lock, irq);
    return SYSCALL_SUCCESS;
}
