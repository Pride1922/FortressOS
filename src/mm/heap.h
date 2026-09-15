#ifndef FORTRESS_HEAP_H
#define FORTRESS_HEAP_H

#include "types.h"

/* FortressOS Kernel Dynamic Heap Allocator (Phase 4B)
 *
 * Architecture:
 * - Boundary-tag based heap allocator with 16-byte aligned payloads.
 * - 16-byte block header and 16-byte block footer for O(1) bidirectional coalescing.
 * - Explicit doubly linked free list with pointers stored inside free payloads.
 * - On-demand virtual memory expansion backed by non-contiguous 4 KiB physical frames.
 * - Transactional expansion rollback on physical frame or mapping exhaustion.
 * - Invariant: heap_get_used_bytes() + heap_get_free_bytes() == heap_get_total_bytes().
 *
 * Concurrency & Execution Context Contract:
 * - Single-CPU only; non-reentrant.
 * - MUST NOT be called from Interrupt Service Routines (ISRs) or interrupt context.
 * - Full spinlock synchronization will be integrated in Phase 6 (Scheduling & Threads).
 */

/* Virtual Address Bounds for Kernel Dynamic Heap (512 MiB capacity) */
#define KERNEL_HEAP_START 0xFFFFFFFFB0000000ULL
#define KERNEL_HEAP_MAX   0xFFFFFFFFD0000000ULL

/* Block Magic Signatures for Metadata Integrity and Corruption Trapping */
#define HEAP_MAGIC_ALLOC   0x414C4F43U /* 'ALOC' */
#define HEAP_MAGIC_FREE    0x46524545U /* 'FREE' */
#define HEAP_MAGIC_FOOTER  0x464F4F54U /* 'FOOT' */

/* Memory Alignment Constraints */
#define HEAP_ALIGNMENT     16ULL
#define HEAP_MIN_BLOCK_SZ  48ULL /* 16B header + 16B free node payload + 16B footer */

/* Public Heap API */
void  heap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t num, size_t size);
void *krealloc(void *ptr, size_t new_size);

/* Introspection, Invariant Checking & Metrics */
size_t heap_get_used_bytes(void);
size_t heap_get_free_bytes(void);
size_t heap_get_total_bytes(void);
size_t heap_get_allocated_blocks(void);
size_t heap_get_free_blocks(void);
bool   heap_verify_integrity(void);

/* Controlled Fault Injection Hooks (For Transactional Rollback Testing) */
typedef enum {
    HEAP_FAULT_NONE = 0,
    HEAP_FAULT_PMM_AFTER_N_PAGES, /* Fails PMM frame allocation at page index N */
    HEAP_FAULT_VMM_AFTER_N_PAGES  /* Fails VMM page mapping at page index N (after frame acquired) */
} heap_fault_type_t;

void heap_set_fault_injection(heap_fault_type_t type, size_t trigger_count);
void heap_clear_fault_injection(void);

#endif /* FORTRESS_HEAP_H */
