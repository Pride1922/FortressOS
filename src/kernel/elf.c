#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "serial.h"
#include "spawn_profile.h"
#include "elf_page.h"
#include "thread.h"

/* Restorer stub symbols from sigrestorer.asm (linked into the kernel image). */
extern uint8_t sigrestorer_start[];
extern uint8_t sigrestorer_end[];

int elf_load_executable(const void *image, size_t image_size, elf_loaded_process_t *out_proc) {
    return elf_load_executable_profile(image, image_size, out_proc, NULL);
}

int elf_load_executable_profile(const void *image, size_t image_size, elf_loaded_process_t *out_proc, spawn_profile_t *profile) {
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

    /* Reserved regions that ELF segments must not overlap (page-rounded). */
    const uintptr_t RESTORER_PAGE      = USER_SIGRESTORER_VIRT;
    const uintptr_t RESTORER_PAGE_END  = USER_SIGRESTORER_VIRT + USER_SIGRESTORER_SIZE - 1;
    const uintptr_t GUARD_PAGE         = USER_STACK_GUARD_VIRT;
    const uintptr_t STACK_TOP          = USER_STACK_TOP_VIRT;

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
        if (seg_end_page >= GUARD_PAGE && seg_start_page < STACK_TOP) {
            return ELF_ERR_OVERLAP;
        }

        /* Restorer page collision check. */
        if (!(seg_end_page < RESTORER_PAGE || seg_start_page > RESTORER_PAGE_END)) {
            serial_puts("[ELF] Segment overlaps reserved restorer page\n");
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
    uint64_t phase_begin = spawn_profile_clock(profile);
    uintptr_t pml4_phys = (spawn_get_fault_type() == SPAWN_FAULT_VMM_USER_PML4)
        ? (spawn_record_fault_hit(), 0)
        : vmm_create_user_pml4();
    SPAWN_ADD(profile, elf[SE_SPACE], phase_begin);
    if (pml4_phys == 0) {
        return ELF_ERR_NOMEM;
    }
    uint64_t *pml4_virt = (uint64_t *)vmm_phys_to_virt(pml4_phys);

    /* 4. Segment Loading Pass with Frame Ownership & Rollback */
    size_t seg_page_idx = 0;
    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;

        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (p->p_flags & PF_W) flags |= PTE_WRITABLE;
        if (!(p->p_flags & PF_X)) flags |= PTE_NX;

        uintptr_t seg_start_page = p->p_vaddr & ~(PAGE_SIZE - 1);
        uintptr_t seg_end_page   = (p->p_vaddr + p->p_memsz - 1) & ~(PAGE_SIZE - 1);

        for (uintptr_t page = seg_start_page; page <= seg_end_page; page += PAGE_SIZE) {
            phase_begin = spawn_profile_clock(profile);
            bool fail_seg_pmm = (spawn_get_fault_type() == SPAWN_FAULT_ELF_SEGMENT_PMM &&
                                 seg_page_idx == spawn_get_fault_trigger());
            uintptr_t frame_phys = fail_seg_pmm
                ? (spawn_record_fault_hit(), 0)
                : pmm_alloc_page();
            SPAWN_ADD(profile, elf[SE_ALLOC], phase_begin);
            if (frame_phys == 0) {
                vmm_destroy_pml4(pml4_phys, true);
                return ELF_ERR_NOMEM;
            }

            phase_begin = spawn_profile_clock(profile);
            uint8_t *frame_virt = (uint8_t *)vmm_phys_to_virt(frame_phys);

            /* Calculate data slice overlapping this page from the ELF file */
            uintptr_t page_file_start = (page < p->p_vaddr) ? p->p_vaddr : page;
            uintptr_t seg_file_end    = p->p_vaddr + p->p_filesz;
            uintptr_t page_file_end   = (page + PAGE_SIZE < seg_file_end) ? page + PAGE_SIZE : seg_file_end;

            size_t copy_len = page_file_end > page_file_start ? page_file_end - page_file_start : 0;
            size_t page_dest_offset = copy_len ? page_file_start - page : 0;
            const uint8_t *source = copy_len
                ? img_bytes + p->p_offset + (page_file_start - p->p_vaddr) : img_bytes;
            elf_page_init(frame_virt, source, page_dest_offset, copy_len);

            SPAWN_ADD(profile, elf[SE_COPY], phase_begin);
            phase_begin = spawn_profile_clock(profile);
            bool fail_seg_map = (spawn_get_fault_type() == SPAWN_FAULT_ELF_SEGMENT_MAP &&
                                 seg_page_idx == spawn_get_fault_trigger());
            int map_res = fail_seg_map
                ? (spawn_record_fault_hit(), VMM_ERR_NOMEM)
                : vmm_map_page(pml4_virt, page, frame_phys, flags);
            SPAWN_ADD(profile, elf[SE_MAP], phase_begin);
            if (map_res != VMM_OK) {
                /* Explicit rollback: free unmapped frame before destroying PML4 */
                pmm_free_page(frame_phys);
                vmm_destroy_pml4(pml4_phys, true);
                return ELF_ERR_NOMEM;
            }
            seg_page_idx++;
        }
    }

    /* 5. Restorer Page Allocation & Installation (RX, user, no write).
     * Stub bytes come from sigrestorer_start/sigrestorer_end linked into kernel. */
    size_t restorer_stub_size = (size_t)(sigrestorer_end - sigrestorer_start);
    if (restorer_stub_size == 0 || restorer_stub_size > PAGE_SIZE) {
        /* Should never happen — NASM %if guard rejects oversized stub. */
        serial_puts("[ELF] Restorer stub size invalid\n");
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_INVALID;
    }

    phase_begin = spawn_profile_clock(profile);
    uintptr_t restorer_phys = (spawn_get_fault_type() == SPAWN_FAULT_SIGRESTORER_PMM)
        ? (spawn_record_fault_hit(), 0)
        : pmm_alloc_page();
    SPAWN_ADD(profile, elf[SE_ALLOC], phase_begin);
    if (restorer_phys == 0) {
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    phase_begin = spawn_profile_clock(profile);
    uint8_t *restorer_kvirt = (uint8_t *)vmm_phys_to_virt(restorer_phys);
    elf_page_init(restorer_kvirt, sigrestorer_start, 0, restorer_stub_size);

    SPAWN_ADD(profile, elf[SE_COPY], phase_begin);
    phase_begin = spawn_profile_clock(profile);
    /* PTE: present | user | executable (no PTE_WRITABLE, no PTE_NX). */
    int restorer_map = (spawn_get_fault_type() == SPAWN_FAULT_SIGRESTORER_MAP)
        ? (spawn_record_fault_hit(), VMM_ERR_NOMEM)
        : vmm_map_page(pml4_virt, USER_SIGRESTORER_VIRT, restorer_phys,
                       PTE_PRESENT | PTE_USER);
    SPAWN_ADD(profile, elf[SE_MAP], phase_begin);
    if (restorer_map != VMM_OK) {
        pmm_free_page(restorer_phys);
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    total_pages++;

    /* 6. User Stack Allocation */
    phase_begin = spawn_profile_clock(profile);
    uintptr_t stack_phys = (spawn_get_fault_type() == SPAWN_FAULT_USER_STACK_PMM)
        ? (spawn_record_fault_hit(), 0)
        : pmm_alloc_page();
    SPAWN_ADD(profile, elf[SE_ALLOC], phase_begin);
    if (stack_phys == 0) {
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    phase_begin = spawn_profile_clock(profile);
    uint8_t *stack_virt = (uint8_t *)vmm_phys_to_virt(stack_phys);
    elf_page_init(stack_virt, img_bytes, 0, 0);

    SPAWN_ADD(profile, elf[SE_COPY], phase_begin);
    phase_begin = spawn_profile_clock(profile);
    int stack_map_res = (spawn_get_fault_type() == SPAWN_FAULT_USER_STACK_MAP)
        ? (spawn_record_fault_hit(), VMM_ERR_NOMEM)
        : vmm_map_page(pml4_virt, USER_STACK_PAGE_VIRT, stack_phys,
                       PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    SPAWN_ADD(profile, elf[SE_MAP], phase_begin);
    if (stack_map_res != VMM_OK) {
        pmm_free_page(stack_phys);
        vmm_destroy_pml4(pml4_phys, true);
        return ELF_ERR_NOMEM;
    }
    total_pages++;

    /* 7. Success: Publish output descriptor */
    out_proc->pml4_phys      = pml4_phys;
    out_proc->entry_point    = ehdr->e_entry;
    out_proc->user_stack_top = USER_STACK_TOP_VIRT;
    out_proc->stack_phys     = stack_phys;
    out_proc->total_pages    = total_pages;

    return ELF_OK;
}
