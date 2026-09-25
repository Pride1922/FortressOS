#ifndef FORTRESS_VMM_H
#define FORTRESS_VMM_H

#include "types.h"
#include "boot_info.h"

/* 64-bit Page Table Entry Flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Status Error Codes */
#define VMM_OK                   0
#define VMM_ERR_NOMEM           -1
#define VMM_ERR_ALREADY_MAPPED  -2
#define VMM_ERR_NOT_MAPPED      -3
#define VMM_ERR_INVALID_ADDR    -4
#define VMM_ERR_BUSY            -5

/* Address Space Lifecycle States */
typedef enum {
    VMM_SPACE_LIVE = 0,   /* Active, accepts new operations and sched references */
    VMM_SPACE_DYING = 1,  /* Retiring; rejects new operations and sched references */
    VMM_SPACE_DEAD = 2    /* Teardown complete, unlinked */
} vmm_space_state_t;

/* Address Space Record */
typedef struct vmm_space {
    uintptr_t           cr3;              /* Normalized physical root address */
    uint64_t           *pml4_virt;        /* Virtual address of PML4 root */
    vmm_space_state_t   state;            /* LIVE / DYING / DEAD */
    bool                is_kernel;        /* True if permanent master kernel space */
    bool                free_user_frames; /* Saved preference for deferred teardown */
    bool                deferred_queued;  /* True if queued on deferred destruction list */

    /* Reference counters protected by g_vmm_lock (Rank 3) */
    uint32_t            owner_refs;       /* Process/fixture ownership */
    uint32_t            sched_refs;       /* TCBs queued, running, blocked, or switching */
    uint32_t            op_refs;          /* Transient operations (map, unmap, walk) */

    /* Hardware residency */
    uint64_t            active_cpus_mask; /* Bitmask of CPUs currently using this CR3 */

    struct vmm_space   *next;             /* Intrusive link in global registry */
    struct vmm_space   *deferred_next;    /* Intrusive link in deferred destruction queue */
} vmm_space_t;

/* VMM Public API
 *
 * ADDRESS SPACE OWNERSHIP & LIFECYCLE CONTRACTS:
 * 1. Higher-Half (PML4 entries 256..511):
 *    - Kernel space (>= 0xFFFF800000000000). Globally owned and shared.
 *    - Process address spaces mirror these entries directly to share kernel mappings.
 *    - Higher-half entries are NEVER modified, traversed, or freed during address-space destruction.
 *    - Attempting to map any higher-half address with PTE_USER is rejected (VMM_ERR_INVALID_ADDR).
 *    - All required kernel PML4 entries are established statically during vmm_init().
 * 2. Lower-Half (PML4 entries 0..255):
 *    - User space (< 0x0000800000000000). Privately owned by each process.
 *    - Private page-table frames must NEVER be shared between user spaces.
 *    - Restricted to standard 4 KiB user page mappings (PTE_HUGE is unsupported in user space).
 * 3. Data Frame Ownership (free_user_frames):
 *    - When free_user_frames == true: Every mapped lower-half data frame must be exclusively
 *      owned by this address space and mapped exactly once. Teardown frees each frame to PMM.
 *    - When free_user_frames == false: Callers retain responsibility for data frames.
 *      Teardown reclaims only intermediate tables (PT, PD, PDPT) and the root PML4 frame.
 *    - PTE_USER describes access permissions, not memory ownership.
 * 4. Structural Pre-Validation:
 *    - Before freeing any tables, vmm_destroy_pml4() verifies the structure is supported.
 *      If unsupported huge pages (PTE_HUGE) or malformed entries are found, destruction is rejected.
 * 5. Self-Destruction Guard:
 *    - Attempting to destroy kernel_pml4_phys or the currently active CR3 (normalized)
 *      returns VMM_ERR_INVALID_ADDR.
 */
void      vmm_init(boot_info_t *boot_info);
/* Release-published only after all boot RAM mappings succeed and BSP CR3
 * reads back as the kernel root. Immutable thereafter; APs acquire-read it. */
bool      vmm_boot_memory_ready(void);
uintptr_t vmm_create_pml4(void);
uintptr_t vmm_create_user_pml4(void);
int       vmm_destroy_pml4(uintptr_t pml4_phys, bool free_user_frames);
int       vmm_map_page(uint64_t *pml4_virt, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags);
int       vmm_unmap_page(uint64_t *pml4_virt, uintptr_t virt_addr);
bool      vmm_is_mapped(uint64_t *pml4_virt, uintptr_t virt_addr);
uintptr_t vmm_get_physical_address(uint64_t *pml4_virt, uintptr_t virt_addr);
void      vmm_switch_pml4(uintptr_t pml4_phys);
uintptr_t vmm_get_kernel_pml4(void);
uint64_t *vmm_get_kernel_pml4_virt(void);
/* Diagnostic non-cryptographic fingerprint, ignoring hardware A/D changes.
 * Not an ownership proof; compare only with kernel mappings quiescent. */
uint64_t vmm_kernel_mapping_fingerprint(void);
uintptr_t vmm_get_current_pml4(void);
uint64_t *vmm_get_active_pml4_virt(void);
uint64_t  vmm_get_hhdm_offset(void);
void     *vmm_phys_to_virt(uintptr_t phys);
bool      vmm_validate_user_range(uint64_t *pml4_virt, uintptr_t virt_addr, size_t length, bool write_req);
size_t    vmm_get_allocated_table_frames(void);
size_t    vmm_get_retained_table_frames(void); /* Backward-compatible alias */

/* Address Space Registry & Lifecycle API */
vmm_space_t *vmm_space_lookup(uintptr_t cr3);
vmm_space_t *vmm_space_get_kernel(void);
int          vmm_space_retire(uintptr_t pml4_phys);
int          vmm_space_get_op(uint64_t *pml4_virt);
void         vmm_space_put_op(uint64_t *pml4_virt);
int          vmm_space_enter(uintptr_t next_cr3);
void         vmm_space_leave(uintptr_t old_cr3, bool thread_terminated, bool cr3_changed);
int          vmm_space_add_sched_ref(uintptr_t cr3);
void         vmm_space_sub_sched_ref(uintptr_t cr3);
size_t       vmm_drain_deferred_destructions(void);
size_t       vmm_get_deferred_count(void);

#endif /* FORTRESS_VMM_H */

