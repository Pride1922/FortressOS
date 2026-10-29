#include "fb.h"
#include "boot_info.h"
#include "vmm.h"
#include "elf.h"
#include "console.h"
#include "serial.h"
#include "spinlock.h"

/* Rank 1 is held only for state publication; never across VMM, console or I/O.
 * Root identity cannot be reused until the VMM teardown callback completes.
 * BUSY serializes map/release; TRANSITION excludes acquire during console work. */
static spinlock_t fb_lock=SPINLOCK_RANKED(1,"display_owner");
enum { FB_FREE, FB_TRANSITION, FB_OWNED, FB_BUSY };
static unsigned state;
static uintptr_t owner, mapped_va, mapped_pa;
static size_t mapped_bytes;
static uint64_t *mapped_root;

static bool geometry(fb_info_v1_t *info, uintptr_t *pa) {
    const boot_info_t *b=boot_info_get();
    if (!b || !b->has_framebuffer || !b->fb_width || !b->fb_height
        || b->fb_width>8192 || b->fb_height>8192 || b->fb_bpp!=32
        || b->fb_pitch%4 || b->fb_pitch<b->fb_width*4
        || b->fb_pitch>(32*1024*1024ULL)/b->fb_height
        || b->fb_address<b->hhdm_offset) return false;
    *pa=b->fb_address-b->hhdm_offset;
    size_t bytes=(b->fb_pitch*b->fb_height+4095)&~4095ULL;
    if (!*pa || *pa%4096 || *pa>PTE_ADDR_MASK || bytes-1>(PTE_ADDR_MASK|4095)-*pa) return false;
    *info=(fb_info_v1_t){.version=1,.width=b->fb_width,.height=b->fb_height,
        .pitch=b->fb_pitch,.bpp=32,.fb_size=bytes};
    return true;
}

int64_t fb_acquire(uintptr_t cr3) {
    fb_info_v1_t info; uintptr_t pa;
    if (!cr3 || !geometry(&info,&pa)) return SYSCALL_ENODEV;
    uint64_t irq=spin_lock_irqsave(&fb_lock);
    if (state!=FB_FREE) { spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EBUSY; }
    owner=cr3; state=FB_TRANSITION;
    spin_unlock_irqrestore(&fb_lock,irq);
    console_graphics_mode(true);
    irq=spin_lock_irqsave(&fb_lock); state=FB_OWNED;
    spin_unlock_irqrestore(&fb_lock,irq);
    return 0;
}

int64_t fb_map(uintptr_t cr3, uint64_t *root, uintptr_t requested, fb_info_v1_t *out) {
    fb_info_v1_t info; uintptr_t pa;
    if (!geometry(&info,&pa)) return SYSCALL_ENODEV;
    uintptr_t va=requested ? requested : 0x6000000000ULL;
    if (va%4096 || va<0x1000000ULL || va>=USER_SIGRESTORER_VIRT
        || info.fb_size>USER_SIGRESTORER_VIRT-va) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&fb_lock);
    if (!owner || owner!=cr3) { spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EPERM; }
    if (state!=FB_OWNED) { spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EBUSY; }
    if (mapped_bytes) {
        int64_t result=(va==mapped_va && root==mapped_root) ? 0 : SYSCALL_EBUSY;
        if (!result) { info.fb_user_vaddr=va; *out=info; }
        spin_unlock_irqrestore(&fb_lock,irq); return result;
    }
    state=FB_BUSY;
    spin_unlock_irqrestore(&fb_lock,irq);
    int error=vmm_map_framebuffer_range(root,va,pa,info.fb_size);
    irq=spin_lock_irqsave(&fb_lock);
    if (error==VMM_OK) {
        mapped_va=va; mapped_pa=pa; mapped_root=root; mapped_bytes=info.fb_size;
        info.fb_user_vaddr=va; *out=info;
    }
    state=FB_OWNED;
    spin_unlock_irqrestore(&fb_lock,irq);
    return error==VMM_OK ? 0 : error==VMM_ERR_ALREADY_MAPPED ? SYSCALL_EEXIST :
        error==VMM_ERR_NOMEM ? SYSCALL_ENOMEM : SYSCALL_EINVAL;
}

int64_t fb_release(uintptr_t cr3) {
    uint64_t irq=spin_lock_irqsave(&fb_lock);
    if (!owner || owner!=cr3) { spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EPERM; }
    if (state!=FB_OWNED) { spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EBUSY; }
    state=FB_BUSY;
    spin_unlock_irqrestore(&fb_lock,irq);
    int error=mapped_bytes ? vmm_unmap_device_range(mapped_root,mapped_va,mapped_pa,mapped_bytes) : VMM_OK;
    if (error!=VMM_OK) {
        irq=spin_lock_irqsave(&fb_lock); state=FB_OWNED;
        spin_unlock_irqrestore(&fb_lock,irq); return SYSCALL_EIO;
    }
    /* Synchronous invalidation has completed before text or another owner draws. */
    console_graphics_mode(false);
    irq=spin_lock_irqsave(&fb_lock);
    owner=0; mapped_bytes=0; mapped_root=NULL; state=FB_FREE;
    spin_unlock_irqrestore(&fb_lock,irq);
    return 0;
}

void fb_space_destroyed(uintptr_t cr3) {
    uint64_t irq=spin_lock_irqsave(&fb_lock);
    if (owner!=cr3 || !owner) { spin_unlock_irqrestore(&fb_lock,irq); return; }
    state=FB_TRANSITION;
    spin_unlock_irqrestore(&fb_lock,irq);
    /* VMM calls only after all sched/CPU/op refs drain and leaves are removed. */
    console_graphics_mode(false);
    serial_raw_puts("[CONSOLE] Display owner exited, restoring text mode\n");
    irq=spin_lock_irqsave(&fb_lock);
    owner=0; mapped_bytes=0; mapped_root=NULL; state=FB_FREE;
    spin_unlock_irqrestore(&fb_lock,irq);
}
