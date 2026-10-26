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

    /* Initramfs module metadata */
    bool     has_initramfs;
    uint64_t initramfs_paddr;
    uint64_t initramfs_vaddr;
    uint64_t initramfs_size;

    /* Deep-copied kernel command line (kernel-owned) */
    char     cmdline[512];
    bool     cmdline_truncated;
    bool     installed_modules_valid;
    uint64_t readiness_vaddr, readiness_size;
    uint64_t kernel_file_vaddr, kernel_file_size;
    /* Installer admission needs complete, matching provenance for every module.
     * Unknown/TFTP origins fail closed; generic media must identify its GPT disk. */
    bool installer_source_valid, installer_source_optical;
    uint8_t installer_source_disk_guid[16];
    bool installer_payload_valid;
    uint64_t installer_payload_vaddr[5], installer_payload_size[5];
} boot_info_t;

void boot_info_init(boot_info_t *out_info,
                    struct limine_memmap_response *memmap_resp,
                    struct limine_hhdm_response *hhdm_resp,
                    struct limine_kernel_address_response *kernel_addr_resp,
                    struct limine_framebuffer_response *fb_resp,
                    struct limine_rsdp_response *rsdp_resp,
                    struct limine_module_response *module_resp,
                    struct limine_kernel_file_response *kernel_file_resp);

const boot_info_t *boot_info_get(void);

#endif /* FORTRESS_BOOT_INFO_H */
