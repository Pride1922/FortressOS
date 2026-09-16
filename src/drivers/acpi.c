#include "acpi.h"
#include "vmm.h"
#include "string.h"
#include "serial.h"

#define PAGE_SIZE 4096

static uintptr_t g_hhdm_offset = 0;
static acpi_rsdp_t *g_rsdp = NULL;
static acpi_sdt_header_t *g_root_sdt = NULL;
static bool g_is_xsdt = false;

bool acpi_ensure_mapped(uintptr_t phys_addr, size_t length) {
    if (phys_addr == 0 || length == 0 || g_hhdm_offset == 0 ||
        length > UINTPTR_MAX - phys_addr ||
        phys_addr + length > UINTPTR_MAX - (PAGE_SIZE - 1) ||
        phys_addr + length > UINTPTR_MAX - g_hhdm_offset) {
        return false;
    }

    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    uintptr_t start_page = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_page   = (phys_addr + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uintptr_t page = start_page; page < end_page; page += PAGE_SIZE) {
        uintptr_t virt = page + g_hhdm_offset;
        if (vmm_is_mapped(pml4, virt)) {
            if (vmm_get_physical_address(pml4, virt) != page) return false;
        } else {
            /* Map firmware page in HHDM with NX */
            int res = vmm_map_page(pml4, virt, page, PTE_PRESENT | PTE_NX);
            if (res != VMM_OK && res != VMM_ERR_ALREADY_MAPPED) {
                serial_puts("[FAIL] acpi_ensure_mapped: failed to map page ");
                serial_print_hex(page);
                serial_puts("\n");
                return false;
            }
        }
    }
    return true;
}

bool acpi_validate_checksum(const acpi_sdt_header_t *header) {
    if (!header || header->length < sizeof(acpi_sdt_header_t) || header->length > MAX_ACPI_TABLE_SIZE) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)header;
    uint8_t sum = 0;
    for (size_t i = 0; i < header->length; i++) {
        sum += bytes[i];
    }
    return (sum == 0);
}

static bool validate_rsdp_checksum(const acpi_rsdp_t *rsdp) {
    const uint8_t *bytes = (const uint8_t *)rsdp;
    uint8_t sum = 0;
    for (size_t i = 0; i < 20; i++) {
        sum += bytes[i];
    }
    if (sum != 0) {
        return false;
    }
    if (rsdp->revision >= 2) {
        if (rsdp->length < sizeof(acpi_rsdp_t) || rsdp->length > 256) {
            return false;
        }
        sum = 0;
        for (size_t i = 0; i < rsdp->length; i++) {
            sum += bytes[i];
        }
        if (sum != 0) {
            return false;
        }
    }
    return true;
}

bool acpi_init(uintptr_t rsdp_phys_addr, uintptr_t hhdm_offset) {
    g_root_sdt = NULL;
    g_is_xsdt = false;
    g_hhdm_offset = hhdm_offset;
    if (rsdp_phys_addr == 0) {
        serial_puts("[FAIL] ACPI init failed: RSDP physical address is 0\n");
        return false;
    }

    /* 1. Ensure RSDP header is mapped */
    if (!acpi_ensure_mapped(rsdp_phys_addr, sizeof(acpi_rsdp_t))) {
        serial_puts("[FAIL] Failed to map ACPI RSDP\n");
        return false;
    }

    g_rsdp = (acpi_rsdp_t *)(rsdp_phys_addr + g_hhdm_offset);

    /* 2. Verify RSDP Signature */
    if (memcmp(g_rsdp->signature, "RSD PTR ", 8) != 0) {
        serial_puts("[FAIL] ACPI RSDP signature mismatch\n");
        return false;
    }

    if (g_rsdp->revision >= 2 &&
        (g_rsdp->length < sizeof(acpi_rsdp_t) || g_rsdp->length > 256 ||
         !acpi_ensure_mapped(rsdp_phys_addr, g_rsdp->length))) return false;

    /* 3. Verify RSDP Checksum */
    if (!validate_rsdp_checksum(g_rsdp)) {
        serial_puts("[FAIL] ACPI RSDP checksum mismatch\n");
        return false;
    }

    serial_puts("[ OK ] ACPI RSDP verified (Revision ");
    serial_print_dec(g_rsdp->revision);
    serial_puts(", OEM: ");
    for (int i = 0; i < 6; i++) {
        serial_putc(g_rsdp->oem_id[i] ? g_rsdp->oem_id[i] : ' ');
    }
    serial_puts(")\n");

    /* 4. Prefer XSDT (ACPI 2.0+) over RSDT (ACPI 1.0) */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        uintptr_t xsdt_phys = (uintptr_t)g_rsdp->xsdt_address;
        if (acpi_ensure_mapped(xsdt_phys, sizeof(acpi_sdt_header_t))) {
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(xsdt_phys + g_hhdm_offset);
            if (hdr->length >= sizeof(acpi_sdt_header_t) && hdr->length <= MAX_ACPI_TABLE_SIZE) {
                if (acpi_ensure_mapped(xsdt_phys, hdr->length)) {
                    /* Check XSDT payload divisibility by sizeof(uint64_t) */
                    size_t payload_len = hdr->length - sizeof(acpi_sdt_header_t);
                    if (payload_len % sizeof(uint64_t) == 0 &&
                        memcmp(hdr->signature, "XSDT", 4) == 0 &&
                        acpi_validate_checksum(hdr)) {
                        g_root_sdt = hdr;
                        g_is_xsdt = true;
                        serial_puts("[ OK ] ACPI 2.0+ XSDT discovered, validated, and mapped\n");
                        return true;
                    }
                }
            }
        }
        serial_puts("[WARN] XSDT validation failed, attempting RSDT fallback\n");
    }

    /* 5. Fallback to 32-bit RSDT */
    if (g_rsdp->rsdt_address != 0) {
        uintptr_t rsdt_phys = (uintptr_t)g_rsdp->rsdt_address;
        if (acpi_ensure_mapped(rsdt_phys, sizeof(acpi_sdt_header_t))) {
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(rsdt_phys + g_hhdm_offset);
            if (hdr->length >= sizeof(acpi_sdt_header_t) && hdr->length <= MAX_ACPI_TABLE_SIZE) {
                if (acpi_ensure_mapped(rsdt_phys, hdr->length)) {
                    /* Check RSDT payload divisibility by sizeof(uint32_t) */
                    size_t payload_len = hdr->length - sizeof(acpi_sdt_header_t);
                    if (payload_len % sizeof(uint32_t) == 0 &&
                        memcmp(hdr->signature, "RSDT", 4) == 0 &&
                        acpi_validate_checksum(hdr)) {
                        g_root_sdt = hdr;
                        g_is_xsdt = false;
                        serial_puts("[ OK ] ACPI 1.0 RSDT discovered, validated, and mapped\n");
                        return true;
                    }
                }
            }
        }
    }

    serial_puts("[FAIL] No valid ACPI root table (XSDT/RSDT) found\n");
    return false;
}

acpi_sdt_header_t *acpi_find_table(const char *signature) {
    if (!g_root_sdt || !signature) {
        return NULL;
    }

    if (g_is_xsdt) {
        size_t entries = (g_root_sdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
        uint8_t *table_ptrs = (uint8_t *)((uint8_t *)g_root_sdt + sizeof(acpi_sdt_header_t));
        for (size_t i = 0; i < entries; i++) {
            uintptr_t phys = 0;
            memcpy(&phys, (uint8_t *)table_ptrs + i * (g_is_xsdt ? 8 : 4), g_is_xsdt ? 8 : 4);
            if (!acpi_ensure_mapped(phys, sizeof(acpi_sdt_header_t))) continue;
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(phys + g_hhdm_offset);
            if (hdr->length < sizeof(acpi_sdt_header_t) || hdr->length > MAX_ACPI_TABLE_SIZE) continue;
            if (!acpi_ensure_mapped(phys, hdr->length)) continue;

            if (memcmp(hdr->signature, signature, 4) == 0 && acpi_validate_checksum(hdr)) {
                return hdr;
            }
        }
    } else {
        size_t entries = (g_root_sdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)g_root_sdt + sizeof(acpi_sdt_header_t));
        for (size_t i = 0; i < entries; i++) {
            uintptr_t phys = 0;
            memcpy(&phys, (uint8_t *)table_ptrs + i * (g_is_xsdt ? 8 : 4), g_is_xsdt ? 8 : 4);
            if (!acpi_ensure_mapped(phys, sizeof(acpi_sdt_header_t))) continue;
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(phys + g_hhdm_offset);
            if (hdr->length < sizeof(acpi_sdt_header_t) || hdr->length > MAX_ACPI_TABLE_SIZE) continue;
            if (!acpi_ensure_mapped(phys, hdr->length)) continue;

            if (memcmp(hdr->signature, signature, 4) == 0 && acpi_validate_checksum(hdr)) {
                return hdr;
            }
        }
    }

    return NULL;
}

bool acpi_parse_madt(acpi_madt_info_t *out_info) {
    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");
    if (!madt) return false;
    return acpi_parse_madt_buffer(madt, madt->header.length, out_info);
}

/* Buffer must be accessible for available bytes; used for firmware and fixtures. */
bool acpi_parse_madt_buffer(const void *buffer, size_t available, acpi_madt_info_t *out_info) {
    if (!buffer || !out_info || available < sizeof(acpi_madt_t)) return false;
    const acpi_madt_t *madt = buffer;
    if (madt->header.length > available || memcmp(madt->header.signature, "APIC", 4)) return false;
    memset(out_info, 0, sizeof(*out_info));
    if (!acpi_validate_checksum(&madt->header)) {
        serial_puts("[FAIL] MADT table checksum validation failed\n");
        return false;
    }

    if (madt->header.length < sizeof(acpi_madt_t)) {
        serial_puts("[FAIL] MADT table too small\n");
        return false;
    }

    out_info->lapic_phys_addr = madt->lapic_address;
    out_info->pcat_compat     = (madt->flags & 1) != 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(acpi_madt_t);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        if ((size_t)(end - ptr) < sizeof(acpi_madt_entry_t)) return false;
        acpi_madt_entry_t *entry = (acpi_madt_entry_t *)ptr;
        if (entry->length < sizeof(acpi_madt_entry_t) || entry->length > (size_t)(end - ptr)) {
            serial_puts("[WARN] Malformed MADT entry encountered, terminating parse\n");
            return false;
        }
        static const uint8_t minimum[] = {8, 12, 10, 8, 6, 12};
        if (entry->type < sizeof(minimum) && entry->length < minimum[entry->type]) return false;
        if ((entry->type == 1 && out_info->ioapic_count == MAX_DETECTED_IOAPICS) ||
            (entry->type == 2 && out_info->iso_count == MAX_DETECTED_ISOS)) return false;

        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                if (entry->length >= sizeof(acpi_madt_lapic_entry_t)) {
                    acpi_madt_lapic_entry_t *lapic = (acpi_madt_lapic_entry_t *)entry;
                    if (lapic->flags & 1) {
                        /* Enabled CPU */
                        if (out_info->enabled_cpu_count == MAX_DETECTED_CPUS) return false;
                        if (out_info->enabled_cpu_count < MAX_DETECTED_CPUS) {
                            out_info->enabled_cpu_processor_ids[out_info->enabled_cpu_count] = lapic->processor_id;
                            out_info->enabled_cpu_apic_ids[out_info->enabled_cpu_count++] = lapic->apic_id;
                        }
                    } else if (lapic->flags & 2) {
                        /* Online Capable CPU (ACPI 6.3+) */
                        if (out_info->online_capable_cpu_count == MAX_DETECTED_CPUS) return false;
                        if (out_info->online_capable_cpu_count < MAX_DETECTED_CPUS) {
                            out_info->online_capable_cpu_apic_ids[out_info->online_capable_cpu_count++] = lapic->apic_id;
                        }
                    }
                }
                break;
            }
            case MADT_TYPE_IO_APIC: {
                if (entry->length >= sizeof(acpi_madt_ioapic_entry_t) &&
                    out_info->ioapic_count < MAX_DETECTED_IOAPICS) {
                    acpi_madt_ioapic_entry_t *ioapic = (acpi_madt_ioapic_entry_t *)entry;
                    out_info->ioapics[out_info->ioapic_count].id = ioapic->ioapic_id;
                    out_info->ioapics[out_info->ioapic_count].phys_addr = ioapic->ioapic_address;
                    out_info->ioapics[out_info->ioapic_count].gsi_base = ioapic->gsi_base;
                    out_info->ioapic_count++;
                }
                break;
            }
            case MADT_TYPE_INTERRUPT_OVERRIDE: {
                if (entry->length >= sizeof(acpi_madt_iso_entry_t) &&
                    out_info->iso_count < MAX_DETECTED_ISOS) {
                    acpi_madt_iso_entry_t *iso = (acpi_madt_iso_entry_t *)entry;
                    out_info->isos[out_info->iso_count].bus = iso->bus;
                    out_info->isos[out_info->iso_count].source_irq = iso->source_irq;
                    out_info->isos[out_info->iso_count].gsi = iso->gsi;
                    out_info->isos[out_info->iso_count].flags = iso->flags;
                    if (iso->source_irq == 0) {
                        out_info->has_irq0_override = true;
                        out_info->irq0_gsi = iso->gsi;
                    }
                    out_info->iso_count++;
                }
                break;
            }
            case MADT_TYPE_NMI: {
                /* Type 4: processor ID, unaligned flags, LINT number. */
                uint16_t flags;
                memcpy(&flags, ptr + 3, sizeof(flags));
                if (ptr[5] > 1 || (flags & ~15U) ||
                    (flags & 3) == 2 || ((flags >> 2) & 3) == 2 ||
                    out_info->nmi_count == MAX_DETECTED_CPUS * 2) return false;
                size_t n = out_info->nmi_count++;
                out_info->nmis[n].processor_id = ptr[2];
                out_info->nmis[n].flags = flags;
                out_info->nmis[n].lint = ptr[5];
                break;
            }
            case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
                if (entry->length >= sizeof(acpi_madt_lapic_override_entry_t)) {
                    acpi_madt_lapic_override_entry_t *ovr = (acpi_madt_lapic_override_entry_t *)entry;
                    out_info->lapic_phys_addr = ovr->lapic_address;
                }
                break;
            }
            default:
                break;
        }

        ptr += entry->length;
    }

    serial_puts("[ OK ] MADT parsed: LAPIC Base: ");
    serial_print_hex(out_info->lapic_phys_addr);
    serial_puts(", Enabled CPUs: ");
    serial_print_dec(out_info->enabled_cpu_count);
    serial_puts(", Online-capable CPUs: ");
    serial_print_dec(out_info->online_capable_cpu_count);
    serial_puts(", I/O APICs: ");
    serial_print_dec(out_info->ioapic_count);
    serial_puts(", ISOs: ");
    serial_print_dec(out_info->iso_count);
    if (out_info->has_irq0_override) {
        serial_puts(" (IRQ0 -> GSI ");
        serial_print_dec(out_info->irq0_gsi);
        serial_puts(")");
    }
    serial_puts("\n");

    return true;
}
