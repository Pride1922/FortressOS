#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "serial.h"

int elf_load_executable(const void *image, size_t image_size, elf_loaded_process_t *out_proc) {
    if (out_proc) {
        memset(out_proc, 0, sizeof(elf_loaded_process_t));
    }
    if (!image || !out_proc) {
        return ELF_ERR_INVALID;
    }
    if (image_size < sizeof(Elf64_Ehdr)) {
        return ELF_ERR_INVALID;
    }

    const uint8_t *img_bytes = (const uint8_t *)image;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)image;

    /* 1. Header Validation */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT || ehdr->e_version != EV_CURRENT) {
        return ELF_ERR_INVALID;
    }

    /* Narrow Format Contract 1: Strictly ET_EXEC only (no dynamic relocations / PIE) */
    if (ehdr->e_type != ET_EXEC) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_machine != EM_X86_64) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_ehsize != sizeof(Elf64_Ehdr)) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return ELF_ERR_INVALID;
    }
    if (ehdr->e_phnum == 0 || ehdr->e_phnum > MAX_ELF_PHNUM) {
        return ELF_ERR_INVALID;
    }

    /* Overflow-safe program header table bounds check */
    if (ehdr->e_phoff > image_size ||
        (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > image_size - ehdr->e_phoff) {
        return ELF_ERR_BOUNDS;
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(img_bytes + ehdr->e_phoff);

    /* 2. Program Header Scanning & Safety Pre-Validation */
    size_t load_seg_count = 0;
    size_t total_pages = 0;

    typedef struct {
        uintptr_t start_page;
        uintptr_t end_page;
    } seg_range_t;
    seg_range_t loaded_ranges[MAX_ELF_PHNUM];

    bool entry_in_exec_seg = false;

    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *p = &phdrs[i];

        /* Narrow Format Contract 2: Reject dynamic interpreter request (PT_INTERP) */
        if (p->p_type == PT_INTERP) {
            return ELF_ERR_INVALID;
        }
        if (p->p_type != PT_LOAD) {
            continue;
        }

        /* Zero-sized segment handling */
        if (p->p_memsz == 0) {
            continue;
        }

        /* Narrow Format Contract 3: W^X enforcement (reject writable AND executable) */
        if ((p->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            return ELF_ERR_PERM;
        }

        /* Overflow-safe file bounds check */
        if (p->p_offset > image_size || p->p_filesz > image_size - p->p_offset) {
            return ELF_ERR_BOUNDS;
        }
        if (p->p_filesz > p->p_memsz) {
            return ELF_ERR_BOUNDS;
        }

        /* Narrow Format Contract 4: Reject page-zero mapping */
        if (p->p_vaddr < PAGE_SIZE) {
            return ELF_ERR_PERM;
        }

        /* Lower-half user space bounds check */
        if (p->p_vaddr >= USER_SPACE_LIMIT || p->p_memsz > USER_SPACE_LIMIT - p->p_vaddr) {
            return ELF_ERR_BOUNDS;
        }

        /* Congruence check: p_vaddr and p_offset must have identical offset within page */
        if ((p->p_vaddr & (PAGE_SIZE - 1)) != (p->p_offset & (PAGE_SIZE - 1))) {
            return ELF_ERR_BOUNDS;
        }

        uintptr_t seg_start_page = p->p_vaddr & ~(PAGE_SIZE - 1);
        uintptr_t seg_end_page   = (p->p_vaddr + p->p_memsz - 1) & ~(PAGE_SIZE - 1);

        /* Stack & guard page collision check */
        if (seg_end_page >= USER_STACK_GUARD_VIRT && seg_start_page < USER_STACK_TOP_VIRT) {
            return ELF_ERR_OVERLAP;
        }

        /* Non-overlapping segments check */
        for (size_t r = 0; r < load_seg_count; r++) {
            if (!(seg_end_page < loaded_ranges[r].start_page || seg_start_page > loaded_ranges[r].end_page)) {
                return ELF_ERR_OVERLAP;
            }
        }

        /* Total page allocation bound check */
        size_t seg_pages = (seg_end_page - seg_start_page) / PAGE_SIZE + 1;
        if (seg_pages > MAX_ELF_PAGES - total_pages) {
            return ELF_ERR_NOMEM;
        }
        total_pages += seg_pages;

        /* Check entry point coverage in an executable segment */
        if ((p->p_flags & PF_X) &&
            ehdr->e_entry >= p->p_vaddr &&
            ehdr->e_entry < p->p_vaddr + p->p_memsz) {
            entry_in_exec_seg = true;
        }

        loaded_ranges[load_seg_count].start_page = seg_start_page;
        loaded_ranges[load_seg_count].end_page   = seg_end_page;
        load_seg_count++;
    }

    if (load_seg_count == 0) {
        return ELF_ERR_INVALID;
    }

    /* Narrow Format Contract 5: Entry point must fall inside an executable segment */
    if (!entry_in_exec_seg) {
        return ELF_ERR_PERM;
    }

    /* 3. Address Space Creation */
    uintptr_t pml4_phys = vmm_create_user_pml4();
    if (pml4_phys == 0) {
        return ELF_ERR_NOMEM;
    }
    uint64_t *pml4_virt = (uint64_t *)vmm_phys_to_virt(pml4_phys);

    /* 4. Segment Loading Pass with Frame Ownership & Rollback */
    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;

        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (p->p_flags & PF_W) flags |= PTE_WRITABLE;
        if (!(p->p_flags & PF_X)) flags |= PTE_NX;

        uintptr_t seg_start_page = p->p_vaddr & ~(PAGE_SIZE - 1);
        uintptr_t seg_end_page   = (p->p_vaddr + p->p_memsz - 1) & ~(PAGE_SIZE - 1);

        for (uintptr_t page = seg_start_page; page <= seg_end_page; page += PAGE_SIZE) {
            uintptr_t frame_phys = pmm_alloc_page();
            if (frame_phys == 0) {
                vmm_destroy_pml4(pml4_phys, true);
                return ELF_ERR_NOMEM;
            }

            uint8_t *frame_virt = (uint8_t *)vmm_phys_to_virt(frame_phys);
            memset(frame_virt, 0, PAGE_SIZE);

            /* Calculate data slice overlapping this page from the ELF file */
            uintptr_t page_file_start = (page < p->p_vaddr) ? p->p_vaddr : page;
            uintptr_t seg_file_end    = p->p_vaddr + p->p_filesz;
            uintptr_t page_file_end   = (page + PAGE_SIZE < seg_file_end) ? page + PAGE_SIZE : seg_file_end;

            if (page_file_end > page_file_start) {
                size_t copy_len = page_file_end - page_file_start;
                size_t file_offset = p->p_offset + (page_file_start - p->p_vaddr);
                size_t page_dest_offset = page_file_start - page;
                memcpy(frame_virt + page_dest_offset, img_bytes + file_offset, copy_len);
            }

            int map_res = vmm_map_page(pml4_virt, page, frame_phys, flags);
            if (map_res != VMM_OK) {
                /* Explicit rollback: free unmapped frame before destroying PML4 */
                pmm_free_page(frame_phys);
                vmm_destroy_pml4(pml4_phys, true);
                return ELF_ERR_NOMEM;
            }
        }
    }

    /* 5. User Stack Allocation */
    uintptr_t stack_phys = pmm_alloc_page();
    if (stack_phys == 0) {
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    uint8_t *stack_virt = (uint8_t *)vmm_phys_to_virt(stack_phys);
    memset(stack_virt, 0, PAGE_SIZE);

    int stack_map_res = vmm_map_page(pml4_virt, USER_STACK_PAGE_VIRT, stack_phys,
                                     PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    if (stack_map_res != VMM_OK) {
        pmm_free_page(stack_phys);
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    total_pages++;

    /* 6. Success: Publish output descriptor */
    out_proc->pml4_phys      = pml4_phys;
    out_proc->entry_point    = ehdr->e_entry;
    out_proc->user_stack_top = USER_STACK_TOP_VIRT;
    out_proc->total_pages    = total_pages;

    return ELF_OK;
}
