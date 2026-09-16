#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"

static spinlock_t g_heap_lock = {0};

/* Block Header Structure (16 bytes, 16-byte aligned) */
typedef struct heap_block_header {
    uint32_t magic;      /* HEAP_MAGIC_ALLOC or HEAP_MAGIC_FREE */
    uint32_t is_free;    /* 1 = free, 0 = allocated */
    size_t   size;       /* Total block size including header & footer (multiple of 16) */
} __attribute__((aligned(16))) heap_block_header_t;

/* Block Footer Structure (Boundary Tag, 16 bytes, 16-byte aligned) */
typedef struct heap_block_footer {
    uint32_t magic;      /* HEAP_MAGIC_FOOTER */
    uint32_t is_free;    /* 1 = free, 0 = allocated */
    size_t   size;       /* Must match header->size */
} __attribute__((aligned(16))) heap_block_footer_t;

/* Free List Node (Stored directly in unused payload of free blocks) */
typedef struct heap_free_node {
    struct heap_free_node *next;
    struct heap_free_node *prev;
} heap_free_node_t;

/* Global Heap State */
static uintptr_t         g_heap_start     = KERNEL_HEAP_START;
static uintptr_t         g_heap_end       = KERNEL_HEAP_START;
static heap_free_node_t *g_free_list_head = NULL;
static bool              g_heap_ready     = false;

/* Metrics (Count whole blocks including metadata) */
static size_t g_allocated_bytes  = 0;
static size_t g_allocated_blocks = 0;

/* Fault Injection State for Transactional Rollback Verification */
static heap_fault_type_t g_fault_type          = HEAP_FAULT_NONE;
static size_t            g_fault_trigger_count = 0;

#define MAX_EXPANSION_BATCH_PAGES 512

/* Diagnostic panic for corruption or invalid operations */
static void heap_panic(const char *reason, uintptr_t address) {
    serial_puts("\n=======================================================\n");
    serial_puts("[FATAL KERNEL HEAP CORRUPTION DETECTED]\n");
    serial_puts("Reason:  ");
    serial_puts(reason);
    serial_puts("\nAddress: ");
    serial_print_hex(address);
    serial_puts("\nHeap start: ");
    serial_print_hex(g_heap_start);
    serial_puts(" | Heap end: ");
    serial_print_hex(g_heap_end);
    serial_puts("\nHalting system to prevent further corruption.\n");
    serial_puts("=======================================================\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Metadata Validation Helper:
 * Validates header, footer, size, alignment, and bounds using subtraction before pointer arithmetic.
 */
static bool is_valid_block(const heap_block_header_t *hdr) {
    if (!hdr) return false;
    uintptr_t addr = (uintptr_t)hdr;

    /* 1. Header address must lie within committed heap and be 16-byte aligned */
    if (addr < g_heap_start || addr >= g_heap_end) {
        return false;
    }
    if ((addr % HEAP_ALIGNMENT) != 0) {
        return false;
    }

    /* Ensure the entire header fits in the committed range before reading fields */
    if (sizeof(heap_block_header_t) > g_heap_end - addr) {
        return false;
    }

    /* 2. Validate block size using subtraction to prevent arithmetic overflow */
    size_t size = hdr->size;
    if (size < HEAP_MIN_BLOCK_SZ || (size % HEAP_ALIGNMENT) != 0) {
        return false;
    }
    if (size > g_heap_end - addr) {
        return false;
    }

    /* 3. Header magic and allocation state: expected magic, valid state */
    if (hdr->magic != HEAP_MAGIC_ALLOC && hdr->magic != HEAP_MAGIC_FREE) {
        return false;
    }
    if (hdr->is_free != 0 && hdr->is_free != 1) {
        return false;
    }

    /* 4. Footer consistency: footer has its expected magic (HEAP_MAGIC_FOOTER); size and state match */
    const heap_block_footer_t *ftr = (const heap_block_footer_t *)((const uint8_t *)hdr + size - sizeof(heap_block_footer_t));
    if (ftr->magic != HEAP_MAGIC_FOOTER) {
        return false;
    }
    if (ftr->size != size) {
        return false;
    }
    if (ftr->is_free != hdr->is_free) {
        return false;
    }

    /* 5. Free list links reciprocal check (if free) */
    if (hdr->is_free == 1) {
        const heap_free_node_t *node = (const heap_free_node_t *)((const uint8_t *)hdr + sizeof(heap_block_header_t));
        if (node->next) {
            uintptr_t next_addr = (uintptr_t)node->next;
            if (next_addr < g_heap_start || next_addr >= g_heap_end) return false;
            if (node->next->prev != node) return false;
        }
        if (node->prev) {
            uintptr_t prev_addr = (uintptr_t)node->prev;
            if (prev_addr < g_heap_start || prev_addr >= g_heap_end) return false;
            if (node->prev->next != node) return false;
        } else {
            if (g_free_list_head != node) return false;
        }
    }

    return true;
}

/* Free List Operations (O(1) LIFO Insertion & Removal) */
static void free_list_insert(heap_block_header_t *hdr) {
    heap_free_node_t *node = (heap_free_node_t *)((uint8_t *)hdr + sizeof(heap_block_header_t));
    node->next = g_free_list_head;
    node->prev = NULL;
    if (g_free_list_head) {
        g_free_list_head->prev = node;
    }
    g_free_list_head = node;
}

static void free_list_remove(heap_block_header_t *hdr) {
    heap_free_node_t *node = (heap_free_node_t *)((uint8_t *)hdr + sizeof(heap_block_header_t));
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        g_free_list_head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    node->next = NULL;
    node->prev = NULL;
}

/* Expand the kernel heap dynamically in page-sized batches with transactional rollback */
static bool heap_expand(size_t bytes_needed) {
    size_t pages_needed = DIV_ROUND_UP(bytes_needed, PAGE_SIZE);
    if (pages_needed == 0) pages_needed = 1;
    if (pages_needed > MAX_EXPANSION_BATCH_PAGES) {
        return false;
    }

    /* Verify we do not exceed the virtual heap upper bound */
    if (g_heap_end + pages_needed * PAGE_SIZE > KERNEL_HEAP_MAX) {
        serial_puts("[HEAP] Expansion rejected: would exceed KERNEL_HEAP_MAX\n");
        return false;
    }

    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    if (!pml4) {
        serial_puts("[HEAP] Expansion failed: VMM kernel PML4 unavailable\n");
        return false;
    }

    uintptr_t old_end = g_heap_end;
    uintptr_t phys_frames[MAX_EXPANSION_BATCH_PAGES];
    size_t pages_mapped = 0;

    /* Execute page-by-page allocation and mapping with rollback on any failure */
    for (size_t i = 0; i < pages_needed; i++) {
        /* Fault Injection Check: PMM failure */
        if (g_fault_type == HEAP_FAULT_PMM_AFTER_N_PAGES && i == g_fault_trigger_count) {
            serial_puts("[FAULT-INJECT] Simulating PMM frame allocation failure\n");
            /* Rollback all previously mapped pages in this batch */
            for (size_t j = 0; j < pages_mapped; j++) {
                uintptr_t v = old_end + j * PAGE_SIZE;
                vmm_unmap_page(pml4, v);
                pmm_free_page(phys_frames[j]);
            }
            return false;
        }

        uintptr_t frame = pmm_alloc_page();
        if (frame == 0) {
            /* PMM out of memory -> rollback all previously mapped pages */
            for (size_t j = 0; j < pages_mapped; j++) {
                uintptr_t v = old_end + j * PAGE_SIZE;
                vmm_unmap_page(pml4, v);
                pmm_free_page(phys_frames[j]);
            }
            return false;
        }
        phys_frames[i] = frame;

        /* Fault Injection Check: VMM mapping failure */
        int status = VMM_OK;
        if (g_fault_type == HEAP_FAULT_VMM_AFTER_N_PAGES && i == g_fault_trigger_count) {
            serial_puts("[FAULT-INJECT] Simulating VMM page mapping failure after frame acquisition\n");
            status = VMM_ERR_NOMEM;
        } else {
            uintptr_t virt = old_end + i * PAGE_SIZE;
            status = vmm_map_page(pml4, virt, frame, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
        }

        if (status != VMM_OK) {
            /* Free the current frame that failed to map */
            pmm_free_page(frame);

            /* Rollback all previously mapped pages in this batch */
            for (size_t j = 0; j < pages_mapped; j++) {
                uintptr_t v = old_end + j * PAGE_SIZE;
                vmm_unmap_page(pml4, v);
                pmm_free_page(phys_frames[j]);
            }
            serial_puts("[HEAP] Rollback complete: unmapped data frames released to PMM\n");
            serial_puts("       Note: Retained VMM intermediate page-table frames accounted for\n");
            return false;
        }

        pages_mapped++;
    }

    /* TRANSACTION COMMIT POINT:
     * Up to here, g_heap_end, existing block metadata, and free list were untouched.
     * All pages in the batch are now successfully mapped into the virtual address space.
     */
    size_t new_bytes = pages_needed * PAGE_SIZE;
    uintptr_t new_end = old_end + new_bytes;

    /* Check if the block virtually adjacent to old_end was free to coalesce immediately */
    if (old_end > g_heap_start && (old_end - g_heap_start >= sizeof(heap_block_footer_t))) {
        heap_block_footer_t *prev_ftr = (heap_block_footer_t *)(old_end - sizeof(heap_block_footer_t));
        if (prev_ftr->magic == HEAP_MAGIC_FOOTER && prev_ftr->is_free == 1 &&
            prev_ftr->size >= HEAP_MIN_BLOCK_SZ && ((prev_ftr->size % HEAP_ALIGNMENT) == 0) &&
            prev_ftr->size <= old_end - g_heap_start) {
            heap_block_header_t *prev_hdr = (heap_block_header_t *)(old_end - prev_ftr->size);
            if (is_valid_block(prev_hdr) && prev_hdr->is_free == 1 && prev_hdr->size == prev_ftr->size) {
                /* Virtually adjacent block is free: expand it across the newly mapped batch */
                free_list_remove(prev_hdr);
                size_t combined = prev_hdr->size + new_bytes;
                prev_hdr->size = combined;

                heap_block_footer_t *new_ftr = (heap_block_footer_t *)(new_end - sizeof(heap_block_footer_t));
                new_ftr->magic = HEAP_MAGIC_FOOTER;
                new_ftr->is_free = 1;
                new_ftr->size = combined;

                free_list_insert(prev_hdr);
                g_heap_end = new_end;
                return true;
            }
        }
    }

    /* Otherwise, format the entire new batch as a single free block */
    heap_block_header_t *new_hdr = (heap_block_header_t *)old_end;
    new_hdr->magic = HEAP_MAGIC_FREE;
    new_hdr->is_free = 1;
    new_hdr->size = new_bytes;

    heap_block_footer_t *new_ftr = (heap_block_footer_t *)(new_end - sizeof(heap_block_footer_t));
    new_ftr->magic = HEAP_MAGIC_FOOTER;
    new_ftr->is_free = 1;
    new_ftr->size = new_bytes;

    free_list_insert(new_hdr);
    g_heap_end = new_end;
    return true;
}

/* Initialize Kernel Heap with an initial 16 KiB (4 pages) mapped pool */
void heap_init(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    serial_puts("[HEAP] Initializing Dynamic Kernel Heap Allocator...\n");
    g_heap_start = KERNEL_HEAP_START;
    g_heap_end = KERNEL_HEAP_START;
    g_free_list_head = NULL;
    g_allocated_bytes = 0;
    g_allocated_blocks = 0;

    /* Expand with initial 16 KiB */
    if (!heap_expand(4 * PAGE_SIZE)) {
        spin_unlock_irqrestore(&g_heap_lock, rflags);
        serial_puts("[FAIL] Heap initial expansion failed!\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    g_heap_ready = true;
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    serial_puts("[HEAP] Initialized successfully. Start: ");
    serial_print_hex(g_heap_start);
    serial_puts(" | End: ");
    serial_print_hex(g_heap_end);
    serial_puts(" (Capacity: 16 KiB)\n");
}

/* Internal unlocked kmalloc */
static void *kmalloc_unlocked(size_t size) {
    if (size == 0 || !g_heap_ready) {
        return NULL;
    }

    /* Arithmetic overflow check */
    if (size > (size_t)(KERNEL_HEAP_MAX - KERNEL_HEAP_START)) {
        return NULL;
    }

    size_t aligned_payload = ALIGN_UP(size, HEAP_ALIGNMENT);
    if (aligned_payload < sizeof(heap_free_node_t)) {
        aligned_payload = sizeof(heap_free_node_t);
    }

    size_t total_size = sizeof(heap_block_header_t) + aligned_payload + sizeof(heap_block_footer_t);
    total_size = ALIGN_UP(total_size, HEAP_ALIGNMENT);

    /* Search for first-fit block in free list */
    heap_free_node_t *curr = g_free_list_head;
    heap_block_header_t *matched_hdr = NULL;

    while (curr) {
        heap_block_header_t *hdr = (heap_block_header_t *)((uint8_t *)curr - sizeof(heap_block_header_t));
        if (!is_valid_block(hdr) || hdr->is_free != 1) {
            heap_panic("Corrupt free block in free list during kmalloc", (uintptr_t)hdr);
        }
        if (hdr->size >= total_size) {
            matched_hdr = hdr;
            break;
        }
        curr = curr->next;
    }

    /* If no fit found, attempt to expand heap on-demand */
    if (!matched_hdr) {
        if (!heap_expand(total_size)) {
            return NULL; /* Out of memory */
        }
        /* Search again after expansion */
        curr = g_free_list_head;
        while (curr) {
            heap_block_header_t *hdr = (heap_block_header_t *)((uint8_t *)curr - sizeof(heap_block_header_t));
            if (hdr->size >= total_size) {
                matched_hdr = hdr;
                break;
            }
            curr = curr->next;
        }
        if (!matched_hdr) {
            return NULL;
        }
    }

    /* Remove matched block from free list */
    free_list_remove(matched_hdr);

    /* Block Splitting: Only split if excess remainder is >= HEAP_MIN_BLOCK_SZ */
    size_t excess = matched_hdr->size - total_size;
    if (excess >= HEAP_MIN_BLOCK_SZ) {
        matched_hdr->size = total_size;

        heap_block_footer_t *alloc_ftr = (heap_block_footer_t *)((uint8_t *)matched_hdr + total_size - sizeof(heap_block_footer_t));
        alloc_ftr->magic = HEAP_MAGIC_FOOTER;
        alloc_ftr->is_free = 0;
        alloc_ftr->size = total_size;

        heap_block_header_t *rem_hdr = (heap_block_header_t *)((uint8_t *)matched_hdr + total_size);
        rem_hdr->magic = HEAP_MAGIC_FREE;
        rem_hdr->is_free = 1;
        rem_hdr->size = excess;

        heap_block_footer_t *rem_ftr = (heap_block_footer_t *)((uint8_t *)rem_hdr + excess - sizeof(heap_block_footer_t));
        rem_ftr->magic = HEAP_MAGIC_FOOTER;
        rem_ftr->is_free = 1;
        rem_ftr->size = excess;

        free_list_insert(rem_hdr);
    } else {
        /* Keep entire block without splitting */
        heap_block_footer_t *alloc_ftr = (heap_block_footer_t *)((uint8_t *)matched_hdr + matched_hdr->size - sizeof(heap_block_footer_t));
        alloc_ftr->is_free = 0;
    }

    matched_hdr->magic = HEAP_MAGIC_ALLOC;
    matched_hdr->is_free = 0;

    g_allocated_bytes += matched_hdr->size;
    g_allocated_blocks++;

    return (void *)((uint8_t *)matched_hdr + sizeof(heap_block_header_t));
}

void *kmalloc(size_t size) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    void *ptr = kmalloc_unlocked(size);
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return ptr;
}

/* Internal unlocked kfree */
static void kfree_unlocked(void *ptr) {
    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;
    if (addr < g_heap_start + sizeof(heap_block_header_t) || addr >= g_heap_end) {
        heap_panic("kfree called with pointer out of heap bounds", addr);
    }
    if ((addr % HEAP_ALIGNMENT) != 0) {
        heap_panic("kfree called with unaligned pointer", addr);
    }

    heap_block_header_t *hdr = (heap_block_header_t *)(addr - sizeof(heap_block_header_t));
    if (!is_valid_block(hdr)) {
        if (hdr->magic == HEAP_MAGIC_FREE || hdr->is_free == 1) {
            heap_panic("kfree: detected double-free", addr);
        }
        heap_panic("kfree: invalid or corrupt block metadata", addr);
    }
    if (hdr->is_free != 0) {
        heap_panic("kfree: block already marked free", addr);
    }

    g_allocated_bytes -= hdr->size;
    g_allocated_blocks--;

    /* 1. Check virtually adjacent right neighbor (if not at heap end) */
    if (hdr->size < g_heap_end - (uintptr_t)hdr) {
        uintptr_t next_addr = (uintptr_t)hdr + hdr->size;
        if (next_addr < g_heap_end) {
            heap_block_header_t *next_hdr = (heap_block_header_t *)next_addr;
            if (is_valid_block(next_hdr) && next_hdr->is_free == 1) {
                free_list_remove(next_hdr);
                hdr->size += next_hdr->size;
            }
        }
    }

    /* 2. Check virtually adjacent left neighbor (if not at heap start) */
    if ((uintptr_t)hdr > g_heap_start && ((uintptr_t)hdr - g_heap_start >= sizeof(heap_block_footer_t))) {
        heap_block_footer_t *prev_ftr = (heap_block_footer_t *)((uint8_t *)hdr - sizeof(heap_block_footer_t));
        if (prev_ftr->magic == HEAP_MAGIC_FOOTER && prev_ftr->is_free == 1 &&
            prev_ftr->size >= HEAP_MIN_BLOCK_SZ && ((prev_ftr->size % HEAP_ALIGNMENT) == 0) &&
            prev_ftr->size <= (uintptr_t)hdr - g_heap_start) {
            heap_block_header_t *prev_hdr = (heap_block_header_t *)((uint8_t *)hdr - prev_ftr->size);
            if (is_valid_block(prev_hdr) && prev_hdr->is_free == 1 && prev_hdr->size == prev_ftr->size) {
                free_list_remove(prev_hdr);
                prev_hdr->size += hdr->size;
                hdr = prev_hdr;
            }
        }
    }

    /* 3. Update coalesced block metadata */
    hdr->magic = HEAP_MAGIC_FREE;
    hdr->is_free = 1;

    heap_block_footer_t *final_ftr = (heap_block_footer_t *)((uint8_t *)hdr + hdr->size - sizeof(heap_block_footer_t));
    final_ftr->magic = HEAP_MAGIC_FOOTER;
    final_ftr->is_free = 1;
    final_ftr->size = hdr->size;

    free_list_insert(hdr);
}

void kfree(void *ptr) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    kfree_unlocked(ptr);
    spin_unlock_irqrestore(&g_heap_lock, rflags);
}

/* Internal unlocked kcalloc */
static void *kcalloc_unlocked(size_t num, size_t size) {
    if (num == 0 || size == 0) return NULL;

    /* Multiplication overflow check */
    if (size > (size_t)-1 / num) {
        return NULL;
    }

    size_t total = num * size;
    void *ptr = kmalloc_unlocked(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *kcalloc(size_t num, size_t size) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    void *ptr = kcalloc_unlocked(num, size);
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return ptr;
}

/* Internal unlocked krealloc */
static void *krealloc_unlocked(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc_unlocked(new_size);
    }
    if (new_size == 0) {
        kfree_unlocked(ptr);
        return NULL;
    }

    uintptr_t addr = (uintptr_t)ptr;
    if (addr < g_heap_start + sizeof(heap_block_header_t) || addr >= g_heap_end) {
        heap_panic("krealloc called with pointer out of heap bounds", addr);
    }

    heap_block_header_t *hdr = (heap_block_header_t *)(addr - sizeof(heap_block_header_t));
    if (!is_valid_block(hdr) || hdr->is_free != 0) {
        heap_panic("krealloc: invalid or free block metadata", addr);
    }

    size_t old_payload_size = hdr->size - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);

    size_t req_aligned = ALIGN_UP(new_size, HEAP_ALIGNMENT);
    if (req_aligned < sizeof(heap_free_node_t)) {
        req_aligned = sizeof(heap_free_node_t);
    }
    size_t req_total = sizeof(heap_block_header_t) + req_aligned + sizeof(heap_block_footer_t);
    req_total = ALIGN_UP(req_total, HEAP_ALIGNMENT);

    /* Case 1: Shrink in place */
    if (req_total <= hdr->size) {
        size_t excess = hdr->size - req_total;
        if (excess >= HEAP_MIN_BLOCK_SZ) {
            hdr->size = req_total;
            heap_block_footer_t *ftr = (heap_block_footer_t *)((uint8_t *)hdr + req_total - sizeof(heap_block_footer_t));
            ftr->magic = HEAP_MAGIC_FOOTER;
            ftr->is_free = 0;
            ftr->size = req_total;

            heap_block_header_t *rem = (heap_block_header_t *)((uint8_t *)hdr + req_total);
            rem->magic = HEAP_MAGIC_ALLOC;
            rem->is_free = 0;
            rem->size = excess;
            heap_block_footer_t *rem_ftr = (heap_block_footer_t *)((uint8_t *)rem + excess - sizeof(heap_block_footer_t));
            rem_ftr->magic = HEAP_MAGIC_FOOTER;
            rem_ftr->is_free = 0;
            rem_ftr->size = excess;

            /* Free the excess remainder */
            kfree_unlocked((void *)((uint8_t *)rem + sizeof(heap_block_header_t)));
        }
        return ptr;
    }

    /* Case 2: In-place growth into virtually adjacent free right neighbor */
    uintptr_t next_addr = (uintptr_t)hdr + hdr->size;
    if (next_addr < g_heap_end) {
        heap_block_header_t *next_hdr = (heap_block_header_t *)next_addr;
        if (is_valid_block(next_hdr) && next_hdr->is_free == 1 &&
            (hdr->size + next_hdr->size >= req_total)) {
            size_t old_size = hdr->size;
            size_t combined = hdr->size + next_hdr->size;
            free_list_remove(next_hdr);

            size_t excess = combined - req_total;
            if (excess >= HEAP_MIN_BLOCK_SZ) {
                hdr->size = req_total;
                heap_block_footer_t *ftr = (heap_block_footer_t *)((uint8_t *)hdr + req_total - sizeof(heap_block_footer_t));
                ftr->magic = HEAP_MAGIC_FOOTER;
                ftr->is_free = 0;
                ftr->size = req_total;

                heap_block_header_t *rem = (heap_block_header_t *)((uint8_t *)hdr + req_total);
                rem->magic = HEAP_MAGIC_FREE;
                rem->is_free = 1;
                rem->size = excess;
                heap_block_footer_t *rem_ftr = (heap_block_footer_t *)((uint8_t *)rem + excess - sizeof(heap_block_footer_t));
                rem_ftr->magic = HEAP_MAGIC_FOOTER;
                rem_ftr->is_free = 1;
                rem_ftr->size = excess;
                free_list_insert(rem);

                g_allocated_bytes += (req_total - old_size);
            } else {
                hdr->size = combined;
                heap_block_footer_t *ftr = (heap_block_footer_t *)((uint8_t *)hdr + combined - sizeof(heap_block_footer_t));
                ftr->magic = HEAP_MAGIC_FOOTER;
                ftr->is_free = 0;
                ftr->size = combined;

                g_allocated_bytes += (combined - old_size);
            }
            return ptr;
        }
    }

    /* Case 3: Allocate new buffer, copy min(old_payload, new_requested), and free original */
    void *new_ptr = kmalloc_unlocked(new_size);
    if (!new_ptr) {
        /* Failure strictly preserves original allocation and data intact */
        return NULL;
    }

    size_t copy_len = old_payload_size < new_size ? old_payload_size : new_size;
    memcpy(new_ptr, ptr, copy_len);
    kfree_unlocked(ptr);
    return new_ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    void *res = krealloc_unlocked(ptr, new_size);
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

/* Metrics & Invariants (Internal Unlocked) */
static size_t heap_get_used_bytes_unlocked(void) {
    return g_allocated_bytes;
}

size_t heap_get_used_bytes(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    size_t res = heap_get_used_bytes_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

static size_t heap_get_free_bytes_unlocked(void) {
    size_t free_bytes = 0;
    heap_free_node_t *curr = g_free_list_head;
    while (curr) {
        heap_block_header_t *hdr = (heap_block_header_t *)((uint8_t *)curr - sizeof(heap_block_header_t));
        if (hdr->magic == HEAP_MAGIC_FREE && hdr->is_free == 1) {
            free_bytes += hdr->size;
        }
        curr = curr->next;
    }
    return free_bytes;
}

size_t heap_get_free_bytes(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    size_t res = heap_get_free_bytes_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

static size_t heap_get_total_bytes_unlocked(void) {
    return g_heap_end - g_heap_start;
}

size_t heap_get_total_bytes(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    size_t res = heap_get_total_bytes_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

static size_t heap_get_allocated_blocks_unlocked(void) {
    return g_allocated_blocks;
}

size_t heap_get_allocated_blocks(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    size_t res = heap_get_allocated_blocks_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

static size_t heap_get_free_blocks_unlocked(void) {
    size_t count = 0;
    heap_free_node_t *curr = g_free_list_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

size_t heap_get_free_blocks(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    size_t res = heap_get_free_blocks_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

/* Linear Debug Heap Walk (Internal Unlocked) */
static bool heap_verify_integrity_unlocked(void) {
    if (!g_heap_ready) return false;

    uintptr_t curr_addr = g_heap_start;
    size_t calculated_used = 0;
    size_t calculated_free = 0;
    size_t free_block_count = 0;
    bool prev_was_free = false;

    while (curr_addr < g_heap_end) {
        heap_block_header_t *hdr = (heap_block_header_t *)curr_addr;
        if (!is_valid_block(hdr)) {
            serial_puts("[HEAP-AUDIT FAIL] Corrupt block at: ");
            serial_print_hex(curr_addr);
            serial_puts("\n");
            return false;
        }

        if (hdr->is_free == 1) {
            if (prev_was_free) {
                serial_puts("[HEAP-AUDIT FAIL] Adjacent free blocks not coalesced at: ");
                serial_print_hex(curr_addr);
                serial_puts("\n");
                return false;
            }
            prev_was_free = true;
            calculated_free += hdr->size;
            free_block_count++;
        } else {
            prev_was_free = false;
            calculated_used += hdr->size;
        }

        curr_addr += hdr->size;
    }

    if (curr_addr != g_heap_end) {
        serial_puts("[HEAP-AUDIT FAIL] Final address does not match g_heap_end!\n");
        return false;
    }

    /* Verify free list count matches free blocks found */
    size_t list_count = heap_get_free_blocks_unlocked();
    if (list_count != free_block_count) {
        serial_puts("[HEAP-AUDIT FAIL] Free list count mismatch (list: ");
        serial_print_dec(list_count);
        serial_puts(", walk: ");
        serial_print_dec(free_block_count);
        serial_puts(")\n");
        return false;
    }

    /* Verify invariant: calculated_used + calculated_free == total */
    size_t total = heap_get_total_bytes_unlocked();
    if (calculated_used + calculated_free != total || calculated_used != g_allocated_bytes) {
        serial_puts("[HEAP-AUDIT FAIL] Heap metric invariant violated! used=");
        serial_print_dec(calculated_used);
        serial_puts(" (g_alloc=");
        serial_print_dec(g_allocated_bytes);
        serial_puts(") free=");
        serial_print_dec(calculated_free);
        serial_puts(" total=");
        serial_print_dec(total);
        serial_puts(" sum=");
        serial_print_dec(calculated_used + calculated_free);
        serial_puts("\n");
        return false;
    }

    return true;
}

bool heap_verify_integrity(void) {
    uint64_t rflags = spin_lock_irqsave(&g_heap_lock);
    bool res = heap_verify_integrity_unlocked();
    spin_unlock_irqrestore(&g_heap_lock, rflags);
    return res;
}

/* Fault injection hooks for testing */
void heap_set_fault_injection(heap_fault_type_t type, size_t trigger_count) {
    g_fault_type = type;
    g_fault_trigger_count = trigger_count;
}

void heap_clear_fault_injection(void) {
    g_fault_type = HEAP_FAULT_NONE;
    g_fault_trigger_count = 0;
}
