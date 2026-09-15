#ifndef FORTRESS_BOOT_INFO_H
#define FORTRESS_BOOT_INFO_H

#include "types.h"
#include "limine.h"

#define MAX_BOOT_MEMMAP_ENTRIES 256

typedef struct {
    uint64_t hhdm_offset;
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;

    /* Framebuffer metadata */
    bool     has_framebuffer;
    uint64_t fb_address;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_pitch;
    uint16_t fb_bpp;

    /* Deep-copied memory map entries (kernel-owned) */
    size_t   memmap_entry_count;
    struct limine_memmap_entry memmap_entries[MAX_BOOT_MEMMAP_ENTRIES];

    /* ACPI RSDP physical address (Limine Base Revision 3) */
    bool     has_rsdp;
    uintptr_t rsdp_phys_addr;
} boot_info_t;

void boot_info_init(boot_info_t *out_info,
                    struct limine_memmap_response *memmap_resp,
                    struct limine_hhdm_response *hhdm_resp,
                    struct limine_kernel_address_response *kernel_addr_resp,
                    struct limine_framebuffer_response *fb_resp,
                    struct limine_rsdp_response *rsdp_resp);

#endif /* FORTRESS_BOOT_INFO_H */
