#include "boot_info.h"
#include "string.h"
#include "serial.h"
#include "root_boot.h"

void boot_info_init(boot_info_t *out_info,
                    struct limine_memmap_response *memmap_resp,
                    struct limine_hhdm_response *hhdm_resp,
                    struct limine_kernel_address_response *kernel_addr_resp,
                    struct limine_framebuffer_response *fb_resp,
                    struct limine_rsdp_response *rsdp_resp,
                    struct limine_module_response *module_resp,
                    struct limine_kernel_file_response *kernel_file_resp) {
    if (!out_info) return;
    memset(out_info, 0, sizeof(boot_info_t));

    /* ACPI RSDP Physical Address (Limine Base Revision 3) */
    if (rsdp_resp && rsdp_resp->address) {
        out_info->has_rsdp = true;
        out_info->rsdp_phys_addr = (uintptr_t)rsdp_resp->address;
    }

    /* Snapshot Initramfs Module */
    if (module_resp && module_resp->module_count > 0 && module_resp->modules) {
        for (uint64_t i = 0; i < module_resp->module_count; i++) {
            struct limine_file *mod = module_resp->modules[i];
            if (mod && mod->address && mod->size > 0) {
                out_info->has_initramfs   = true;
                out_info->initramfs_vaddr = (uint64_t)mod->address;
                out_info->initramfs_size  = mod->size;
                /* If hhdm is available, calculate physical address */
                if (hhdm_resp) {
                    out_info->initramfs_paddr = (uint64_t)mod->address - hhdm_resp->offset;
                }
                break;
            }
        }
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

    /* 5. Snapshot Kernel Command Line */
    if (kernel_file_resp && kernel_file_resp->kernel_file && kernel_file_resp->kernel_file->cmdline) {
        const char *cmd = kernel_file_resp->kernel_file->cmdline;
        size_t len = 0;
        while (len < sizeof(out_info->cmdline) - 1 && cmd[len]) len++;
        out_info->cmdline_truncated = cmd[len] != '\0';
        memcpy(out_info->cmdline, cmd, len);
        out_info->cmdline[len] = '\0';
        out_info->kernel_file_vaddr=(uint64_t)kernel_file_resp->kernel_file->address;
        out_info->kernel_file_size=kernel_file_resp->kernel_file->size;
    } else {
        out_info->cmdline[0] = '\0';
    }

    root_boot_config_t root=root_boot_parse(out_info->cmdline,sizeof(out_info->cmdline),out_info->cmdline_truncated);
    if ((root.mode!=ROOT_BOOT_LIVE && root.mode!=ROOT_BOOT_INSTALLER) || root.has_root || !root.valid) {
        out_info->has_initramfs=false;
        bool good=module_resp && module_resp->modules && module_resp->module_count==2;
        if (module_resp && module_resp->modules && module_resp->module_count<=2) for (unsigned i=0;i<module_resp->module_count;i++) {
            struct limine_file *m=module_resp->modules[i];
            if (!m || !m->path || !m->cmdline || !m->address) {good=false;break;}
            const char *base=m->path;
            size_t path_len=0,role_len=0;
            while (path_len<256 && m->path[path_len]) {
                if (m->path[path_len]=='/') base=m->path+path_len+1;
                path_len++;
            }
            while (role_len<32 && m->cmdline[role_len]) role_len++;
            if (path_len==256 || role_len==32) {good=false;continue;}
            if (!strcmp(base,"bootstrap.tar") && !strcmp(m->cmdline,"role=bootstrap") &&
                m->size && m->size<=8u*1024*1024 && !out_info->has_initramfs) {
                out_info->has_initramfs=true;out_info->initramfs_vaddr=(uint64_t)m->address;
                out_info->initramfs_size=m->size;
                out_info->initramfs_paddr=hhdm_resp ? (uint64_t)m->address-hhdm_resp->offset : 0;
            } else if (!strcmp(base,"readiness.v1") && !strcmp(m->cmdline,"role=readiness") &&
                m->size==512 && !out_info->readiness_vaddr) {
                out_info->readiness_vaddr=(uint64_t)m->address;out_info->readiness_size=m->size;
            } else good=false;
        }
        out_info->installed_modules_valid=good && out_info->has_initramfs && out_info->readiness_vaddr;
    }

    if (root.valid && root.mode==ROOT_BOOT_INSTALLER && kernel_file_resp && kernel_file_resp->kernel_file) {
        struct limine_file *kernel=kernel_file_resp->kernel_file;
        bool optical=kernel->media_type==LIMINE_MEDIA_TYPE_OPTICAL;
        bool generic=kernel->media_type==LIMINE_MEDIA_TYPE_GENERIC;
        uint8_t guid[16];memcpy(guid,&kernel->gpt_disk_uuid,sizeof(guid));
        bool nonzero=false;for (unsigned i=0;i<16;i++) nonzero|=guid[i]!=0;
        bool good=(optical || (generic && nonzero)) && module_resp && module_resp->modules &&
            module_resp->module_count && module_resp->module_count<=16;
        if (good) for (uint64_t i=0;i<module_resp->module_count;i++) {
            struct limine_file *m=module_resp->modules[i];
            if (!m || !m->address || !m->size || m->media_type!=kernel->media_type ||
                (!optical && memcmp(&m->gpt_disk_uuid,guid,16))) {good=false;break;}
        }
        out_info->installer_source_valid=good;out_info->installer_source_optical=optical;
        if (good && !optical) memcpy(out_info->installer_source_disk_guid,guid,16);
        /* Exact typed payload set. Provenance alone does not admit arbitrary
         * modules as trusted installation inputs. Keep bootstrap backing alive. */
        bool payload=good && module_resp->module_count==6;
        unsigned found=0,bootstrap_count=0;
        const char *roles[]={"role=install-esp","role=install-root","role=install-bios","role=install-mbr","role=install-manifest"};
        const char *names[]={"esp.img","root.img","bios.img","mbr.bin","payload.v1"};
        const uint64_t sizes[]={64u*1024*1024,256u*1024*1024,1024u*1024,440,256};
        if (payload) for (uint64_t i=0;i<module_resp->module_count;i++) {
            struct limine_file *m=module_resp->modules[i];
            size_t path=0,role=0;const char *base=m->path;
            if (!m->path || !m->cmdline) {payload=false;break;}
            while (path<256 && m->path[path]) {if (m->path[path]=='/') base=m->path+path+1;path++;}
            while (role<32 && m->cmdline[role]) role++;
            if (path==256 || role==32) {payload=false;break;}
            if (!strcmp(base,"initramfs.tar") && !strcmp(m->cmdline,"role=bootstrap") &&
                m->size<=8u*1024*1024 && !bootstrap_count++) {
                out_info->initramfs_vaddr=(uint64_t)m->address;out_info->initramfs_size=m->size;
                out_info->initramfs_paddr=hhdm_resp ? (uint64_t)m->address-hhdm_resp->offset : 0;
                continue;
            }
            unsigned j=0;while (j<5 && strcmp(m->cmdline,roles[j])) j++;
            if (j==5 || strcmp(base,names[j]) || m->size!=sizes[j] || (found & (1u<<j))) {payload=false;break;}
            found|=1u<<j;out_info->installer_payload_vaddr[j]=(uint64_t)m->address;
            out_info->installer_payload_size[j]=m->size;
        }
        out_info->installer_payload_valid=payload && found==31 && bootstrap_count==1;
    }

    if (out_info->cmdline[0] != '\0') {
        serial_puts("[BOOT] Kernel cmdline: \"");
        serial_puts(out_info->cmdline);
        serial_puts("\"\n");
    } else {
        serial_puts("[BOOT] Kernel cmdline: (none)\n");
    }

    serial_puts("[ OK ] Boot metadata captured into kernel memory (deep copy complete)\n");
}
