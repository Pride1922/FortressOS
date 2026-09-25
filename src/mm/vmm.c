#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "gdt.h"
#include "spinlock.h"
#include "smp.h"
#include "percpu.h"
#include "thread.h"
#include "heap.h"

extern uint8_t __kernel_start[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __kernel_end[];

static uint64_t  hhdm_offset = 0;
static uintptr_t kernel_pml4_phys = 0;
static bool boot_memory_ready = false;

/* Static master kernel address space (permanent, never destroyed) */
static vmm_space_t g_vmm_kernel_space;
static vmm_space_t *g_vmm_spaces_list = NULL;
static vmm_space_t *g_vmm_deferred_list = NULL;
static size_t      g_vmm_allocated_table_frames = 0;
static spinlock_t  g_vmm_lock = SPINLOCK_RANKED(3, "vmm");

vmm_space_t *vmm_space_lookup(uintptr_t cr3) {
    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    uintptr_t norm_cr3 = cr3 & PTE_ADDR_MASK;
    while (curr) {
        if ((curr->cr3 & PTE_ADDR_MASK) == norm_cr3) {
            spin_unlock_irqrestore(&g_vmm_lock, rflags);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return NULL;
}

vmm_space_t *vmm_space_get_kernel(void) {
    return &g_vmm_kernel_space;
}

int vmm_space_retire(uintptr_t pml4_phys) {
    uintptr_t norm_cr3 = pml4_phys & PTE_ADDR_MASK;
    if (norm_cr3 == (kernel_pml4_phys & PTE_ADDR_MASK) || norm_cr3 == 0) {
        return VMM_ERR_INVALID_ADDR;
    }

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if ((curr->cr3 & PTE_ADDR_MASK) == norm_cr3) {
            if (curr->is_kernel) {
                spin_unlock_irqrestore(&g_vmm_lock, rflags);
                return VMM_ERR_INVALID_ADDR;
            }
            if (curr->state == VMM_SPACE_DEAD) {
                spin_unlock_irqrestore(&g_vmm_lock, rflags);
                return VMM_ERR_INVALID_ADDR;
            }
            if (curr->state == VMM_SPACE_DYING) {
                spin_unlock_irqrestore(&g_vmm_lock, rflags);
                return VMM_OK; /* Idempotent */
            }
            curr->state = VMM_SPACE_DYING;
            spin_unlock_irqrestore(&g_vmm_lock, rflags);
            return VMM_OK;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return VMM_ERR_INVALID_ADDR;
}

int vmm_space_get_op(uint64_t *pml4_virt) {
    if (!pml4_virt) return VMM_ERR_INVALID_ADDR;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->pml4_virt == pml4_virt) {
            if (curr->is_kernel) {
                spin_unlock_irqrestore(&g_vmm_lock, rflags);
                return VMM_OK;
            }
            if (curr->state != VMM_SPACE_LIVE) {
                spin_unlock_irqrestore(&g_vmm_lock, rflags);
                return VMM_ERR_INVALID_ADDR;
            }
            curr->op_refs++;
            spin_unlock_irqrestore(&g_vmm_lock, rflags);
            return VMM_OK;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return VMM_ERR_INVALID_ADDR;
}

void vmm_space_put_op(uint64_t *pml4_virt) {
    if (!pml4_virt) return;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->pml4_virt == pml4_virt) {
            if (!curr->is_kernel && curr->op_refs > 0) {
                curr->op_refs--;
            }
            spin_unlock_irqrestore(&g_vmm_lock, rflags);
            return;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
}

int vmm_space_enter(uintptr_t next_cr3) {
    uintptr_t norm_cr3 = next_cr3 & PTE_ADDR_MASK;
    if (norm_cr3 == 0 || norm_cr3 == (kernel_pml4_phys & PTE_ADDR_MASK)) {
        return VMM_OK;
    }

    uint64_t flags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->cr3 == norm_cr3) {
            if (curr->state == VMM_SPACE_DEAD) {
                spin_unlock_irqrestore(&g_vmm_lock, flags);
                return VMM_ERR_INVALID_ADDR;
            }
            uint32_t cid = cpu_current()->id;
            if (cid < 64) {
                curr->active_cpus_mask |= (1ULL << cid);
            }
            spin_unlock_irqrestore(&g_vmm_lock, flags);
            return VMM_OK;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, flags);
    return VMM_ERR_INVALID_ADDR;
}

void vmm_space_leave(uintptr_t old_cr3, bool thread_terminated, bool cr3_changed) {
    uintptr_t norm_cr3 = old_cr3 & PTE_ADDR_MASK;
    if (norm_cr3 == 0 || norm_cr3 == (kernel_pml4_phys & PTE_ADDR_MASK)) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->cr3 == norm_cr3) {
            if (cr3_changed) {
                uint32_t cid = cpu_current()->id;
                if (cid < 64) {
                    curr->active_cpus_mask &= ~(1ULL << cid);
                }
            }
            if (thread_terminated && curr->sched_refs > 0) {
                curr->sched_refs--;
            }
            spin_unlock_irqrestore(&g_vmm_lock, flags);
            return;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, flags);
}

int vmm_space_add_sched_ref(uintptr_t cr3) {
    uintptr_t norm_cr3 = cr3 & PTE_ADDR_MASK;
    if (norm_cr3 == 0 || norm_cr3 == (kernel_pml4_phys & PTE_ADDR_MASK)) {
        return VMM_OK;
    }

    uint64_t flags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->cr3 == norm_cr3) {
            if (curr->state != VMM_SPACE_LIVE) {
                spin_unlock_irqrestore(&g_vmm_lock, flags);
                return VMM_ERR_INVALID_ADDR;
            }
            curr->sched_refs++;
            spin_unlock_irqrestore(&g_vmm_lock, flags);
            return VMM_OK;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, flags);
    return VMM_ERR_INVALID_ADDR;
}

void vmm_space_sub_sched_ref(uintptr_t cr3) {
    uintptr_t norm_cr3 = cr3 & PTE_ADDR_MASK;
    if (norm_cr3 == 0 || norm_cr3 == (kernel_pml4_phys & PTE_ADDR_MASK)) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_spaces_list;
    while (curr) {
        if (curr->cr3 == norm_cr3) {
            if (curr->sched_refs > 0) {
                curr->sched_refs--;
            }
            spin_unlock_irqrestore(&g_vmm_lock, flags);
            return;
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, flags);
}


bool vmm_boot_memory_ready(void) {
    return __atomic_load_n(&boot_memory_ready, __ATOMIC_ACQUIRE);
}

static inline void *phys_to_virt(uintptr_t phys) {
    return (void *)(phys + hhdm_offset);
}

static inline uintptr_t virt_to_phys(void *virt) {
    return ((uintptr_t)virt - hhdm_offset);
}

static inline size_t pml4_index(uintptr_t virt) { return (virt >> 39) & 0x1FF; }
static inline size_t pdpt_index(uintptr_t virt) { return (virt >> 30) & 0x1FF; }
static inline size_t pd_index(uintptr_t virt)   { return (virt >> 21) & 0x1FF; }
static inline size_t pt_index(uintptr_t virt)   { return (virt >> 12) & 0x1FF; }

static inline bool is_canonical_address(uintptr_t addr) {
    uintptr_t top = addr >> 47;
    return (top == 0) || (top == 0x1FFFF);
}

static uint64_t fingerprint_table(uint64_t *table, unsigned level,
                                  unsigned first, uint64_t hash) {
    for (unsigned i = first; i < 512; i++) {
        uint64_t entry = table[i];
        uint64_t stable = entry & ~(PTE_ACCESSED | PTE_DIRTY);
        hash = (hash ^ stable) * 1099511628211ULL;
        if (level > 1 && (entry & PTE_PRESENT) && !(entry & PTE_HUGE))
            hash = fingerprint_table(phys_to_virt(entry & PTE_ADDR_MASK),
                                     level - 1, 0, hash);
    }
    return hash;
}

uint64_t vmm_kernel_mapping_fingerprint(void) {
    uint64_t flags = spin_lock_irqsave(&g_vmm_lock);
    uint64_t result = fingerprint_table(phys_to_virt(kernel_pml4_phys), 4, 256,
                                        14695981039346656037ULL);
    spin_unlock_irqrestore(&g_vmm_lock, flags);
    return result;
}

static uint64_t *get_or_create_table(uint64_t *parent_table, size_t index, uint64_t flags) {
    uint64_t entry = parent_table[index];

    if (entry & PTE_PRESENT) {
        /* If child mapping requires user mode, upgrade intermediate table entry */
        if (flags & PTE_USER) {
            parent_table[index] |= PTE_USER;
        }
        uintptr_t table_phys = entry & PTE_ADDR_MASK;
        return (uint64_t *)phys_to_virt(table_phys);
    }

    /* Allocate a fresh 4 KiB frame from PMM for the intermediate page table */
    uintptr_t new_table_phys = pmm_alloc_page();
    if (new_table_phys == 0) {
        return NULL; /* Out of physical memory */
    }

    g_vmm_allocated_table_frames++;
    uint64_t *new_table_virt = (uint64_t *)phys_to_virt(new_table_phys);
    memset(new_table_virt, 0, PAGE_SIZE);

    /* Link table into parent hierarchy */
    parent_table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return new_table_virt;
}

uintptr_t vmm_create_pml4(void) {
    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    uintptr_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        spin_unlock_irqrestore(&g_vmm_lock, rflags);
        return 0;
    }

    g_vmm_allocated_table_frames++;
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);
    memset(pml4_virt, 0, PAGE_SIZE);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return pml4_phys;
}

uintptr_t vmm_create_user_pml4(void) {
    /* 1. Pre-allocate metadata before taking g_vmm_lock (Heap Rank 2 -> VMM Rank 3) */
    vmm_space_t *space = (vmm_space_t *)kmalloc(sizeof(vmm_space_t));
    if (!space) {
        return 0;
    }

    /* 2. Allocate root PML4 frame from PMM */
    uintptr_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        kfree(space); /* Clean rollback on frame allocation failure */
        return 0;
    }

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);

    g_vmm_allocated_table_frames++;
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);

    /* 1. Clear lower half (user space, PML4 entries 0..255) */
    memset(pml4_virt, 0, 256 * sizeof(uint64_t));

    /* 2. Mirror higher half (kernel space, PML4 entries 256..511) from master kernel PML4 */
    if (kernel_pml4_phys != 0) {
        uint64_t *k_pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);
        memcpy(&pml4_virt[256], &k_pml4[256], 256 * sizeof(uint64_t));
    } else {
        memset(&pml4_virt[256], 0, 256 * sizeof(uint64_t));
    }

    /* Initialize space metadata and register under lock */
    space->cr3 = pml4_phys & PTE_ADDR_MASK;
    space->pml4_virt = pml4_virt;
    space->state = VMM_SPACE_LIVE;
    space->is_kernel = false;
    space->owner_refs = 1;
    space->sched_refs = 0;
    space->op_refs = 0;
    space->active_cpus_mask = 0;
    space->free_user_frames = false;
    space->deferred_queued = false;
    space->deferred_next = NULL;
    space->next = g_vmm_spaces_list;
    g_vmm_spaces_list = space;

    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return pml4_phys;
}

uintptr_t vmm_get_current_pml4(void) {
#ifdef TEST_VMM_HOST
    extern uintptr_t g_host_mock_cr3;
    return g_host_mock_cr3 & PTE_ADDR_MASK;
#else
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR_MASK;
#endif
}

static int vmm_teardown_pml4_tables_unlocked(uintptr_t pml4_phys, bool free_user_frames) {
    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);

    /*
     * PASS 1: Pre-Validation.
     * Ensure the lower half contains only supported 4 KiB structures.
     * Reject unsupported huge-page entries (PTE_HUGE) BEFORE freeing any frames
     * to prevent leaving a partially destroyed or corrupt address space.
     */
    for (size_t i = 0; i < 256; i++) {
        if (!(pml4_virt[i] & PTE_PRESENT)) continue;

        uintptr_t pdpt_phys = pml4_virt[i] & PTE_ADDR_MASK;
        uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (size_t j = 0; j < 512; j++) {
            if (!(pdpt_virt[j] & PTE_PRESENT)) continue;
            if (pdpt_virt[j] & PTE_HUGE) {
                return VMM_ERR_INVALID_ADDR; /* 1 GiB huge page unsupported in user space */
            }

            uintptr_t pd_phys = pdpt_virt[j] & PTE_ADDR_MASK;
            uint64_t *pd_virt = (uint64_t *)phys_to_virt(pd_phys);

            for (size_t k = 0; k < 512; k++) {
                if (!(pd_virt[k] & PTE_PRESENT)) continue;
                if (pd_virt[k] & PTE_HUGE) {
                    return VMM_ERR_INVALID_ADDR; /* 2 MiB huge page unsupported in user space */
                }
            }
        }
    }

    /*
     * PASS 2: Safe Destruction Pass.
     * Traverse ONLY lower half (user space, PML4 entries 0..255).
     * Entries 256..511 are shared kernel mappings and must NEVER be freed!
     */
    for (size_t i = 0; i < 256; i++) {
        if (!(pml4_virt[i] & PTE_PRESENT)) continue;

        uintptr_t pdpt_phys = pml4_virt[i] & PTE_ADDR_MASK;
        uint64_t *pdpt_virt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (size_t j = 0; j < 512; j++) {
            if (!(pdpt_virt[j] & PTE_PRESENT)) continue;

            uintptr_t pd_phys = pdpt_virt[j] & PTE_ADDR_MASK;
            uint64_t *pd_virt = (uint64_t *)phys_to_virt(pd_phys);

            for (size_t k = 0; k < 512; k++) {
                if (!(pd_virt[k] & PTE_PRESENT)) continue;

                uintptr_t pt_phys = pd_virt[k] & PTE_ADDR_MASK;
                uint64_t *pt_virt = (uint64_t *)phys_to_virt(pt_phys);

                for (size_t l = 0; l < 512; l++) {
                    if (pt_virt[l] & PTE_PRESENT) {
                        if (free_user_frames) {
                            /* Exclusive ownership contract: free singly-owned user data frame */
                            pmm_free_page(pt_virt[l] & PTE_ADDR_MASK);
                        }
                        pt_virt[l] = 0;
                    }
                }

                /* Free Level 1 PT frame */
                pmm_free_page(pt_phys);
                g_vmm_allocated_table_frames--;
                pd_virt[k] = 0;
            }

            /* Free Level 2 PD frame */
            pmm_free_page(pd_phys);
            g_vmm_allocated_table_frames--;
            pdpt_virt[j] = 0;
        }

        /* Free Level 3 PDPT frame */
        pmm_free_page(pdpt_phys);
        g_vmm_allocated_table_frames--;
        pml4_virt[i] = 0;
    }

    /* Free root Level 4 PML4 frame */
    pmm_free_page(pml4_phys);
    g_vmm_allocated_table_frames--;

    return VMM_OK;
}

int vmm_destroy_pml4(uintptr_t pml4_phys, bool free_user_frames) {
    if (pml4_phys == 0 || (pml4_phys % PAGE_SIZE) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }

    /* Safety Guard: Never destroy master kernel PML4 or currently active normalized CR3 */
    if (pml4_phys == kernel_pml4_phys || (pml4_phys & PTE_ADDR_MASK) == (kernel_pml4_phys & PTE_ADDR_MASK)) {
        return VMM_ERR_INVALID_ADDR;
    }
    if (pml4_phys == (vmm_get_current_pml4() & PTE_ADDR_MASK)) {
        return VMM_ERR_INVALID_ADDR;
    }

    vmm_space_t *sp = vmm_space_lookup(pml4_phys);
    if (sp && sp->is_kernel) {
        return VMM_ERR_INVALID_ADDR;
    }

    /* SMP Invariant: Ensure no other online core is running in this address space */
    for (size_t i = 0; i < smp_get_cpu_count(); i++) {
        if (cpu_locals[i].current_thread &&
            (cpu_locals[i].current_thread->cr3 & PTE_ADDR_MASK) == pml4_phys) {
            return VMM_ERR_INVALID_ADDR;
        }
    }

    /* Synchronously invalidate TLB entries on any cores that cached this PML4 */
    smp_tlb_shootdown(0, pml4_phys);

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);

    /* Locate space in registry and check eligibility under lock */
    vmm_space_t *space_to_free = NULL;
    vmm_space_t **link = &g_vmm_spaces_list;
    while (*link) {
        if (((*link)->cr3 & PTE_ADDR_MASK) == (pml4_phys & PTE_ADDR_MASK)) {
            space_to_free = *link;
            break;
        }
        link = &(*link)->next;
    }

    if (!space_to_free) {
        spin_unlock_irqrestore(&g_vmm_lock, rflags);
        return VMM_ERR_INVALID_ADDR;
    }

    /* Invariant: Cannot immediately destroy active or referenced space */
    if (space_to_free->active_cpus_mask != 0 ||
        space_to_free->sched_refs != 0 ||
        space_to_free->op_refs != 0) {
        /* Enqueue for deferred destruction once all references and active masks drain */
        space_to_free->state = VMM_SPACE_DYING;
        space_to_free->free_user_frames = free_user_frames;
        if (!space_to_free->deferred_queued) {
            space_to_free->deferred_queued = true;
            space_to_free->deferred_next = g_vmm_deferred_list;
            g_vmm_deferred_list = space_to_free;
        }
        spin_unlock_irqrestore(&g_vmm_lock, rflags);
        return VMM_ERR_BUSY;
    }

    /* If on deferred list, unlink from it */
    if (space_to_free->deferred_queued) {
        vmm_space_t *dcurr = g_vmm_deferred_list;
        vmm_space_t *dprev = NULL;
        while (dcurr) {
            if (dcurr == space_to_free) {
                if (dprev) dprev->deferred_next = dcurr->deferred_next;
                else g_vmm_deferred_list = dcurr->deferred_next;
                space_to_free->deferred_next = NULL;
                space_to_free->deferred_queued = false;
                break;
            }
            dprev = dcurr;
            dcurr = dcurr->deferred_next;
        }
    }

    /* Unlink space metadata under lock once pre-validation succeeds */
    *link = space_to_free->next;
    space_to_free->state = VMM_SPACE_DEAD;

    int teardown_status = vmm_teardown_pml4_tables_unlocked(pml4_phys, free_user_frames);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    if (space_to_free && !space_to_free->is_kernel) {
        kfree(space_to_free);
    }

    return teardown_status;
}

size_t vmm_drain_deferred_destructions(void) {
    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    vmm_space_t *curr = g_vmm_deferred_list;
    vmm_space_t *prev = NULL;
    vmm_space_t *free_list = NULL;
    size_t drained_count = 0;

    while (curr) {
        vmm_space_t *next = curr->deferred_next;
        if (curr->active_cpus_mask == 0 && curr->sched_refs == 0 && curr->op_refs == 0) {
            /* Unlink from deferred list */
            if (prev) {
                prev->deferred_next = next;
            } else {
                g_vmm_deferred_list = next;
            }
            curr->deferred_next = NULL;
            curr->deferred_queued = false;

            /* Unlink from active spaces list */
            vmm_space_t **link = &g_vmm_spaces_list;
            while (*link) {
                if (*link == curr) {
                    *link = curr->next;
                    break;
                }
                link = &(*link)->next;
            }
            curr->state = VMM_SPACE_DEAD;

            /* Teardown page table hierarchy */
            vmm_teardown_pml4_tables_unlocked(curr->cr3, curr->free_user_frames);

            /* Queue for kfree outside g_vmm_lock */
            curr->next = free_list;
            free_list = curr;
            drained_count++;
        } else {
            prev = curr;
        }
        curr = next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    /* Free space metadata structures outside g_vmm_lock (Rank 3 -> Rank 2) */
    while (free_list) {
        vmm_space_t *next = free_list->next;
        if (!free_list->is_kernel) {
            kfree(free_list);
        }
        free_list = next;
    }

    return drained_count;
}

size_t vmm_get_deferred_count(void) {
    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    size_t count = 0;
    vmm_space_t *curr = g_vmm_deferred_list;
    while (curr) {
        count++;
        curr = curr->deferred_next;
    }
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return count;
}

static int vmm_map_page_unlocked(uint64_t *pml4_virt, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags) {
    if (!pml4_virt) return VMM_ERR_INVALID_ADDR;
    if ((virt_addr % PAGE_SIZE) != 0 || (phys_addr % PAGE_SIZE) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    if (!is_canonical_address(virt_addr)) {
        return VMM_ERR_INVALID_ADDR;
    }

    /* Invariant: Reject user privilege in higher-half kernel space */
    if ((flags & PTE_USER) && virt_addr >= 0xFFFF800000000000ULL) {
        return VMM_ERR_INVALID_ADDR;
    }

    size_t pml4_i = pml4_index(virt_addr);
    size_t pdpt_i = pdpt_index(virt_addr);
    size_t pd_i   = pd_index(virt_addr);
    size_t pt_i   = pt_index(virt_addr);

    /* 1. Level 4 -> Level 3 (PDPT) */
    uint64_t *pdpt = get_or_create_table(pml4_virt, pml4_i, flags);
    if (!pdpt) return VMM_ERR_NOMEM;

    /* 2. Level 3 -> Level 2 (PD) */
    uint64_t *pd = get_or_create_table(pdpt, pdpt_i, flags);
    if (!pd) return VMM_ERR_NOMEM;

    /* 3. Level 2 -> Level 1 (PT) */
    uint64_t *pt = get_or_create_table(pd, pd_i, flags);
    if (!pt) return VMM_ERR_NOMEM;

    /* 4. Level 1 Entry */
    if (pt[pt_i] & PTE_PRESENT) {
        return VMM_ERR_ALREADY_MAPPED;
    }

    pt[pt_i] = (phys_addr & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    /* Invalidate TLB for this virtual address */
#ifndef TEST_VMM_HOST
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
#else
    (void)virt_addr;
#endif
    return VMM_OK;
}

int vmm_map_page(uint64_t *pml4_virt, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags) {
    int op_status = vmm_space_get_op(pml4_virt);
    if (op_status != VMM_OK) return op_status;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    int res = vmm_map_page_unlocked(pml4_virt, virt_addr, phys_addr, flags);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    vmm_space_put_op(pml4_virt);
    return res;
}

static int vmm_unmap_page_unlocked(uint64_t *pml4_virt, uintptr_t virt_addr) {
    if (!pml4_virt || (virt_addr % PAGE_SIZE) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    if (!is_canonical_address(virt_addr)) {
        return VMM_ERR_INVALID_ADDR;
    }

    size_t pml4_i = pml4_index(virt_addr);
    size_t pdpt_i = pdpt_index(virt_addr);
    size_t pd_i   = pd_index(virt_addr);
    size_t pt_i   = pt_index(virt_addr);

    if (!(pml4_virt[pml4_i] & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_virt[pml4_i] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_i] & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

    if (!(pd[pd_i] & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

    if (!(pt[pt_i] & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;

    /* Clear entry (ownership rule: does NOT free physical frame) */
    pt[pt_i] = 0;

    /* Invalidate TLB */
#ifndef TEST_VMM_HOST
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
#else
    (void)virt_addr;
#endif
    return VMM_OK;
}

int vmm_unmap_page(uint64_t *pml4_virt, uintptr_t virt_addr) {
    int op_status = vmm_space_get_op(pml4_virt);
    if (op_status != VMM_OK) return op_status;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    int res = vmm_unmap_page_unlocked(pml4_virt, virt_addr);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    if (res == VMM_OK) {
        uintptr_t cr3 = 0;
        if (pml4_virt && pml4_virt != vmm_get_kernel_pml4_virt()) {
            cr3 = (uintptr_t)pml4_virt - hhdm_offset;
        }
        smp_tlb_shootdown(virt_addr, cr3);
    }

    vmm_space_put_op(pml4_virt);
    return res;
}

static bool vmm_is_mapped_unlocked(uint64_t *pml4_virt, uintptr_t virt_addr) {
    if (!pml4_virt || !is_canonical_address(virt_addr)) return false;

    size_t pml4_i = pml4_index(virt_addr);
    size_t pdpt_i = pdpt_index(virt_addr);
    size_t pd_i   = pd_index(virt_addr);
    size_t pt_i   = pt_index(virt_addr);

    if (!(pml4_virt[pml4_i] & PTE_PRESENT)) return false;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_virt[pml4_i] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_i] & PTE_PRESENT)) return false;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

    if (!(pd[pd_i] & PTE_PRESENT)) return false;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

    return (pt[pt_i] & PTE_PRESENT) != 0;
}

bool vmm_is_mapped(uint64_t *pml4_virt, uintptr_t virt_addr) {
    if (vmm_space_get_op(pml4_virt) != VMM_OK) return false;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    bool res = vmm_is_mapped_unlocked(pml4_virt, virt_addr);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    vmm_space_put_op(pml4_virt);
    return res;
}

static uintptr_t vmm_get_physical_address_unlocked(uint64_t *pml4_virt, uintptr_t virt_addr) {
    if (!pml4_virt || !is_canonical_address(virt_addr)) return 0;

    size_t pml4_i = pml4_index(virt_addr);
    size_t pdpt_i = pdpt_index(virt_addr);
    size_t pd_i   = pd_index(virt_addr);
    size_t pt_i   = pt_index(virt_addr);

    if (!(pml4_virt[pml4_i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_virt[pml4_i] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_i] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

    if (!(pd[pd_i] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

    if (!(pt[pt_i] & PTE_PRESENT)) return 0;

    return (pt[pt_i] & PTE_ADDR_MASK) | (virt_addr & 0xFFF);
}

uintptr_t vmm_get_physical_address(uint64_t *pml4_virt, uintptr_t virt_addr) {
    if (vmm_space_get_op(pml4_virt) != VMM_OK) return 0;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    uintptr_t res = vmm_get_physical_address_unlocked(pml4_virt, virt_addr);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    vmm_space_put_op(pml4_virt);
    return res;
}

void vmm_switch_pml4(uintptr_t pml4_phys) {
#ifdef TEST_VMM_HOST
    extern uintptr_t g_host_mock_cr3;
    g_host_mock_cr3 = pml4_phys & PTE_ADDR_MASK;
#else
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
#endif
}

#ifdef TEST_VMM_HOST
void vmm_test_init_kernel_space(uintptr_t k_cr3, uint64_t *k_virt, uint64_t hhdm) {
    hhdm_offset = hhdm;
    kernel_pml4_phys = k_cr3 & PTE_ADDR_MASK;
    g_vmm_kernel_space.cr3 = kernel_pml4_phys;
    g_vmm_kernel_space.pml4_virt = k_virt;
    g_vmm_kernel_space.state = VMM_SPACE_LIVE;
    g_vmm_kernel_space.is_kernel = true;
    g_vmm_kernel_space.owner_refs = 1;
    g_vmm_kernel_space.sched_refs = 0;
    g_vmm_kernel_space.op_refs = 0;
    g_vmm_kernel_space.active_cpus_mask = 0;
    g_vmm_kernel_space.free_user_frames = false;
    g_vmm_kernel_space.deferred_queued = false;
    g_vmm_kernel_space.deferred_next = NULL;
    g_vmm_kernel_space.next = NULL;
    g_vmm_spaces_list = &g_vmm_kernel_space;
    g_vmm_deferred_list = NULL;
    g_vmm_allocated_table_frames = 1;
}
#endif

uintptr_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

uint64_t *vmm_get_kernel_pml4_virt(void) {
    if (kernel_pml4_phys == 0) return NULL;
    return (uint64_t *)phys_to_virt(kernel_pml4_phys);
}

size_t vmm_get_allocated_table_frames(void) {
    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    size_t res = g_vmm_allocated_table_frames;
    spin_unlock_irqrestore(&g_vmm_lock, rflags);
    return res;
}

size_t vmm_get_retained_table_frames(void) {
    return vmm_get_allocated_table_frames();
}

uint64_t *vmm_get_active_pml4_virt(void) {
    uintptr_t cr3 = vmm_get_current_pml4();
    if (cr3 == 0) return NULL;
    return (uint64_t *)phys_to_virt(cr3);
}

uint64_t vmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

void *vmm_phys_to_virt(uintptr_t phys) {
    return phys_to_virt(phys);
}

static bool vmm_validate_user_range_unlocked(uint64_t *pml4_virt, uintptr_t virt_addr, size_t length, bool write_req) {
    if (!pml4_virt) return false;
    if (length == 0) return true;

    /* Check integer overflow */
    if (virt_addr + length < virt_addr) return false;

    /* Must reside strictly in lower-half user space (< 0x0000800000000000) */
    if ((virt_addr + length) > 0x0000800000000000ULL) return false;
    if (!is_canonical_address(virt_addr) || !is_canonical_address(virt_addr + length - 1)) return false;

    uintptr_t start_page = virt_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_page   = (virt_addr + length - 1) & ~(PAGE_SIZE - 1);

    for (uintptr_t page = start_page; ; page += PAGE_SIZE) {
        size_t pml4_i = pml4_index(page);
        size_t pdpt_i = pdpt_index(page);
        size_t pd_i   = pd_index(page);
        size_t pt_i   = pt_index(page);

        /* Level 4 entry */
        if (!(pml4_virt[pml4_i] & PTE_PRESENT) || !(pml4_virt[pml4_i] & PTE_USER)) return false;
        if (write_req && !(pml4_virt[pml4_i] & PTE_WRITABLE)) return false;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_virt[pml4_i] & PTE_ADDR_MASK);

        /* Level 3 entry */
        if (!(pdpt[pdpt_i] & PTE_PRESENT) || !(pdpt[pdpt_i] & PTE_USER)) return false;
        if (write_req && !(pdpt[pdpt_i] & PTE_WRITABLE)) return false;
        if (pdpt[pdpt_i] & PTE_HUGE) return false; /* User space restricted to 4 KiB */
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

        /* Level 2 entry */
        if (!(pd[pd_i] & PTE_PRESENT) || !(pd[pd_i] & PTE_USER)) return false;
        if (write_req && !(pd[pd_i] & PTE_WRITABLE)) return false;
        if (pd[pd_i] & PTE_HUGE) return false; /* User space restricted to 4 KiB */
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

        /* Level 1 entry */
        if (!(pt[pt_i] & PTE_PRESENT) || !(pt[pt_i] & PTE_USER)) return false;
        if (write_req && !(pt[pt_i] & PTE_WRITABLE)) return false;

        if (page == end_page) break;
    }

    return true;
}

bool vmm_validate_user_range(uint64_t *pml4_virt, uintptr_t virt_addr, size_t length, bool write_req) {
    if (vmm_space_get_op(pml4_virt) != VMM_OK) return false;

    uint64_t rflags = spin_lock_irqsave(&g_vmm_lock);
    bool res = vmm_validate_user_range_unlocked(pml4_virt, virt_addr, length, write_req);
    spin_unlock_irqrestore(&g_vmm_lock, rflags);

    vmm_space_put_op(pml4_virt);
    return res;
}

/* Helper to assert that essential boot mappings succeed without ignoring errors */
static void vmm_must_map(uint64_t *pml4, uintptr_t virt, uintptr_t phys, uint64_t flags, const char *context) {
    int res = vmm_map_page(pml4, virt, phys, flags);
    if (res != VMM_OK) {
        serial_puts("[FATAL] VMM boot mapping failed in ");
        serial_puts(context);
        serial_puts(" at virt ");
        serial_print_hex(virt);
        serial_puts(" (error: ");
        serial_print_dec(res);
        serial_puts(")\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
}

/* Filter memory map regions: only map physical RAM into HHDM; skip massive reserved/MMIO holes */
static bool should_map_in_hhdm(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
            return true;
        default:
            return false;
    }
}

extern uint8_t kernel_stack_guard[];

void vmm_init(boot_info_t *boot_info) {
    if (!boot_info) {
        serial_puts("[FAIL] VMM: Missing boot info!\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    hhdm_offset = boot_info->hhdm_offset;

    serial_puts("[VMM] Building kernel 4-level page tables (PML4)...\n");

    /* 1. Allocate root PML4 table */
    kernel_pml4_phys = vmm_create_pml4();
    if (kernel_pml4_phys == 0) {
        serial_puts("[FAIL] VMM: Failed to allocate root PML4 table!\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    if ((kernel_pml4_phys & ~PTE_ADDR_MASK) != 0) {
        serial_puts("[FAIL] VMM: kernel_pml4_phys has unaligned bits\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* Initialize static master kernel space metadata so early boot mappings hold op_refs safely */
    g_vmm_kernel_space.cr3 = kernel_pml4_phys & PTE_ADDR_MASK;
    g_vmm_kernel_space.pml4_virt = (uint64_t *)phys_to_virt(kernel_pml4_phys);
    g_vmm_kernel_space.state = VMM_SPACE_LIVE;
    g_vmm_kernel_space.is_kernel = true;
    g_vmm_kernel_space.owner_refs = 1;
    g_vmm_kernel_space.sched_refs = 0;
    g_vmm_kernel_space.op_refs = 0;
    g_vmm_kernel_space.active_cpus_mask = 0;
    g_vmm_kernel_space.free_user_frames = false;
    g_vmm_kernel_space.deferred_queued = false;
    g_vmm_kernel_space.deferred_next = NULL;
    g_vmm_kernel_space.next = NULL;
    g_vmm_spaces_list = &g_vmm_kernel_space;
    g_vmm_deferred_list = NULL;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);

    /* 2. Map Higher-Half Direct Map (HHDM) selectively for physical RAM regions */
    for (size_t i = 0; i < boot_info->memmap_entry_count; i++) {
        struct limine_memmap_entry *entry = &boot_info->memmap_entries[i];
        if (!should_map_in_hhdm(entry->type)) {
            continue;
        }

        if (entry->length == 0) continue;
        if (entry->length > UINT64_MAX - entry->base ||
            entry->base + entry->length > UINT64_MAX - (PAGE_SIZE - 1)) {
            serial_puts("[FAIL] VMM: RAM map range overflow\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }

        uintptr_t start_phys = entry->base & ~(PAGE_SIZE - 1);
        uintptr_t end_phys   = (entry->base + entry->length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (end_phys > PTE_ADDR_MASK + PAGE_SIZE ||
            end_phys - 1 > UINT64_MAX - hhdm_offset ||
            !is_canonical_address(hhdm_offset + start_phys) ||
            !is_canonical_address(hhdm_offset + end_phys - 1)) {
            serial_puts("[FAIL] VMM: RAM range outside representable HHDM\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }

        for (uintptr_t phys = start_phys; phys < end_phys; phys += PAGE_SIZE) {
            uintptr_t virt = hhdm_offset + phys;
            if (!vmm_is_mapped(pml4, virt)) {
                vmm_must_map(pml4, virt, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX, "HHDM RAM");
            }
        }
    }
    serial_puts("[VMM] Higher-Half Direct Map (HHDM) physical RAM regions mapped (RW, NX)\n");

    /* 3. Map Kernel ELF sections with precise permissions */
    uintptr_t phys_base = boot_info->kernel_phys_base;
    uintptr_t virt_base = boot_info->kernel_virt_base;

    /* .text: Read-Only, Executable (RX) */
    uintptr_t text_start = (uintptr_t)__text_start;
    uintptr_t text_end   = (uintptr_t)__text_end;
    for (uintptr_t v = text_start; v < text_end; v += PAGE_SIZE) {
        uintptr_t p = phys_base + (v - virt_base);
        vmm_must_map(pml4, v, p, PTE_PRESENT, "Kernel .text");
    }
    serial_puts("[VMM] Kernel .text mapped (RX - Read-Only, Executable)\n");

    /* .rodata: Read-Only, No-Execute (R, NX) */
    uintptr_t rodata_start = (uintptr_t)__rodata_start;
    uintptr_t rodata_end   = (uintptr_t)__rodata_end;
    for (uintptr_t v = rodata_start; v < rodata_end; v += PAGE_SIZE) {
        uintptr_t p = phys_base + (v - virt_base);
        vmm_must_map(pml4, v, p, PTE_PRESENT | PTE_NX, "Kernel .rodata");
    }
    serial_puts("[VMM] Kernel .rodata mapped (R - Read-Only, NX)\n");

    /* .data and .bss (including stacks, GDT/TSS, IDT): Writable, No-Execute (RW, NX) */
    uintptr_t data_start = (uintptr_t)__data_start;
    uintptr_t kernel_end = (uintptr_t)__kernel_end;
    for (uintptr_t v = data_start; v < kernel_end; v += PAGE_SIZE) {
        uintptr_t p = phys_base + (v - virt_base);
        vmm_must_map(pml4, v, p, PTE_PRESENT | PTE_WRITABLE | PTE_NX, "Kernel .data/.bss");
    }
    serial_puts("[VMM] Kernel .data, .bss, and stacks mapped (RW, NX)\n");

    /* Explicitly unmap the dedicated guard pages below the active boot, IST1, and IST2 stacks */
    if (vmm_unmap_page(pml4, (uintptr_t)kernel_stack_guard) != VMM_OK ||
        vmm_unmap_page(pml4, gdt_get_ist1_guard()) != VMM_OK ||
        vmm_unmap_page(pml4, gdt_get_ist2_guard()) != VMM_OK) {
        serial_puts("[FATAL] Failed to unmap stack guards\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("[VMM] Guard pages below boot stack, IST1, and IST2 unmapped (hardware overflow trap armed)\n");

    /* 4. Map Linear Framebuffer explicitly with Cache-Disable (PTE_PCD) */
    if (boot_info->has_framebuffer) {
        uintptr_t fb_virt = boot_info->fb_address;
        uintptr_t fb_phys = fb_virt - hhdm_offset;
        size_t fb_size = (boot_info->fb_pitch * boot_info->fb_height + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (size_t p = 0; p < fb_size; p += PAGE_SIZE) {
            uintptr_t virt = fb_virt + p;
            uintptr_t phys = fb_phys + p;
            if (!vmm_is_mapped(pml4, virt)) {
                vmm_must_map(pml4, virt, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX | PTE_PCD, "Framebuffer MMIO");
            }
        }
        serial_puts("[VMM] Framebuffer video memory mapped (RW, NX, Cache-Disable)\n");
    }

    /* 5. Inspect required mappings before loading CR3 (Verification Checkpoint) */
    serial_puts("[VMM] Inspecting required address spaces prior to CR3 load:\n");
    bool text_ok        = vmm_is_mapped(pml4, (uintptr_t)__text_start);
    bool rodata_ok      = vmm_is_mapped(pml4, (uintptr_t)__rodata_start);
    bool data_ok        = vmm_is_mapped(pml4, (uintptr_t)__data_start);
    bool hhdm_ok        = vmm_is_mapped(pml4, (uintptr_t)phys_to_virt(0x100000));
    bool stack_guard_ok = !vmm_is_mapped(pml4, (uintptr_t)kernel_stack_guard);
    bool ist1_guard_ok  = !vmm_is_mapped(pml4, gdt_get_ist1_guard());
    bool ist1_layout_ok = (gdt_get_ist1_guard() % PAGE_SIZE == 0) &&
                         gdt_get_ist1_stack_top() == gdt_get_ist1_guard() + 5 * PAGE_SIZE;
    for (uintptr_t v = gdt_get_ist1_guard() + PAGE_SIZE;
         v < gdt_get_ist1_stack_top(); v += PAGE_SIZE) {
        if (!vmm_is_mapped(pml4, v)) ist1_layout_ok = false;
    }

    bool ist2_guard_ok  = !vmm_is_mapped(pml4, gdt_get_ist2_guard());
    bool ist2_layout_ok = (gdt_get_ist2_guard() % PAGE_SIZE == 0) &&
                         gdt_get_ist2_stack_top() == gdt_get_ist2_guard() + 5 * PAGE_SIZE;
    for (uintptr_t v = gdt_get_ist2_guard() + PAGE_SIZE;
         v < gdt_get_ist2_stack_top(); v += PAGE_SIZE) {
        if (!vmm_is_mapped(pml4, v)) ist2_layout_ok = false;
    }

    serial_puts("       Kernel .text:        "); serial_puts(text_ok        ? "[MAPPED RX]\n" : "[UNMAPPED]\n");
    serial_puts("       Kernel .rodata:      "); serial_puts(rodata_ok      ? "[MAPPED R, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       Kernel .data:        "); serial_puts(data_ok        ? "[MAPPED RW, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       PMM / HHDM:          "); serial_puts(hhdm_ok        ? "[MAPPED RW, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       Boot Stack Guard:    "); serial_puts(stack_guard_ok ? "[UNMAPPED OK]\n" : "[MAPPED ERROR!]\n");
    serial_puts("       IST1 Stack Guard:    "); serial_puts(ist1_guard_ok  ? "[UNMAPPED OK]\n" : "[MAPPED ERROR!]\n");
    serial_puts("       IST2 Stack Guard:    "); serial_puts(ist2_guard_ok  ? "[UNMAPPED OK]\n" : "[MAPPED ERROR!]\n");

    if (!text_ok || !rodata_ok || !data_ok || !hhdm_ok || !stack_guard_ok || !ist1_guard_ok || !ist1_layout_ok || !ist2_guard_ok || !ist2_layout_ok) {
        serial_puts("[FAIL] Pre-CR3 verification failed! Aborting switch.\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* 6. Switch CR3 to the new kernel PML4 */
    serial_puts("[VMM] Switching CR3 to kernel PML4 (Phys ");
    serial_print_hex(kernel_pml4_phys);
    serial_puts(")...\n");

    vmm_switch_pml4(kernel_pml4_phys);

    if (vmm_get_current_pml4() != kernel_pml4_phys) {
        serial_puts("[FAIL] VMM: Kernel CR3 readback mismatch\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    __atomic_store_n(&boot_memory_ready, true, __ATOMIC_RELEASE);
    if (!pmm_unlock_high_memory()) {
        serial_puts("[FAIL] PMM: High-memory unlock rejected\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    serial_puts("[ OK ] CR3 switch survived! Kernel running on independent 4-level page tables.\n\n");
    serial_puts("[PMM] High-memory allocation unlocked\n");
}
