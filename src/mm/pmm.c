#include "pmm.h"
#include "serial.h"
#include "spinlock.h"
#include "string.h"


static uint8_t   *bitmap = NULL;
static uintptr_t  bitmap_phys_addr = 0;
static size_t     bitmap_total_pages = 0;
static size_t     total_pages = 0;
static size_t     used_pages = 0;
static size_t     free_pages = 0;
static size_t     last_allocated_index = 0;
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

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    if (!memmap || memmap->entry_count == 0) {
        serial_puts("[FAIL] PMM: Invalid or missing Limine memory map\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* 1. Calculate highest usable physical memory address */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
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
    used_pages  = total_pages;
    free_pages  = 0;

    /* 2. Calculate bitmap size in bytes, aligned up to 4 KiB */
    size_t bitmap_size = (total_pages + 7) / 8;
    bitmap_size = (bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t bitmap_pages = bitmap_size / PAGE_SIZE;

    /* Keep the bitmap itself in managed, page-aligned RAM above 1 MiB. */
    uintptr_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->base >= highest_addr ||
            entry->length > UINT64_MAX - entry->base) continue;
        uint64_t end = entry->base + entry->length;
        if (end > highest_addr) end = highest_addr;
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
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            size_t start_frame = (size_t)(entry->base / PAGE_SIZE);
            size_t frame_count = (size_t)(entry->length / PAGE_SIZE);

            for (size_t f = 0; f < frame_count; f++) {
                size_t frame = start_frame + f;
                if (frame < total_pages && bitmap_test(frame)) {
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
}

/* Temporary probe helper: allocate one free page whose physical address is
 * >= min_phys. Returns 0 if none exists above min_phys. */
uintptr_t pmm_alloc_page_above(uintptr_t min_phys) {
    size_t start = min_phys / PAGE_SIZE;
    if (start >= total_pages) return 0;

    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    uintptr_t result = 0;
    for (size_t i = start; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            free_pages--;
            result = (uintptr_t)(i * PAGE_SIZE);
            break;
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return result;
}

static uintptr_t pmm_alloc_page_unlocked(void) {
    for (size_t i = 0; i < total_pages; i++) {
        size_t idx = (last_allocated_index + i) % total_pages;

        /* Skip byte quickly if all 8 frames are occupied */
        if ((idx % 8 == 0) && (bitmap[idx / 8] == 0xFF)) {
            i += 7;
            continue;
        }

        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            used_pages++;
            free_pages--;
            last_allocated_index = (idx + 1) % total_pages;
            return (uintptr_t)(idx * PAGE_SIZE);
        }
    }

    serial_puts("[WARN] PMM: Out of physical memory (pmm_alloc_page)!\n");
    return 0;
}

uintptr_t pmm_alloc_page(void) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    uintptr_t p = pmm_alloc_page_unlocked();
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return p;
}

static void pmm_free_page_unlocked(uintptr_t phys_addr) {
    if (phys_addr == 0 || (phys_addr % PAGE_SIZE) != 0) {
        return;
    }

    size_t idx = (size_t)(phys_addr / PAGE_SIZE);
    if (idx >= total_pages) {
        return;
    }

    if (bitmap_test(idx)) {
        bitmap_clear(idx);
        used_pages--;
        free_pages++;
        if (idx < last_allocated_index) {
            last_allocated_index = idx;
        }
    }
}

void pmm_free_page(uintptr_t phys_addr) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    pmm_free_page_unlocked(phys_addr);
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
}

static uintptr_t pmm_alloc_pages_unlocked(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page_unlocked();

    size_t consecutive = 0;
    size_t start_idx = 0;

    for (size_t i = 0; i < total_pages; i++) {
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
                return (uintptr_t)(start_idx * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }

    serial_puts("[WARN] PMM: Out of contiguous physical memory!\n");
    return 0;
}

uintptr_t pmm_alloc_pages(size_t count) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    uintptr_t p = pmm_alloc_pages_unlocked(count);
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
    return p;
}

static void pmm_free_pages_unlocked(uintptr_t phys_addr, size_t count) {
    if (phys_addr == 0 || (phys_addr % PAGE_SIZE) != 0 || count == 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        pmm_free_page_unlocked(phys_addr + (i * PAGE_SIZE));
    }
}

void pmm_free_pages(uintptr_t phys_addr, size_t count) {
    uint64_t rflags = spin_lock_irqsave(&g_pmm_lock);
    pmm_free_pages_unlocked(phys_addr, count);
    spin_unlock_irqrestore(&g_pmm_lock, rflags);
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
    return (uint64_t)total_pages * PAGE_SIZE;
}

uint64_t pmm_get_used_memory(void) {
    return (uint64_t)used_pages * PAGE_SIZE;
}

uint64_t pmm_get_free_memory(void) {
    return (uint64_t)free_pages * PAGE_SIZE;
}

bool pmm_audit(void) {
    /* 1. Verify bitmap is placed at or above 1 MiB and page-aligned */
    if (bitmap_phys_addr < 0x100000 || (bitmap_phys_addr % PAGE_SIZE) != 0) {
        serial_puts("[FAIL] PMM Audit: Bitmap physical address invalid or unaligned!\n");
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
        serial_puts("[FAIL] PMM Audit: Total memory exceeds 2 GiB bitmap capacity!\n");
        return false;
    }

    return true;
}

size_t pmm_reclaim_bootloader_memory(struct limine_memmap_response *memmap) {
    if (!memmap) return 0;
    size_t reclaimed_frames = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            size_t start_frame = (size_t)(entry->base / PAGE_SIZE);
            size_t frame_count = (size_t)(entry->length / PAGE_SIZE);

            for (size_t f = 0; f < frame_count; f++) {
                size_t frame = start_frame + f;
                if (frame < total_pages && bitmap_test(frame)) {
                    bitmap_clear(frame);
                    used_pages--;
                    free_pages++;
                    reclaimed_frames++;
                }
            }
        }
    }

    return reclaimed_frames;
}
