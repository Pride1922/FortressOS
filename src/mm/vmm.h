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

/* VMM Public API
 * Page-table frames are retained for the lifetime of the address space.
 * A failed map can retain newly allocated intermediate tables; unmap only
 * clears the leaf and never frees the caller-owned physical frame.
 * Rollback, empty-table reclamation, and address-space destruction are deferred.
 */
void      vmm_init(boot_info_t *boot_info);
uintptr_t vmm_create_pml4(void);
int       vmm_map_page(uint64_t *pml4_virt, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags);
int       vmm_unmap_page(uint64_t *pml4_virt, uintptr_t virt_addr);
bool      vmm_is_mapped(uint64_t *pml4_virt, uintptr_t virt_addr);
uintptr_t vmm_get_physical_address(uint64_t *pml4_virt, uintptr_t virt_addr);
void      vmm_switch_pml4(uintptr_t pml4_phys);
uintptr_t vmm_get_kernel_pml4(void);
uint64_t *vmm_get_kernel_pml4_virt(void);
size_t    vmm_get_retained_table_frames(void);

#endif /* FORTRESS_VMM_H */
