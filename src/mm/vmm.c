#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "gdt.h"

extern uint8_t __kernel_start[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];
extern uint8_t __kernel_end[];

static uint64_t  hhdm_offset = 0;
static uintptr_t kernel_pml4_phys = 0;

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

    uint64_t *new_table_virt = (uint64_t *)phys_to_virt(new_table_phys);
    memset(new_table_virt, 0, PAGE_SIZE);

    /* Link table into parent hierarchy */
    parent_table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return new_table_virt;
}

uintptr_t vmm_create_pml4(void) {
    uintptr_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        return 0;
    }

    uint64_t *pml4_virt = (uint64_t *)phys_to_virt(pml4_phys);
    memset(pml4_virt, 0, PAGE_SIZE);
    return pml4_phys;
}

int vmm_map_page(uint64_t *pml4_virt, uintptr_t virt_addr, uintptr_t phys_addr, uint64_t flags) {
    if (!pml4_virt) return VMM_ERR_INVALID_ADDR;
    if ((virt_addr % PAGE_SIZE) != 0 || (phys_addr % PAGE_SIZE) != 0) {
        return VMM_ERR_INVALID_ADDR;
    }
    if (!is_canonical_address(virt_addr)) {
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
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return VMM_OK;
}

int vmm_unmap_page(uint64_t *pml4_virt, uintptr_t virt_addr) {
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
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return VMM_OK;
}

bool vmm_is_mapped(uint64_t *pml4_virt, uintptr_t virt_addr) {
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

uintptr_t vmm_get_physical_address(uint64_t *pml4_virt, uintptr_t virt_addr) {
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

void vmm_switch_pml4(uintptr_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uintptr_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

uint64_t *vmm_get_kernel_pml4_virt(void) {
    if (kernel_pml4_phys == 0) return NULL;
    return (uint64_t *)phys_to_virt(kernel_pml4_phys);
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

    uint64_t *pml4 = (uint64_t *)phys_to_virt(kernel_pml4_phys);

    /* 2. Map Higher-Half Direct Map (HHDM) selectively for physical RAM regions */
    for (size_t i = 0; i < boot_info->memmap_entry_count; i++) {
        struct limine_memmap_entry *entry = &boot_info->memmap_entries[i];
        if (!should_map_in_hhdm(entry->type)) {
            continue;
        }

        uintptr_t start_phys = entry->base & ~(PAGE_SIZE - 1);
        uintptr_t end_phys   = (entry->base + entry->length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

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

    /* Explicitly unmap the dedicated guard pages below the active boot and IST1 stacks */
    if (vmm_unmap_page(pml4, (uintptr_t)kernel_stack_guard) != VMM_OK ||
        vmm_unmap_page(pml4, gdt_get_ist1_guard()) != VMM_OK) {
        serial_puts("[FATAL] Failed to unmap stack guards\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("[VMM] Guard pages below boot stack and IST1 unmapped (hardware overflow trap armed)\n");

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

    serial_puts("       Kernel .text:        "); serial_puts(text_ok        ? "[MAPPED RX]\n" : "[UNMAPPED]\n");
    serial_puts("       Kernel .rodata:      "); serial_puts(rodata_ok      ? "[MAPPED R, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       Kernel .data:        "); serial_puts(data_ok        ? "[MAPPED RW, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       PMM / HHDM:          "); serial_puts(hhdm_ok        ? "[MAPPED RW, NX]\n" : "[UNMAPPED]\n");
    serial_puts("       Boot Stack Guard:    "); serial_puts(stack_guard_ok ? "[UNMAPPED OK]\n" : "[MAPPED ERROR!]\n");
    serial_puts("       IST1 Stack Guard:    "); serial_puts(ist1_guard_ok  ? "[UNMAPPED OK]\n" : "[MAPPED ERROR!]\n");

    if (!text_ok || !rodata_ok || !data_ok || !hhdm_ok || !stack_guard_ok || !ist1_guard_ok || !ist1_layout_ok) {
        serial_puts("[FAIL] Pre-CR3 verification failed! Aborting switch.\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* 6. Switch CR3 to the new kernel PML4 */
    serial_puts("[VMM] Switching CR3 to kernel PML4 (Phys ");
    serial_print_hex(kernel_pml4_phys);
    serial_puts(")...\n");

    vmm_switch_pml4(kernel_pml4_phys);

    serial_puts("[ OK ] CR3 switch survived! Kernel running on independent 4-level page tables.\n\n");
}
