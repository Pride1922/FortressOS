#include "boot_info.h"
#include "string.h"
#include "serial.h"

void boot_info_init(boot_info_t *out_info,
                    struct limine_memmap_response *memmap_resp,
                    struct limine_hhdm_response *hhdm_resp,
                    struct limine_kernel_address_response *kernel_addr_resp,
                    struct limine_framebuffer_response *fb_resp,
                    struct limine_rsdp_response *rsdp_resp) {
    if (!out_info) return;
    memset(out_info, 0, sizeof(boot_info_t));

    /* ACPI RSDP Physical Address (Limine Base Revision 3) */
    if (rsdp_resp && rsdp_resp->address) {
        out_info->has_rsdp = true;
        out_info->rsdp_phys_addr = (uintptr_t)rsdp_resp->address;
    }

    /* 1. HHDM Virtual Offset */
    if (hhdm_resp) {
        out_info->hhdm_offset = hhdm_resp->offset;
    } else {
        out_info->hhdm_offset = 0xFFFF800000000000ULL; /* Limine default */
    }

    /* 2. Kernel Load Address (Physical & Virtual) */
    if (kernel_addr_resp) {
        out_info->kernel_phys_base = kernel_addr_resp->physical_base;
        out_info->kernel_virt_base = kernel_addr_resp->virtual_base;
    } else {
        out_info->kernel_phys_base = 0x100000;          /* Fallback base */
        out_info->kernel_virt_base = 0xFFFFFFFF80000000ULL;
    }

    /* 3. Deep-copy Framebuffer Metadata */
    if (fb_resp && fb_resp->framebuffer_count > 0 && fb_resp->framebuffers[0]) {
        struct limine_framebuffer *fb = fb_resp->framebuffers[0];
        out_info->has_framebuffer = true;
        out_info->fb_address      = (uint64_t)fb->address;
        out_info->fb_width        = fb->width;
        out_info->fb_height       = fb->height;
        out_info->fb_pitch        = fb->pitch;
        out_info->fb_bpp          = fb->bpp;
    }

    /* 4. Deep-copy Memory Map Entries into kernel buffer */
    if (memmap_resp) {
        size_t count = (size_t)memmap_resp->entry_count;
        if (count > MAX_BOOT_MEMMAP_ENTRIES) {
            serial_puts("[FATAL] Bootloader memory map entry count (");
            serial_print_dec(count);
            serial_puts(") exceeds MAX_BOOT_MEMMAP_ENTRIES (");
            serial_print_dec(MAX_BOOT_MEMMAP_ENTRIES);
            serial_puts(")! Cannot preserve complete memory map.\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        out_info->memmap_entry_count = count;

        for (size_t i = 0; i < count; i++) {
            struct limine_memmap_entry *entry = memmap_resp->entries[i];
            if (entry) {
                out_info->memmap_entries[i].base   = entry->base;
                out_info->memmap_entries[i].length = entry->length;
                out_info->memmap_entries[i].type   = entry->type;
            }
        }
    }

    serial_puts("[ OK ] Boot metadata captured into kernel memory (deep copy complete)\n");
}
