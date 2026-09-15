#include "acpi.h"
#include "string.h"
#include "serial.h"

static uintptr_t g_hhdm_offset = 0;
static acpi_rsdp_t *g_rsdp = NULL;
static acpi_sdt_header_t *g_root_sdt = NULL;
static bool g_is_xsdt = false;

bool acpi_validate_checksum(const acpi_sdt_header_t *header) {
    if (!header || header->length < sizeof(acpi_sdt_header_t)) {
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

bool acpi_init(void *rsdp_ptr, uintptr_t hhdm_offset) {
    g_hhdm_offset = hhdm_offset;
    if (!rsdp_ptr) {
        serial_puts("[FAIL] ACPI init failed: RSDP pointer is NULL\n");
        return false;
    }

    if ((uintptr_t)rsdp_ptr < hhdm_offset) {
        g_rsdp = (acpi_rsdp_t *)((uintptr_t)rsdp_ptr + hhdm_offset);
    } else {
        g_rsdp = (acpi_rsdp_t *)rsdp_ptr;
    }

    /* Verify RSDP Signature */
    if (memcmp(g_rsdp->signature, "RSD PTR ", 8) != 0) {
        serial_puts("[FAIL] ACPI RSDP invalid signature\n");
        return false;
    }

    /* Verify RSDP Checksum */
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

    /* Prefer XSDT (ACPI 2.0+) over RSDT (ACPI 1.0) */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        g_root_sdt = (acpi_sdt_header_t *)((uintptr_t)g_rsdp->xsdt_address + g_hhdm_offset);
        if (memcmp(g_root_sdt->signature, "XSDT", 4) == 0 && acpi_validate_checksum(g_root_sdt)) {
            g_is_xsdt = true;
            serial_puts("[ OK ] ACPI 2.0+ XSDT discovered and verified\n");
            return true;
        }
        serial_puts("[WARN] XSDT invalid, falling back to RSDT\n");
    }

    /* Fallback to 32-bit RSDT */
    if (g_rsdp->rsdt_address != 0) {
        g_root_sdt = (acpi_sdt_header_t *)((uintptr_t)g_rsdp->rsdt_address + g_hhdm_offset);
        if (memcmp(g_root_sdt->signature, "RSDT", 4) == 0 && acpi_validate_checksum(g_root_sdt)) {
            g_is_xsdt = false;
            serial_puts("[ OK ] ACPI 1.0 RSDT discovered and verified\n");
            return true;
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
        uint64_t *table_ptrs = (uint64_t *)((uint8_t *)g_root_sdt + sizeof(acpi_sdt_header_t));
        for (size_t i = 0; i < entries; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t *)((uintptr_t)table_ptrs[i] + g_hhdm_offset);
            if (memcmp(header->signature, signature, 4) == 0) {
                return header;
            }
        }
    } else {
        size_t entries = (g_root_sdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)g_root_sdt + sizeof(acpi_sdt_header_t));
        for (size_t i = 0; i < entries; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t *)((uintptr_t)table_ptrs[i] + g_hhdm_offset);
            if (memcmp(header->signature, signature, 4) == 0) {
                return header;
            }
        }
    }

    return NULL;
}

bool acpi_parse_madt(acpi_madt_info_t *out_info) {
    if (!out_info) {
        return false;
    }
    memset(out_info, 0, sizeof(acpi_madt_info_t));

    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");
    if (!madt) {
        serial_puts("[FAIL] MADT table ('APIC') not found\n");
        return false;
    }

    if (!acpi_validate_checksum(&madt->header)) {
        serial_puts("[FAIL] MADT table checksum validation failed\n");
        return false;
    }

    out_info->lapic_phys_addr = madt->lapic_address;
    out_info->pcat_compat     = (madt->flags & 1) != 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(acpi_madt_t);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr + sizeof(acpi_madt_entry_t) <= end) {
        acpi_madt_entry_t *entry = (acpi_madt_entry_t *)ptr;
        if (entry->length < sizeof(acpi_madt_entry_t) || ptr + entry->length > end) {
            break; /* Malformed record */
        }

        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                if (entry->length >= sizeof(acpi_madt_lapic_entry_t)) {
                    acpi_madt_lapic_entry_t *lapic = (acpi_madt_lapic_entry_t *)entry;
                    if ((lapic->flags & 1) && out_info->cpu_count < MAX_DETECTED_CPUS) {
                        out_info->cpu_apic_ids[out_info->cpu_count++] = lapic->apic_id;
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
                    out_info->iso_count++;
                }
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
    serial_puts(", CPUs detected: ");
    serial_print_dec(out_info->cpu_count);
    serial_puts(", I/O APICs: ");
    serial_print_dec(out_info->ioapic_count);
    serial_puts(", ISOs: ");
    serial_print_dec(out_info->iso_count);
    serial_puts("\n");

    return true;
}
