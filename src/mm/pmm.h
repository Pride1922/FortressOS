#ifndef FORTRESS_PMM_H
#define FORTRESS_PMM_H

#include "types.h"
#include "limine.h"

#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12ULL
#define PMM_BOOT_ALLOC_LIMIT 0x40000000ULL /* Exclusive physical end: 1 GiB */

/* BSP-only initialization, before any concurrent allocator user. All later
 * bitmap scans/mutations and metrics use rank 4, held for the whole operation. */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* BSP boot only, after VMM has built all RAM mappings and activated the kernel
 * root. Refuses premature calls. Release-publishes readiness for AP bring-up. */
bool pmm_unlock_high_memory(void);
bool pmm_high_memory_enabled(void);

/* Allocate a single 4 KiB physical page frame (returns physical address or 0 on OOM) */
uintptr_t pmm_alloc_page(void);

/* Caller must exclusively own a currently allocated frame. Invalid, reserved
 * or already-free frames are rejected without mutation. This does not detect
 * a stale pointer to a frame that has since been reallocated to another owner. */
void pmm_free_page(uintptr_t phys_addr);

/* Allocate contiguous 4 KiB physical page frames (returns physical address or 0 on OOM) */
uintptr_t pmm_alloc_pages(size_t count);

/* First eligible free page at or above min_phys (rounded up), or 0.
 * All allocation APIs respect PMM_BOOT_ALLOC_LIMIT until the unlock. */
uintptr_t pmm_alloc_page_above(uintptr_t min_phys);

/* Same ownership contract. Validate the ENTIRE range before freeing anything;
 * overflow, zero count, reserved or already-free members reject the whole run. */
void pmm_free_pages(uintptr_t phys_addr, size_t count);

/* Memory metrics cover all managed RAM, including temporarily ineligible
 * high pages. Allocatable pages is the free subset below the current ceiling. */
size_t pmm_get_allocatable_pages(void);
typedef struct {
    size_t total_pages, used_pages, free_pages, allocatable_pages;
    uint64_t rejected_frees, allocation_failures, max_scan_steps;
} pmm_stats_t;
/* One coherent snapshot; separate getter calls may observe different instants.
 * max_scan_steps counts bitmap loop iterations, not time or a latency bound. */
void pmm_get_stats(pmm_stats_t *out);
size_t pmm_get_total_pages(void);
size_t pmm_get_used_pages(void);
size_t pmm_get_free_pages(void);
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);

#define PMM_BITMAP_CAPACITY_BYTES (1024 * 1024ULL)
#define PMM_BITMAP_MAX_RAM_BYTES  (PMM_BITMAP_CAPACITY_BYTES * 8ULL * PAGE_SIZE) /* 32 GiB */

/* Full bitmap/accounting audit under rank 4; diagnostics after unlocking.
 * O(managed frames), intended for thread/boot diagnostics, not IRQ handlers. */
bool pmm_audit(void);
/* Exact allocation-set snapshot. Caller owns storage; compare only at a
 * quiescent baseline with expected retained allocations already established. */
bool pmm_snapshot(void *buffer, size_t capacity);

/* Disabled pending a complete boot-response/module lifetime audit; returns 0. */
size_t pmm_reclaim_bootloader_memory(struct limine_memmap_response *memmap);

#ifdef TEST_SMP_MEMORY
void  pmm_test_init(size_t num_pages, uint8_t *bitmap_mem);
void *pmm_host_phys_to_virt(uintptr_t phys);
void  pmm_host_shim_init(void);
void  pmm_host_shim_free(void);
#endif
#endif /* FORTRESS_PMM_H */
