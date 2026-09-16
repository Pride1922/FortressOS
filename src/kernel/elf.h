#ifndef FORTRESS_ELF_H
#define FORTRESS_ELF_H

#include "types.h"

/* ELF Identification Indexes */
#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define EI_OSABI       7
#define EI_ABIVERSION  8
#define EI_NIDENT      16

/* ELF Magic Values */
#define ELFMAG0        0x7F
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'

/* ELF Classes */
#define ELFCLASSNONE   0
#define ELFCLASS32     1
#define ELFCLASS64     2

/* ELF Data Encodings */
#define ELFDATANONE    0
#define ELFDATA2LSB    1  /* Little-endian */
#define ELFDATA2MSB    2  /* Big-endian */

/* ELF Types */
#define ET_NONE        0
#define ET_REL         1  /* Relocatable */
#define ET_EXEC        2  /* Executable (Static) */
#define ET_DYN         3  /* Shared Object / Position-Independent Executable */
#define ET_CORE        4  /* Core Dump */

/* ELF Machine Architectures */
#define EM_NONE        0
#define EM_X86_64      62 /* AMD x86-64 */

/* ELF Versions */
#define EV_CURRENT     1

/* Segment Types (p_type) */
#define PT_NULL        0
#define PT_LOAD        1
#define PT_DYNAMIC     2
#define PT_INTERP      3
#define PT_NOTE        4
#define PT_SHLIB       5
#define PT_PHDR        6
#define PT_TLS         7
#define PT_GNU_EH_FRAME 0x6474E550
#define PT_GNU_STACK    0x6474E551
#define PT_GNU_RELRO    0x6474E552

/* Segment Flags (p_flags) */
#define PF_X           0x1  /* Executable */
#define PF_W           0x2  /* Writable */
#define PF_R           0x4  /* Readable */

/* 64-bit ELF File Header */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* 64-bit ELF Program Header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* Architectural Limits & Reserved Virtual Regions */
#define MAX_ELF_PHNUM          16
#define MAX_ELF_PAGES          1024  /* Max 4 MiB total memory mapped per process */
#define USER_SPACE_LIMIT       0x0000800000000000ULL /* Lower 128 TiB canonical */
#define USER_STACK_PAGE_VIRT   0x00007FFFF0000000ULL
#define USER_STACK_TOP_VIRT    0x00007FFFF0001000ULL
#define USER_STACK_GUARD_VIRT  0x00007FFFEFFFF000ULL

/* Loader Error Codes */
#define ELF_OK                 0
#define ELF_ERR_INVALID       -1  /* Malformed header, bad magic, unsupported class/machine/type */
#define ELF_ERR_BOUNDS        -2  /* Out-of-bounds offsets, non-canonical virtual addresses, overflow */
#define ELF_ERR_PERM          -3  /* W^X violation, non-executable entry point, page-zero mapping */
#define ELF_ERR_NOMEM         -4  /* Frame allocation failure, page limit exceeded */
#define ELF_ERR_OVERLAP       -5  /* Overlapping segments or stack collision */

/* Loaded Process Descriptor */
typedef struct {
    uintptr_t pml4_phys;       /* Physical base of process PML4 */
    uintptr_t entry_point;     /* Virtual entry point (e_entry) */
    uintptr_t user_stack_top;  /* Virtual top of user stack */
    size_t    total_pages;     /* Total physical pages allocated for image */
} elf_loaded_process_t;

/* Public Loader API */
int elf_load_executable(const void *image, size_t image_size, elf_loaded_process_t *out_proc);

#endif /* FORTRESS_ELF_H */
