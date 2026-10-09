#include "pipe.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "thread.h"

_Static_assert(PIPE_CAPACITY == 16 * PAGE_SIZE, "pipe backing size");

typedef struct {
    pipe_t *pipe;
    size_t needed_space;
} pipe_wait_write_t;

/* Scheduler predicates run under rank 1: atomic observations only. */
static bool pipe_read_ready(void *arg) {
    pipe_t *p = arg;
    return __atomic_load_n(&p->data_bytes, __ATOMIC_ACQUIRE) > 0 ||
           __atomic_load_n(&p->writers, __ATOMIC_ACQUIRE) == 0;
}

static bool pipe_write_ready(void *arg) {
    pipe_wait_write_t *w = arg;
    return __atomic_load_n(&w->pipe->space_bytes, __ATOMIC_ACQUIRE) >= w->needed_space ||
           __atomic_load_n(&w->pipe->readers, __ATOMIC_ACQUIRE) == 0;
}

static void pipe_publish(pipe_t *p) {
    __atomic_store_n(&p->data_bytes, p->count, __ATOMIC_RELEASE);
    __atomic_store_n(&p->space_bytes, PIPE_CAPACITY - p->count, __ATOMIC_RELEASE);
}

static void profile_add(pipe_t *p, uint64_t *value, uint64_t n) {
    if (n > UINT64_MAX - *value) p->profile.valid = 0;
    else *value += n;
}

/* Called only with the existing IRQ-excluded pipe lock held. */
static void profile_transfer(pipe_t *p, bool write, size_t n, size_t before) {
    if (!p->profile_enabled) return;
    pipe_io_profile_t *s = &p->profile;
    profile_add(p, write ? &s->writes : &s->reads, 1);
    profile_add(p, write ? &s->write_bytes : &s->read_bytes, n);
    uint64_t *maximum = write ? &s->write_max : &s->read_max;
    if (n > *maximum) *maximum = n;
    uint64_t *bucket = write ? (n < 1024 ? &s->write_small : n == 1024 ? &s->write_1k : &s->write_large)
                             : (n < 1024 ? &s->read_small : n == 1024 ? &s->read_1k : &s->read_large);
    profile_add(p, bucket, 1);
    if (write && !before) profile_add(p, &s->empty_fills, 1);
    if (write && p->count == PIPE_CAPACITY) profile_add(p, &s->full_fills, 1);
    if (!write && !p->count) profile_add(p, &s->empty_drains, 1);
    uint8_t direction = write ? 2 : 1;
    tcb_t *self = thread_current();
    if (!self || self->current_cpu >= 64) { s->valid = 0; return; }
    uint8_t cpu = (uint8_t)self->current_cpu;
    if (write) s->writer_cpus |= 1ULL << cpu;
    else s->reader_cpus |= 1ULL << cpu;
    if (p->profile_direction) {
        if (direction != p->profile_direction) profile_add(p, &s->direction_changes, 1);
        if (cpu != p->profile_cpu) profile_add(p, &s->cpu_changes, 1);
    }
    p->profile_direction = direction;
    p->profile_cpu = cpu;
}

static int64_t pipe_read(vfs_node_t *node, uint64_t offset, void *buf, size_t count) {
    (void)offset;
    if (!count) return 0;
    pipe_t *p = node->fs_private;
    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (p->count) {
            size_t before = p->count;
            size_t n = count < p->count ? count : p->count;
            size_t first = PIPE_CAPACITY - p->tail;
            if (first > n) first = n;
            memcpy(buf, p->buffer + p->tail, first);
            memcpy((uint8_t *)buf + first, p->buffer, n - first);
            p->tail = (p->tail + n) % PIPE_CAPACITY;
            p->count -= n;
            profile_transfer(p, false, n, before);
            pipe_publish(p);
            spin_unlock_irqrestore(&p->lock, flags);
            sched_wake_all(&p->space_bytes);
            return (int64_t)n;
        }
        if (!__atomic_load_n(&p->writers, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }
        if (p->profile_enabled) profile_add(p, &p->profile.read_waits, 1);
        spin_unlock_irqrestore(&p->lock, flags);
        sched_wait_until(&p->data_bytes, pipe_read_ready, p);
    }
}

static int64_t pipe_write(vfs_node_t *node, uint64_t *offset, bool append,
                          const void *buf, size_t count) {
    (void)offset; (void)append;
    if (!count) return 0;
    pipe_t *p = node->fs_private;
    for (;;) {
        if (process_signal_interrupt()) return -VFS_EINTR;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (!__atomic_load_n(&p->readers, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -VFS_EPIPE;
        }
        size_t space = PIPE_CAPACITY - p->count;
        if (space && (count > PIPE_BUF || space >= count)) {
            size_t before = p->count;
            size_t n = count < space ? count : space;
            size_t first = PIPE_CAPACITY - p->head;
            if (first > n) first = n;
            memcpy(p->buffer + p->head, buf, first);
            memcpy(p->buffer, (const uint8_t *)buf + first, n - first);
            p->head = (p->head + n) % PIPE_CAPACITY;
            p->count += n;
            profile_transfer(p, true, n, before);
            pipe_publish(p);
            spin_unlock_irqrestore(&p->lock, flags);
            sched_wake_all(&p->data_bytes);
            return (int64_t)n;
        }
        pipe_wait_write_t wait = {p, count <= PIPE_BUF ? count : 1};
        if (p->profile_enabled) profile_add(p, &p->profile.write_waits, 1);
        spin_unlock_irqrestore(&p->lock, flags);
        sched_wait_until(&p->space_bytes, pipe_write_ready, &wait);
    }
}

int pipe_profile(vfs_node_t *node, uint64_t action, pipe_io_profile_t *out) {
    if (!node || !out || (node->read != pipe_read && node->write != pipe_write) ||
        !node->fs_private || action < PIPE_PROFILE_READ || action > PIPE_PROFILE_DISABLE)
        return -VFS_EINVAL;
    pipe_t *p = node->fs_private;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (action == PIPE_PROFILE_ENABLE) {
        memset(&p->profile, 0, sizeof(p->profile));
        p->profile.valid = 1;
        p->profile_direction = 0;
        p->profile_enabled = true;
    } else if (action == PIPE_PROFILE_DISABLE) p->profile_enabled = false;
    uint64_t fd = out->fd;
    *out = p->profile;
    out->fd = fd;
    spin_unlock_irqrestore(&p->lock, flags);
    return 0;
}

int pipe_create(vfs_node_t **out_read_node, vfs_node_t **out_write_node) {
    if (!out_read_node || !out_write_node || out_read_node == out_write_node)
        return -VFS_EINVAL;
    *out_read_node = NULL;
    *out_write_node = NULL;
    uintptr_t phys = pmm_alloc_pages(16);
    if (!phys) return -VFS_ENOMEM;
    pipe_t *p = kmalloc(sizeof(*p));
    if (!p) {
        pmm_free_pages(phys, 16);
        return -VFS_ENOMEM;
    }
    memset(p, 0, sizeof(*p));
    p->lock = (spinlock_t)SPINLOCK_RANKED(2, "pipe");
    p->buffer_phys = phys;
    p->buffer = vmm_phys_to_virt(phys);
    p->read_node = kmalloc(sizeof(vfs_node_t));
    p->write_node = kmalloc(sizeof(vfs_node_t));
    if (!p->read_node || !p->write_node) {
        kfree(p->read_node);
        kfree(p->write_node);
        kfree(p);
        pmm_free_pages(phys, 16);
        return -VFS_ENOMEM;
    }
    p->readers = p->writers = 1;
    p->active_endpoints = 2;
    p->data_bytes = 0;
    p->space_bytes = PIPE_CAPACITY;
    *p->read_node = (vfs_node_t){ .type = VFS_STREAM, .is_stream = true,
        .fs_private = p, .read = pipe_read, .close = pipe_close_endpoint };
    *p->write_node = (vfs_node_t){ .type = VFS_STREAM, .is_stream = true,
        .fs_private = p, .write = pipe_write, .close = pipe_close_endpoint };
    *out_read_node = p->read_node;
    *out_write_node = p->write_node;
    return 0;
}

void pipe_close_endpoint(vfs_node_t *node) {
    pipe_t *p = node->fs_private;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (node == p->read_node)
        __atomic_sub_fetch(&p->readers, 1, __ATOMIC_RELEASE);
    else
        __atomic_sub_fetch(&p->writers, 1, __ATOMIC_RELEASE);
    node->fs_private = NULL;
    spin_unlock_irqrestore(&p->lock, flags);
    /* Retain this endpoint's lifetime reference until the wake completes. */
    sched_wake_all(&p->data_bytes);
    sched_wake_all(&p->space_bytes);
    if (__atomic_sub_fetch(&p->active_endpoints, 1, __ATOMIC_ACQ_REL) == 0) {
        pmm_free_pages(p->buffer_phys, 16);
        kfree(p->read_node);
        kfree(p->write_node);
        kfree(p);
    }
}
