/* Actual pipe/VFS/sys_pipe code with single-threaded host allocator, fd and
 * user-page-validation adapters. Does not establish IRQ or SMP behavior. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOST_THREAD_H
#include "../src/kernel/thread.h"
#include "../src/fs/pipe.c"
#include "../src/fs/vfs.c"
#include "../src/kernel/syscall.c"

static size_t heap_live, pages_live, alloc_calls;
static int fail_after = -1;
static bool fail_pages, valid_range = true;
static void *backing;
static tcb_t current;
static int fd_fail_after = -1;
static void (*wait_step)(const void *, bool (*)(void *), void *);
static unsigned waits, wakes;

void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    spin_debug_assert_unheld();
    assert(!ready(arg) && wait_step);
    waits++;
    wait_step(channel, ready, arg);
    assert(ready(arg));
    wait_step = NULL;
}
void sched_wake_all(const void *channel) {
    spin_debug_assert_unheld();
    const pipe_t *p = channel;
    assert(p->active_endpoints > 0 && pages_live == 16);
    assert(p->data_bytes == p->count && p->space_bytes == PIPE_CAPACITY - p->count);
    wakes++;
}

int64_t input_read(void *buf, size_t n) { (void)buf; (void)n; assert(0); return 0; }
bool console_is_quiet(void) { return true; }
void console_terminal_write(const char *buf, size_t n) { (void)buf; (void)n; assert(0); }
void serial_raw_putc(char c) { (void)c; assert(0); }

void *kmalloc(size_t n) {
    alloc_calls++;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(n);
    assert(p);
    heap_live++;
    return p;
}
void kfree(void *p) { if (p) { assert(heap_live); heap_live--; free(p); } }
uintptr_t pmm_alloc_pages(size_t n) {
    assert(n == 16 && !pages_live);
    if (fail_pages) return 0;
    backing = malloc(PIPE_CAPACITY);
    assert(backing);
    pages_live = n;
    return 0x100000; /* Deliberately distinct from the host virtual address. */
}
void *vmm_phys_to_virt(uintptr_t phys) { assert(phys == 0x100000); return backing; }
void pmm_free_pages(uintptr_t phys, size_t n) {
    assert(phys == 0x100000 && n == 16 && pages_live == 16);
    free(backing); backing = NULL; pages_live = 0;
}
uint64_t *vmm_get_active_pml4_virt(void) { return (uint64_t *)&current; }
bool vmm_validate_user_range(uint64_t *pml4, uintptr_t addr, size_t n, bool write) {
    assert(pml4 == (uint64_t *)&current && addr >= 0x1000 && n == 8 && write);
    return valid_range;
}
tcb_t *thread_current(void) { return &current; }
int fd_alloc(tcb_t *p, struct file *f) {
    if (fd_fail_after == 0) return -1;
    if (fd_fail_after > 0) fd_fail_after--;
    for (int i = 0; i < MAX_PROCESS_FDS; i++) if (!p->fd_table[i]) {
        p->fd_table[i] = f; p->fd_flags[i] = 0; return i;
    }
    return -1;
}
int fd_free(tcb_t *p, int fd) {
    assert(p->fd_table[fd]);
    vfs_close(p->fd_table[fd]); p->fd_table[fd] = NULL; p->fd_flags[fd] = 0;
    return 0;
}

static void clean(void) { assert(heap_live == 0 && pages_live == 0); }
static void mirrors(pipe_t *p) {
    assert(p->data_bytes == p->count && p->space_bytes == PIPE_CAPACITY - p->count);
    assert(p->head < PIPE_CAPACITY && p->tail < PIPE_CAPACITY);
}
static void pair(file_t **r, file_t **w) {
    vfs_node_t *rn, *wn;
    assert(pipe_create(&rn, &wn) == 0);
    assert(rn->is_stream && wn->is_stream && rn->type == VFS_STREAM && wn->type == VFS_STREAM);
    assert(!rn->write && !wn->read && rn->close && wn->close);
    *r = kmalloc(sizeof(**r)); *w = kmalloc(sizeof(**w));
    **r = (file_t){ .node = rn, .flags = VFS_O_RDONLY, .ref_count = 1 };
    **w = (file_t){ .node = wn, .flags = VFS_O_WRONLY, .ref_count = 1 };
}

static void ring_tests(void) {
    static uint8_t in[PIPE_CAPACITY + 100], out[PIPE_CAPACITY + 100];
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i * 37 + i / 251);
    file_t *r, *w; pair(&r, &w);
    pipe_t *p = r->node->fs_private;
    assert(!pipe_read_ready(p));
    assert(vfs_read(r, NULL, 0) == 0 && vfs_write(w, NULL, 0) == 0);
    assert(vfs_read(w, out, 1) == -VFS_EBADF);
    assert(vfs_write(r, in, 1) == -VFS_EBADF);
    /* Put both indices near the end, then atomically wrap a PIPE_BUF write. */
    assert(vfs_write(w, in, PIPE_CAPACITY - 100) == PIPE_CAPACITY - 100);
    assert(vfs_read(r, out, sizeof(out)) == PIPE_CAPACITY - 100);
    assert(!memcmp(in, out, PIPE_CAPACITY - 100));
    assert(vfs_write(w, in, PIPE_BUF) == PIPE_BUF);
    assert(vfs_read(r, out, sizeof(out)) == PIPE_BUF);
    assert(!memcmp(in, out, PIPE_BUF)); mirrors(p);
    /* A short free region cannot accept any prefix of an atomic write. */
    assert(vfs_write(w, in, PIPE_CAPACITY - PIPE_BUF + 1) == PIPE_CAPACITY - PIPE_BUF + 1);
    size_t head = p->head, count = p->count;
    pipe_wait_write_t pending = {p, PIPE_BUF};
    assert(!pipe_write_ready(&pending));
    assert(p->head == head && p->count == count); mirrors(p);
    assert(vfs_write(w, in, PIPE_BUF - 1) == PIPE_BUF - 1);
    pending.needed_space = 1;
    assert(!pipe_write_ready(&pending));
    assert(vfs_read(r, out, 17) == 17);
    assert(!memcmp(out, in, 17));
    assert(vfs_write(w, in, PIPE_BUF + 1) == 17); mirrors(p);
    assert(vfs_read(r, out, sizeof(out)) == PIPE_CAPACITY);
    assert(!memcmp(out, in + 17, PIPE_CAPACITY - PIPE_BUF + 1 - 17));
    assert(!memcmp(out + PIPE_CAPACITY - PIPE_BUF + 1 - 17, in, PIPE_BUF - 1));
    assert(!memcmp(out + PIPE_CAPACITY - 17, in, 17));
    assert(vfs_write(w, in, sizeof(in)) == PIPE_CAPACITY);
    __atomic_fetch_add(&w->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_close(w); assert(p->writers == 1);
    vfs_close(w); assert(p->writers == 0);
    assert(vfs_read(r, out, sizeof(out)) == PIPE_CAPACITY);
    assert(!memcmp(in, out, PIPE_CAPACITY));
    assert(vfs_read(r, out, 1) == 0);
    assert(r->offset == 0); vfs_close(r); clean();
    pair(&r, &w); p = w->node->fs_private;
    __atomic_fetch_add(&r->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_close(r); assert(p->readers == 1);
    vfs_close(r);
    assert(vfs_write(w, in, 1) == -VFS_EPIPE);
    assert(vfs_write(w, NULL, 0) == 0);
    vfs_close(w); clean();
}

static file_t *wait_reader, *wait_writer;
static unsigned wait_kind;
static void release_wait(const void *channel, bool (*ready)(void *), void *arg) {
    pipe_t *p = (pipe_t *)channel;
    static unsigned char scratch[PIPE_BUF];
    if (wait_kind == 0) {
        assert(vfs_write(wait_writer, "X", 1) == 1);
    } else if (wait_kind == 1) {
        vfs_close(wait_writer);
    } else if (wait_kind == 2) {
        /* 1 byte free is insufficient for a 4096-byte transaction. Even after
         * a wake with 4095 free, its predicate must keep the writer asleep. */
        assert(p->count == PIPE_CAPACITY - 1);
        size_t head = p->head;
        assert(vfs_read(wait_reader, scratch, PIPE_BUF - 2) == PIPE_BUF - 2);
        assert(!ready(arg) && p->head == head);
        assert(vfs_read(wait_reader, scratch, 1) == 1);
    } else if (wait_kind == 3) {
        assert(vfs_read(wait_reader, scratch, 17) == 17);
    } else {
        vfs_close(wait_reader);
    }
}
static void blocking_tests(void) {
    static unsigned char data[PIPE_CAPACITY];
    for (wait_kind = 0; wait_kind < 5; wait_kind++) {
        pair(&wait_reader, &wait_writer);
        if (wait_kind >= 2)
            assert(vfs_write(wait_writer, data, wait_kind == 2 ? PIPE_CAPACITY - 1 : PIPE_CAPACITY) ==
                   (wait_kind == 2 ? PIPE_CAPACITY - 1 : PIPE_CAPACITY));
        wait_step = release_wait;
        if (wait_kind < 2) {
            unsigned char c;
            assert(vfs_read(wait_reader, &c, 1) == (wait_kind == 0 ? 1 : 0));
            if (wait_kind == 0) assert(c == 'X');
        } else {
            size_t n = wait_kind == 2 ? PIPE_BUF : PIPE_BUF + 1;
            assert(vfs_write(wait_writer, data, n) ==
                   (wait_kind == 2 ? PIPE_BUF : wait_kind == 3 ? 17 : -VFS_EPIPE));
        }
        if (wait_kind != 1) vfs_close(wait_writer);
        if (wait_kind != 4) vfs_close(wait_reader);
        clean();
    }
    assert(waits == 5 && wakes > 5);
}

static void syscall_tests(void) {
    int fds[2] = {-99, -99};
    assert(sys_pipe(0, 0) == SYSCALL_EFAULT);
    assert(sys_pipe(0xfff, 0) == SYSCALL_EFAULT);
    assert(sys_pipe(0x800000000000ULL - 7, 0) == SYSCALL_EFAULT);
    assert(sys_pipe(UINTPTR_MAX - 3, 0) == SYSCALL_EFAULT);
    valid_range = false;
    assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_EFAULT);
    valid_range = true;
    assert(sys_pipe((uintptr_t)fds, VFS_O_RDWR) == SYSCALL_EINVAL);
    file_t dummy = {0};
    for (int i = 0; i < MAX_PROCESS_FDS; i++) current.fd_table[i] = &dummy;
    size_t before = alloc_calls;
    assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_EMFILE);
    current.fd_table[7] = NULL;
    assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_EMFILE);
    assert(alloc_calls == before && current.fd_table[7] == NULL);
    memset(&current, 0, sizeof(current));
    fail_pages = true;
    assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_ENOMEM); clean();
    fail_pages = false;
    for (int i = 0; i < 5; i++) {
        fail_after = i;
        assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_ENOMEM);
        assert(fds[0] == -99 && fds[1] == -99); clean();
    }
    fail_after = -1;
    for (int i = 0; i < 2; i++) {
        fd_fail_after = i;
        assert(sys_pipe((uintptr_t)fds, 0) == SYSCALL_EMFILE);
        assert(!current.fd_table[0] && !current.fd_table[1]); clean();
    }
    fd_fail_after = -1;
    for (int cloexec = 0; cloexec < 2; cloexec++) {
        assert(sys_pipe((uintptr_t)fds, cloexec ? VFS_O_CLOEXEC : 0) == 0);
        assert(fds[0] == 0 && fds[1] == 1);
        assert(current.fd_flags[0] == (unsigned)cloexec && current.fd_flags[1] == (unsigned)cloexec);
        assert(current.fd_table[0]->node->read && current.fd_table[1]->node->write);
        fd_free(&current, 0); fd_free(&current, 1); clean();
    }
    assert(syscall_from_vfs_error(-VFS_EPIPE) == SYSCALL_EPIPE);
    assert(syscall_from_vfs_error(-VFS_EAGAIN) == SYSCALL_EAGAIN);
}

int main(void) {
    ring_tests(); blocking_tests(); syscall_tests(); clean();
    puts("pipe host: ring, atomic boundaries, lifetime, syscall validation and rollback PASS");
    return 0;
}
