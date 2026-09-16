#ifndef FORTRESS_PMM_H
#define FORTRESS_PMM_H

#include "types.h"
#include "limine.h"

#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12ULL

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Allocate a single 4 KiB physical page frame (returns physical address or 0 on OOM) */
uintptr_t pmm_alloc_page(void);

/* Free a single 4 KiB physical page frame */
void pmm_free_page(uintptr_t phys_addr);

/* Allocate contiguous 4 KiB physical page frames (returns physical address or 0 on OOM) */
uintptr_t pmm_alloc_pages(size_t count);

/* Free contiguous 4 KiB physical page frames */
void pmm_free_pages(uintptr_t phys_addr, size_t count);

/* Memory metrics */
size_t pmm_get_total_pages(void);
size_t pmm_get_used_pages(void);
size_t pmm_get_free_pages(void);
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);

/* 64 KiB bitmap tracks 524,288 frames = exactly 2 GiB of physical RAM */
#define PMM_BITMAP_CAPACITY_BYTES (64 * 1024ULL)
#define PMM_BITMAP_MAX_RAM_BYTES  (PMM_BITMAP_CAPACITY_BYTES * 8ULL * PAGE_SIZE) /* 2 GiB */

/* PMM Audit & Integrity Validation */
bool pmm_audit(void);
/* Exact allocation-set snapshot. Caller owns storage; compare only at a
 * quiescent baseline with expected retained allocations already established. */
bool pmm_snapshot(void *buffer, size_t capacity);

/* Deferrable bootloader memory reclamation */
size_t pmm_reclaim_bootloader_memory(struct limine_memmap_response *memmap);

#endif /* FORTRESS_PMM_H */
