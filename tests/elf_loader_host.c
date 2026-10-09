#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"

__asm__(".pushsection .rodata\n.global sigrestorer_start,sigrestorer_end\nsigrestorer_start:\n.byte 0x90,0xc3\nsigrestorer_end:\n.popsection");
static void *frames[32];
static size_t allocated, mapped;
static int alloc_budget, map_budget;
static struct { uintptr_t va, phys; uint64_t flags; } leaves[32];
uintptr_t pmm_alloc_page(void) {
    if (alloc_budget-- == 0) return 0;
    void *p = aligned_alloc(4096,4096);
    assert(p && allocated < 32);
    memset(p,0xa5,4096);
    frames[allocated++] = p;
    return (uintptr_t)p;
}
void pmm_free_page(uintptr_t phys) {
    for (size_t i=0;i<allocated;i++) if (frames[i] == (void *)phys) {
        free(frames[i]); frames[i] = frames[--allocated]; return;
    }
    assert(0);
}
uintptr_t vmm_create_user_pml4(void) { return pmm_alloc_page(); }
void *vmm_phys_to_virt(uintptr_t phys) { return (void *)phys; }
int vmm_map_page(uint64_t *root, uintptr_t va, uintptr_t phys, uint64_t flags) {
    assert(root && mapped < 32);
    if (map_budget-- == 0) return VMM_ERR_NOMEM;
    leaves[mapped].va=va; leaves[mapped].phys=phys; leaves[mapped++].flags=flags;
    return VMM_OK;
}
int vmm_destroy_pml4(uintptr_t root, bool free_user_frames) {
    assert(root && free_user_frames);
    while (mapped) pmm_free_page(leaves[--mapped].phys);
    pmm_free_page(root);
    return VMM_OK;
}
void serial_puts(const char *s) { (void)s; }

int main(void) {
    uint8_t *image = calloc(1,16384);
    assert(image);
    Elf64_Ehdr *h=(Elf64_Ehdr *)image;
    h->e_ident[0]=0x7f; h->e_ident[1]='E'; h->e_ident[2]='L'; h->e_ident[3]='F';
    h->e_ident[EI_CLASS]=ELFCLASS64; h->e_ident[EI_DATA]=ELFDATA2LSB;
    h->e_ident[EI_VERSION]=EV_CURRENT; h->e_version=EV_CURRENT;
    h->e_type=ET_EXEC; h->e_machine=EM_X86_64;
    h->e_ehsize=sizeof(*h); h->e_phentsize=sizeof(Elf64_Phdr);
    h->e_phoff=sizeof(*h); h->e_phnum=2; h->e_entry=0x400003;
    Elf64_Phdr *p=(Elf64_Phdr *)(image+h->e_phoff);
    p[0]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_X,.p_offset=4099,.p_vaddr=0x400003,.p_filesz=5000,.p_memsz=9000};
    p[1]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_W,.p_offset=12288,.p_vaddr=0x500000,.p_filesz=4096,.p_memsz=4096};
    for (size_t i=4099;i<9099;i++) image[i]=(uint8_t)(i*13+7);
    for (size_t i=12288;i<16384;i++) image[i]=(uint8_t)(i*17+3);
    elf_loaded_process_t output;
    alloc_budget=map_budget=100;
    assert(elf_load_executable(image,16384,&output)==ELF_OK);
    assert(mapped==6 && allocated==7 && output.total_pages==6);
    for (size_t i=0;i<mapped;i++) {
        uint8_t *data=(void *)leaves[i].phys;
        uint64_t flags=PTE_PRESENT|PTE_USER;
        if (leaves[i].va==0x500000 || leaves[i].va==USER_STACK_PAGE_VIRT) flags|=PTE_WRITABLE|PTE_NX;
        assert(leaves[i].flags==flags);
        for (size_t j=0;j<4096;j++) {
            uintptr_t va=leaves[i].va+j;
            uint8_t expected=0;
            if (va>=0x400003 && va<0x400003+5000) expected=image[4099+va-0x400003];
            if (va>=0x500000 && va<0x501000) expected=image[12288+va-0x500000];
            if (va==USER_SIGRESTORER_VIRT) expected=0x90;
            if (va==USER_SIGRESTORER_VIRT+1) expected=0xc3;
            assert(data[j]==expected);
        }
    }
    vmm_destroy_pml4(output.pml4_phys,true);
    for (int mode=0;mode<2;mode++) for (int budget=0;budget<(mode ? 6 : 7);budget++) {
        alloc_budget=mode ? 100 : budget; map_budget=mode ? budget : 100;
        memset(&output,0xa5,sizeof(output));
        assert(elf_load_executable(image,16384,&output)==ELF_ERR_NOMEM);
        assert(allocated==0 && mapped==0 && output.pml4_phys==0 && output.total_pages==0);
    }
    alloc_budget=map_budget=100;
    p[1].p_flags|=PF_X;
    assert(elf_load_executable(image,16384,&output)==ELF_ERR_PERM && allocated==0);
    p[1].p_flags&=~PF_X;
    assert(elf_load_executable(image,16383,&output)==ELF_ERR_BOUNDS && allocated==0);
    free(image);
    puts("Actual ELF loader: exact poisoned-frame bytes/permissions, BSS, restorer, stack, allocation/map rollback PASS");
}
