#include "pmm.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"
#include "vmm.h"


static uint8_t   *bitmap = NULL;
static uintptr_t  bitmap_phys_addr = 0;
static size_t     bitmap_total_pages = 0;
static size_t     total_pages = 0;
static size_t     used_pages = 0;
static size_t     free_pages = 0;
static size_t     last_allocated_index = 0;
static size_t     alloc_limit_pages = 0;
static bool       high_memory_enabled = false;
/* Boot-only reservation ownership, distinct from transient allocations.
 * Static kernel BSS (1 MiB at 32 GiB capacity); never reclaimable via PMM. */
static uint8_t reserved_bitmap[PMM_BITMAP_CAPACITY_BYTES];
static uint64_t rejected_frees, allocation_failures, max_scan_steps;
static spinlock_t g_pmm_lock = SPINLOCK_RANKED(4, "pmm");

bool pmm_snapshot(void *buffer, size_t capacity) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    size_t bytes = (total_pages + 7) / 8;
    bool ok = buffer && bitmap && capacity >= bytes;
    if (ok) {
        memset(buffer, 0, capacity);
        memcpy(buffer, bitmap, bytes);
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return ok;
}

static inline void bitmap_set(size_t frame_idx) {
    bitmap[frame_idx / 8] |= (uint8_t)(1 << (frame_idx % 8));
}

static inline void bitmap_clear(size_t frame_idx) {
    bitmap[frame_idx / 8] &= (uint8_t)~(1 << (frame_idx % 8));
}

static inline int bitmap_test(size_t frame_idx) {
    return (bitmap[frame_idx / 8] >> (frame_idx % 8)) & 1;
}

static bool frame_reserved(size_t idx) {
    return (reserved_bitmap[idx / 8] & (1U << (idx % 8))) != 0;
}

static void record_scan(size_t steps) {
    if (steps > max_scan_steps) max_scan_steps = steps;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    if (!memmap || !memmap->entries || memmap->entry_count == 0) {
        serial_puts("[FAIL] PMM: Invalid or missing Limine memory map\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* 1. Calculate highest usable physical memory address */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (!entry) continue;
        if (entry->type == LIMINE_MEMMAP_USABLE ||
            entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (entry->length > UINT64_MAX - entry->base) continue;
            uint64_t top = entry->base + entry->length;
            if (top > highest_addr) {
                highest_addr = top;
            }
        }
    }

    if (highest_addr > PMM_BITMAP_MAX_RAM_BYTES) {
        serial_puts("[WARN] PMM: memory map reports RAM above bitmap capacity; clamping to ");
        serial_print_dec(PMM_BITMAP_MAX_RAM_BYTES / (1024ULL * 1024 * 1024));
        serial_puts(" GiB\n");
        highest_addr = PMM_BITMAP_MAX_RAM_BYTES;
    }
    total_pages = (size_t)(highest_addr / PAGE_SIZE);
    if (total_pages == 0) {
        serial_puts("[FAIL] PMM: No managed physical pages\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    alloc_limit_pages = total_pages;
    if (alloc_limit_pages > PMM_BOOT_ALLOC_LIMIT / PAGE_SIZE)
        alloc_limit_pages = PMM_BOOT_ALLOC_LIMIT / PAGE_SIZE;
    __atomic_store_n(&high_memory_enabled, false, __ATOMIC_RELEASE);
    used_pages  = total_pages;
    free_pages  = 0;

    /* 2. Calculate bitmap size in bytes, aligned up to 4 KiB */
    size_t bitmap_size = (total_pages + 7) / 8;
    bitmap_size = (bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t bitmap_pages = bitmap_size / PAGE_SIZE;

    /* Empirical Dell H5: early HHDM coverage is limited. The ENTIRE bitmap
     * must fit below the conservative 1 GiB ceiling, not just its first byte. */
    uintptr_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (!entry || entry->type != LIMINE_MEMMAP_USABLE ||
            entry->base >= alloc_limit_pages * PAGE_SIZE ||
            entry->length > UINT64_MAX - entry->base) continue;
        uint64_t end = entry->base + entry->length;
        if (end > alloc_limit_pages * PAGE_SIZE) end = alloc_limit_pages * PAGE_SIZE;
        uint64_t start = entry->base < 0x100000 ? 0x100000 : entry->base;
        start = ALIGN_UP(start, PAGE_SIZE);
        if (start <= end && bitmap_size <= end - start) {
            bitmap_phys = start;
            break;
        }
    }

    if (bitmap_phys == 0) {
        serial_puts("[FAIL] PMM: Could not find usable memory for bitmap!\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* 4. Set virtual address of bitmap using Limine HHDM */
    bitmap = (uint8_t *)(bitmap_phys + hhdm_offset);
    bitmap_phys_addr = bitmap_phys;
    bitmap_total_pages = bitmap_pages;

    /* 5. Initialize bitmap to all 1s (all memory starts as reserved/used) */
    for (size_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }

    /* 6. Free all frames within LIMINE_MEMMAP_USABLE regions */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry && entry->type == LIMINE_MEMMAP_USABLE &&
            entry->base < total_pages * PAGE_SIZE &&
            entry->length <= UINT64_MAX - entry->base) {
            uint64_t end = entry->base + entry->length;
            if (end > total_pages * PAGE_SIZE) end = total_pages * PAGE_SIZE;
            size_t start_frame = (size_t)(ALIGN_UP(entry->base, PAGE_SIZE) / PAGE_SIZE);
            size_t end_frame = (size_t)(end / PAGE_SIZE);

            for (size_t frame = start_frame; frame < end_frame; frame++) {
                if (bitmap_test(frame)) {
                    bitmap_clear(frame);
                    used_pages--;
                    free_pages++;
                }
            }
        }
    }

    /* 7. Protect physical frame 0 (null address safety) */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        used_pages++;
        free_pages--;
    }

    /* 8. Protect the bitmap's own physical frames */
    size_t bm_start_frame = (size_t)(bitmap_phys / PAGE_SIZE);
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t frame = bm_start_frame + i;
        if (frame < total_pages && !bitmap_test(frame)) {
            bitmap_set(frame);
            used_pages++;
            free_pages--;
        }
    }

    last_allocated_index = 0;
    /* Snapshot immutable reservations only after frame zero and metadata have
     * been pinned. No concurrent users may exist during initialization. */
    memcpy(reserved_bitmap, bitmap, bitmap_size);
    rejected_frees = allocation_failures = max_scan_steps = 0;

    /* 9. Output diagnostics */
    serial_puts("[ OK ] PMM initialized:\n");
    serial_puts("       Total Physical RAM:  ");
    serial_print_dec((total_pages * PAGE_SIZE) / (1024 * 1024));
    serial_puts(" MiB (");
    serial_print_dec(total_pages);
    serial_puts(" frames)\n");

    serial_puts("       Usable Free RAM:     ");
    serial_print_dec((free_pages * PAGE_SIZE) / (1024 * 1024));
    serial_puts(" MiB (");
    serial_print_dec(free_pages);
    serial_puts(" frames)\n");

    serial_puts("       Used/Reserved RAM:   ");
    serial_print_dec((used_pages * PAGE_SIZE) / (1024 * 1024));
    serial_puts(" MiB (");
    serial_print_dec(used_pages);
    serial_puts(" frames)\n");

    serial_puts("       Bitmap Location:     Phys ");
    serial_print_hex(bitmap_phys);
    serial_puts(" (");
    serial_print_dec(bitmap_size / 1024);
    serial_puts(" KiB)\n\n");
    serial_puts("[PMM] Boot allocation ceiling: 1 GiB (all allocation APIs)\n");
}

bool pmm_high_memory_enabled(void) {
    return __atomic_load_n(&high_memory_enabled, __ATOMIC_ACQUIRE);
}

bool pmm_unlock_high_memory(void) {
    /* Idempotent: if already unlocked, just return true. */
    if (__atomic_load_n(&high_memory_enabled, __ATOMIC_ACQUIRE))
        return true;
    /* No VMM lock acquisition under the PMM lock. Readiness is immutable
     * after release publication; CR3 is checked on the calling CPU. */
    if (!vmm_boot_memory_ready() || !vmm_get_kernel_pml4() ||
        vmm_get_current_pml4() != vmm_get_kernel_pml4())
        return false;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (!bitmap) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return false;
    }
    alloc_limit_pages = total_pages;
    __atomic_store_n(&high_memory_enabled, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return true;
}

uintptr_t pmm_alloc_page_above(uintptr_t min_phys) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    size_t start = alloc_limit_pages;
    if (min_phys <= UINT64_MAX - (PAGE_SIZE - 1))
        start = ALIGN_UP(min_phys, PAGE_SIZE) / PAGE_SIZE;
    uintptr_t result = 0;
    size_t steps = 0;
    for (size_t i = start; i < alloc_limit_pages; i++) {
        steps++;
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            free_pages--;
            result = (uintptr_t)(i * PAGE_SIZE);
            break;
        }
    }
    record_scan(steps);
    if (!result) allocation_failures++;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return result;
}

static uintptr_t pmm_alloc_page_unlocked(void) {
    size_t steps = 0;
    for (size_t i = 0; i < alloc_limit_pages; i++) {
        steps++;
        size_t idx = (last_allocated_index + i) % alloc_limit_pages;

        /* Skip byte quickly if all 8 frames are occupied */
        if ((idx % 8 == 0) && alloc_limit_pages - idx >= 8 &&
            alloc_limit_pages - i >= 8 && bitmap[idx / 8] == 0xFF) {
            i += 7;
            continue;
        }

        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            used_pages++;
            free_pages--;
            last_allocated_index = (idx + 1) % alloc_limit_pages;
            record_scan(steps);
            return (uintptr_t)(idx * PAGE_SIZE);
        }
    }

    record_scan(steps);
    return 0;
}

uintptr_t pmm_alloc_page(void) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    uintptr_t p = pmm_alloc_page_unlocked();
    if (!p) allocation_failures++;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return p;
}

static uintptr_t pmm_alloc_pages_unlocked(size_t count) {
    if (count == 0 || count > alloc_limit_pages) return 0;
    if (count == 1) return pmm_alloc_page_unlocked();

    size_t consecutive = 0;
    size_t start_idx = 0;
    size_t steps = 0;

    for (size_t i = 0; i < alloc_limit_pages; i++) {
        steps++;
        if (!bitmap_test(i)) {
            if (consecutive == 0) {
                start_idx = i;
            }
            consecutive++;
            if (consecutive == count) {
                for (size_t j = 0; j < count; j++) {
                    bitmap_set(start_idx + j);
                }
                used_pages += count;
                free_pages -= count;
                record_scan(steps);
                return (uintptr_t)(start_idx * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }

    record_scan(steps);
    return 0;
}

uintptr_t pmm_alloc_pages(size_t count) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    uintptr_t p = pmm_alloc_pages_unlocked(count);
    if (!p) allocation_failures++;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return p;
}

static void pmm_free_pages_unlocked(uintptr_t phys_addr, size_t count) {
    size_t start = phys_addr / PAGE_SIZE;
    if (!bitmap || phys_addr == 0 || phys_addr % PAGE_SIZE != 0 ||
        count == 0 || start >= total_pages || count > total_pages - start) {
        rejected_frees++;
        return;
    }
    /* No multiplication/addition until bounds establish a representable run.
     * A bad trailing member must not free an otherwise valid prefix. */
    for (size_t i = 0; i < count; i++)
        if (frame_reserved(start + i) || !bitmap_test(start + i)) {
            rejected_frees++;
            return;
        }
    for (size_t i = 0; i < count; i++) bitmap_clear(start + i);
    used_pages -= count;
    free_pages += count;
    if (start < last_allocated_index) last_allocated_index = start;
}

void pmm_free_page(uintptr_t phys_addr) {
    pmm_free_pages(phys_addr, 1);
}

void pmm_free_pages(uintptr_t phys_addr, size_t count) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    pmm_free_pages_unlocked(phys_addr, count);
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
}

static size_t allocatable_pages_unlocked(void) {
    size_t result = 0;
    if (alloc_limit_pages == total_pages) result = free_pages;
    else for (size_t i = 0; i < alloc_limit_pages; i++)
        if (!bitmap_test(i)) result++;
    return result;
}

size_t pmm_get_allocatable_pages(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    size_t result = allocatable_pages_unlocked();
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return result;
}

void pmm_get_stats(pmm_stats_t *out) {
    if (!out) return;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    *out = (pmm_stats_t){total_pages, used_pages, free_pages,
        allocatable_pages_unlocked(), rejected_frees, allocation_failures, max_scan_steps};
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_get_lock_stats(uint64_t *out_acquires, uint64_t *out_contentions) {
    if (out_acquires) *out_acquires = __atomic_load_n(&g_pmm_lock.acquire_count, __ATOMIC_RELAXED);
    if (out_contentions) *out_contentions = __atomic_load_n(&g_pmm_lock.contention_count, __ATOMIC_RELAXED);
}

size_t pmm_get_total_pages(void) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    size_t res = total_pages;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return res;
}

size_t pmm_get_used_pages(void) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    size_t res = used_pages;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return res;
}

size_t pmm_get_free_pages(void) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    size_t res = free_pages;
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return res;
}

uint64_t pmm_get_total_memory(void) {
    return (uint64_t)pmm_get_total_pages() * PAGE_SIZE;
}

uint64_t pmm_get_used_memory(void) {
    return (uint64_t)pmm_get_used_pages() * PAGE_SIZE;
}

uint64_t pmm_get_free_memory(void) {
    return (uint64_t)pmm_get_free_pages() * PAGE_SIZE;
}

bool pmm_audit(void) {
    /* 1. Verify bitmap is placed at or above 1 MiB and page-aligned */
    if (bitmap_phys_addr < 0x100000 || (bitmap_phys_addr % PAGE_SIZE) != 0) {
        serial_puts("[FAIL] PMM Audit: Bitmap physical address invalid or unaligned!\n");
        return false;
    }
    if (bitmap_phys_addr >= PMM_BOOT_ALLOC_LIMIT ||
        bitmap_total_pages > (PMM_BOOT_ALLOC_LIMIT - bitmap_phys_addr) / PAGE_SIZE) {
        serial_puts("[FAIL] PMM Audit: Bitmap outside early mapped RAM!\n");
        return false;
    }

    /* 2. Verify all bitmap frames are marked as reserved/used in the bitmap itself */
    size_t bm_start = bitmap_phys_addr / PAGE_SIZE;
    for (size_t i = 0; i < bitmap_total_pages; i++) {
        if (!bitmap_test(bm_start + i)) {
            serial_puts("[FAIL] PMM Audit: Bitmap frame not marked as reserved!\n");
            return false;
        }
    }

    /* 3. Verify physical frame 0 is reserved */
    if (!bitmap_test(0)) {
        serial_puts("[FAIL] PMM Audit: Physical frame 0 is unreserved!\n");
        return false;
    }

    /* 4. Verify total managed memory does not exceed bitmap capacity */
    if (pmm_get_total_memory() > PMM_BITMAP_MAX_RAM_BYTES) {
        serial_puts("[FAIL] PMM Audit: Total memory exceeds bitmap capacity!\n");
        return false;
    }

    return true;
}

size_t pmm_reclaim_bootloader_memory(struct limine_memmap_response *memmap) {
    /* Limine SMP handoff pointers and boot/module backing remain pinned.
     * No caller currently reclaims them; a lock cannot establish lifetime. */
    (void)memmap;
    return 0;
}


#ifdef TEST_SMP_MEMORY
/* Host-test initializer. Bypasses pmm_init()'s memmap parsing and sets
 * internal state directly. `bitmap_mem` must point at a caller-owned
 * buffer of at least (num_pages + 7) / 8 bytes, page-aligned. */
void pmm_test_init(size_t num_pages, uint8_t *bitmap_mem) {
    total_pages = num_pages;
    used_pages = 0;
    free_pages = num_pages;
    last_allocated_index = 0;

    /* Ceiling active: 1 GiB worth of pages. */
    size_t cap_pages = PMM_BOOT_ALLOC_LIMIT / PAGE_SIZE;
    alloc_limit_pages = (cap_pages < num_pages) ? cap_pages : num_pages;
    high_memory_enabled = false;

    bitmap = bitmap_mem;
    bitmap_phys_addr = 0x100000;   /* arbitrary, for audit sanity */
    bitmap_total_pages = 0;        /* audit not used in host test */
    memset(bitmap, 0x00, (num_pages + 7) / 8);   /* all free */
    memset(reserved_bitmap, 0x00, sizeof(reserved_bitmap));

    /* Frame 0 must be marked used to match pmm_init() semantics. */
    bitmap_set(0);
    used_pages++;
    free_pages--;

    rejected_frees = 0;
    allocation_failures = 0;
    max_scan_steps = 0;
}
#endif