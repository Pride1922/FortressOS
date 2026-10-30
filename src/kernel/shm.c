#include "shm.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "thread.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "elf.h"
#include "string.h"
#include "spinlock.h"

typedef struct {
    uintptr_t root_phys;
    uintptr_t va;
    uint64_t token; /* identifies one reservation; 0 = free slot */
    bool detaching; /* unmap in progress: slot stays reserved, not findable */
} shm_mapping_t;

typedef struct shm {
    vfs_node_t node; /* embedded: node->fs_private == this */
    size_t pages;
    uintptr_t *frames;
    unsigned refs;   /* 1 while the node is open, +1 per live/pending mapping */
    shm_mapping_t maps[SHM_MAX_MAPPINGS];
    struct shm *next;
} shm_t;

/* Rank 1: state publication only. Never held across VMM, PMM, heap, TLB
 * shootdown or user memory access. */
static spinlock_t shm_lock = SPINLOCK_RANKED(1, "shm");
static shm_t *objects;
static size_t total_pages;
static uint64_t next_token = 1;

static void shm_close(vfs_node_t *node);

static void destroy(shm_t *s) {
    /* refs==0 and unlinked: no mapping exists and no CPU translates it. */
    for (size_t i = 0; i < s->pages; i++) pmm_free_page(s->frames[i]);
    uint64_t irq = spin_lock_irqsave(&shm_lock);
    total_pages -= s->pages;
    spin_unlock_irqrestore(&shm_lock, irq);
    kfree(s->frames);
    kfree(s);
}

/* Under shm_lock. True when the caller must destroy() after unlocking. */
static bool put_locked(shm_t *s) {
    if (--s->refs) return false;
    for (shm_t **p = &objects; *p; p = &(*p)->next) {
        if (*p == s) { *p = s->next; break; }
    }
    return true;
}

static shm_t *shm_from_fd(int fd, file_t **out) {
    tcb_t *self = thread_current();
    if (!self || fd < 0 || fd >= MAX_PROCESS_FDS) return NULL;
    file_t *file = fd_get(self, fd);
    /* Operation reference: a sibling thread may close the fd meanwhile. */
    if (!file || !vfs_file_get(file)) return NULL;
    if (!file->node || file->node->close != shm_close) { vfs_close(file); return NULL; }
    *out = file;
    return file->node->fs_private;
}

int64_t shm_create(uint64_t bytes, uint32_t flags) {
    tcb_t *self = thread_current();
    if (!self || !self->is_user) return SYSCALL_EPERM;
    if (!bytes || bytes > SHM_MAX_BYTES || (flags & ~VFS_O_CLOEXEC)) return SYSCALL_EINVAL;
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t irq = spin_lock_irqsave(&shm_lock);
    bool fits = total_pages + pages <= SHM_TOTAL_BYTES_MAX / PAGE_SIZE;
    if (fits) total_pages += pages;
    spin_unlock_irqrestore(&shm_lock, irq);
    if (!fits) return SYSCALL_ENOMEM;

    shm_t *s = kmalloc(sizeof(*s));
    uintptr_t *frames = kmalloc(pages * sizeof(*frames));
    file_t *file = kmalloc(sizeof(*file));
    size_t got = 0;
    if (s && frames && file) {
        for (; got < pages; got++) {
            frames[got] = pmm_alloc_page();
            if (!frames[got]) break;
            memset(vmm_phys_to_virt(frames[got]), 0, PAGE_SIZE);
        }
    }
    if (!s || !frames || !file || got != pages) {
        for (size_t i = 0; i < got; i++) pmm_free_page(frames[i]);
        kfree(file); kfree(frames); kfree(s);
        irq = spin_lock_irqsave(&shm_lock);
        total_pages -= pages;
        spin_unlock_irqrestore(&shm_lock, irq);
        return SYSCALL_ENOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->pages = pages;
    s->frames = frames;
    s->refs = 1;
    s->node = (vfs_node_t){ .type = VFS_STREAM, .mode = 0600, .is_stream = true,
        .size = pages * PAGE_SIZE, .fs_private = s, .close = shm_close };
    *file = (file_t){ .node = &s->node, .flags = VFS_O_RDWR, .ref_count = 1 };

    irq = spin_lock_irqsave(&shm_lock);
    s->next = objects;
    objects = s;
    spin_unlock_irqrestore(&shm_lock, irq);

    int fd = fd_alloc(self, file);
    if (fd < 0) { vfs_close(file); return SYSCALL_EMFILE; } /* closes s */
    if (flags & VFS_O_CLOEXEC) {
        self->fd_flags[fd] = FD_FLAG_CLOEXEC;
        if (self->process) self->process->fd_flags[fd] = FD_FLAG_CLOEXEC;
    }
    return fd;
}

int64_t shm_map(int fd, uintptr_t va, uint32_t flags) {
    tcb_t *self = thread_current();
    if (!self || !self->is_user) return SYSCALL_EPERM;
    if (flags & ~SHM_MAP_READONLY) return SYSCALL_EINVAL;
    file_t *file;
    shm_t *s = shm_from_fd(fd, &file);
    if (!s) return SYSCALL_EBADF;
    size_t bytes = s->pages * PAGE_SIZE;
    if (va % PAGE_SIZE || va < 0x1000000ULL || va >= USER_SIGRESTORER_VIRT ||
        bytes > USER_SIGRESTORER_VIRT - va) { vfs_close(file); return SYSCALL_EINVAL; }
    uint64_t *root = vmm_get_active_pml4_virt();
    /* Same identity vmm teardown passes to shm_space_destroyed(). */
    uintptr_t root_phys = vmm_get_current_pml4() & PTE_ADDR_MASK;

    /* Reserve the mapping reference first: frames stay alive even if the
     * descriptor is closed by another thread while the VMM maps them. */
    uint64_t irq = spin_lock_irqsave(&shm_lock);
    shm_mapping_t *slot = NULL;
    for (unsigned i = 0; i < SHM_MAX_MAPPINGS; i++)
        if (!s->maps[i].token) { slot = &s->maps[i]; break; }
    uint64_t token = 0;
    if (slot) {
        token = next_token++;
        *slot = (shm_mapping_t){ .root_phys = root_phys, .va = va, .token = token };
        s->refs++;
    }
    spin_unlock_irqrestore(&shm_lock, irq);
    if (!slot) { vfs_close(file); return SYSCALL_EBUSY; }

    int error = vmm_map_frames(root, va, s->frames, s->pages, !(flags & SHM_MAP_READONLY));
    if (error != VMM_OK) {
        irq = spin_lock_irqsave(&shm_lock);
        for (unsigned i = 0; i < SHM_MAX_MAPPINGS; i++)
            if (s->maps[i].token == token) s->maps[i].token = 0;
        bool dead = put_locked(s); /* never true: the op reference holds the file */
        spin_unlock_irqrestore(&shm_lock, irq);
        if (dead) destroy(s);
        vfs_close(file);
        return error == VMM_ERR_ALREADY_MAPPED ? SYSCALL_EEXIST :
               error == VMM_ERR_NOMEM ? SYSCALL_ENOMEM : SYSCALL_EINVAL;
    }
    vfs_close(file);
    return (int64_t)va;
}

int64_t shm_unmap(uintptr_t va) {
    tcb_t *self = thread_current();
    if (!self || !self->is_user) return SYSCALL_EPERM;
    uint64_t *root = vmm_get_active_pml4_virt();
    /* Same identity vmm teardown passes to shm_space_destroyed(). */
    uintptr_t root_phys = vmm_get_current_pml4() & PTE_ADDR_MASK;
    /* Mark the record detaching so no second unmap can claim it and no map
     * can reuse the slot; its reference keeps the frames alive meanwhile. */
    uint64_t irq = spin_lock_irqsave(&shm_lock);
    shm_t *s = NULL;
    unsigned index = 0;
    for (shm_t *o = objects; o && !s; o = o->next)
        for (unsigned i = 0; i < SHM_MAX_MAPPINGS; i++)
            if (o->maps[i].token && !o->maps[i].detaching &&
                o->maps[i].root_phys == root_phys && o->maps[i].va == va) {
                s = o; index = i;
                o->maps[i].detaching = true;
                break;
            }
    spin_unlock_irqrestore(&shm_lock, irq);
    if (!s) return SYSCALL_EINVAL;

    int error = vmm_unmap_frames(root, va, s->frames, s->pages);
    irq = spin_lock_irqsave(&shm_lock);
    bool dead = false;
    s->maps[index].detaching = false;
    if (error == VMM_OK) {
        s->maps[index].token = 0;
        dead = put_locked(s);
    } /* else still mapped (e.g. its map is still in flight): keep the record */
    spin_unlock_irqrestore(&shm_lock, irq);
    if (dead) destroy(s);
    return error == VMM_OK ? 0 : SYSCALL_EINVAL;
}

static void shm_close(vfs_node_t *node) {
    shm_t *s = node->fs_private;
    uint64_t irq = spin_lock_irqsave(&shm_lock);
    bool dead = put_locked(s);
    spin_unlock_irqrestore(&shm_lock, irq);
    if (dead) destroy(s);
}

void shm_space_destroyed(uintptr_t root_phys) {
    /* Every leaf is already cleared and no CPU uses the root; only the
     * mapping references remain to be dropped. */
    for (;;) {
        shm_t *dead = NULL;
        uint64_t irq = spin_lock_irqsave(&shm_lock);
        for (shm_t *o = objects; o && !dead; o = o->next) {
            for (unsigned i = 0; i < SHM_MAX_MAPPINGS; i++) {
                if (!o->maps[i].token || o->maps[i].root_phys != root_phys) continue;
                o->maps[i].token = 0;
                if (put_locked(o)) { dead = o; break; }
            }
        }
        spin_unlock_irqrestore(&shm_lock, irq);
        if (!dead) return;
        destroy(dead); /* the list changed: rescan from the start */
    }
}
