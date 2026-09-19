#include "pci.h"
#include "acpi.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"

#define PAGE_SIZE 4096

static uintptr_t g_hhdm_offset = 0;

/* Stored MCFG Segments */
typedef struct {
    uint64_t base_address;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
} pci_mcfg_record_t;

static pci_mcfg_record_t g_mcfg_records[MAX_PCI_SEGMENTS];
static size_t g_mcfg_record_count = 0;
static bool g_mcfg_available = false;

/* Ensure MMIO page is mapped uncached and execute-disabled in VMM */
static bool pci_ensure_mapped(uintptr_t phys_addr, size_t length) {
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
            if (vmm_get_physical_address(pml4, virt) != page) {
                return false;
            }
        } else {
            /* Map uncached (PCD|PWT) and execute-disabled (NX) for device MMIO */
            int res = vmm_map_page(pml4, virt, page, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
            if (res != VMM_OK && res != VMM_ERR_ALREADY_MAPPED) {
                serial_puts("[FAIL] pci_ensure_mapped: failed to map physical page 0x");
                serial_print_hex(page);
                serial_puts("\n");
                return false;
            }
        }
    }
    return true;
}

/* Legacy I/O Port Configuration Mechanism (Ports 0xCF8 / 0xCFC) */
static uint32_t pci_legacy_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset) {
    uint32_t address = 0x80000000U |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)(dev & 0x1F) << 11) |
                       ((uint32_t)(fn & 0x07) << 8) |
                       ((uint32_t)(offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return inl(PCI_CONFIG_DATA_PORT);
}

static void pci_legacy_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint32_t val) {
    uint32_t address = 0x80000000U |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)(dev & 0x1F) << 11) |
                       ((uint32_t)(fn & 0x07) << 8) |
                       ((uint32_t)(offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    outl(PCI_CONFIG_DATA_PORT, val);
}

/* Find matching MCFG record for a given segment and bus */
static const pci_mcfg_record_t *pci_find_mcfg_record(uint16_t seg, uint8_t bus) {
    if (!g_mcfg_available) {
        return NULL;
    }
    for (size_t i = 0; i < g_mcfg_record_count; i++) {
        if (g_mcfg_records[i].segment == seg &&
            bus >= g_mcfg_records[i].start_bus &&
            bus <= g_mcfg_records[i].end_bus) {
            return &g_mcfg_records[i];
        }
    }
    return NULL;
}

/* PCIe ECAM Configuration Mechanism */
static uint32_t pci_ecam_read32(const pci_mcfg_record_t *rec, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset) {
    uint64_t bus_offset = (uint64_t)(bus - rec->start_bus);
    uintptr_t phys = (uintptr_t)(rec->base_address +
                     ((bus_offset << 20) |
                      ((uint64_t)(dev & 0x1F) << 15) |
                      ((uint64_t)(fn & 0x07) << 12) |
                      ((uint64_t)(offset & 0xFFC))));

    if (!pci_ensure_mapped(phys, 4)) {
        return 0xFFFFFFFF;
    }

    volatile uint32_t *reg = (volatile uint32_t *)(phys + g_hhdm_offset);
    return *reg;
}

static void pci_ecam_write32(const pci_mcfg_record_t *rec, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint32_t val) {
    uint64_t bus_offset = (uint64_t)(bus - rec->start_bus);
    uintptr_t phys = (uintptr_t)(rec->base_address +
                     ((bus_offset << 20) |
                      ((uint64_t)(dev & 0x1F) << 15) |
                      ((uint64_t)(fn & 0x07) << 12) |
                      ((uint64_t)(offset & 0xFFC))));

    if (!pci_ensure_mapped(phys, 4)) {
        return;
    }

    volatile uint32_t *reg = (volatile uint32_t *)(phys + g_hhdm_offset);
    *reg = val;
}

/* Public Read / Write Configuration Access */
uint32_t pci_read_config32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset) {
    const pci_mcfg_record_t *rec = pci_find_mcfg_record(seg, bus);
    if (rec != NULL) {
        return pci_ecam_read32(rec, bus, dev, fn, offset);
    }
    /* Fallback to legacy configuration for Segment 0 registers < 256 */
    if (seg == 0 && offset < 256) {
        return pci_legacy_read32(bus, dev, fn, offset);
    }
    return 0xFFFFFFFF;
}

uint16_t pci_read_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset) {
    uint32_t dword = pci_read_config32(seg, bus, dev, fn, (uint16_t)(offset & ~0x03));
    return (uint16_t)((dword >> ((offset & 0x02) * 8)) & 0xFFFF);
}

uint8_t pci_read_config8(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset) {
    uint32_t dword = pci_read_config32(seg, bus, dev, fn, (uint16_t)(offset & ~0x03));
    return (uint8_t)((dword >> ((offset & 0x03) * 8)) & 0xFF);
}

void pci_write_config32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint32_t val) {
    const pci_mcfg_record_t *rec = pci_find_mcfg_record(seg, bus);
    if (rec != NULL) {
        pci_ecam_write32(rec, bus, dev, fn, offset, val);
        return;
    }
    if (seg == 0 && offset < 256) {
        pci_legacy_write32(bus, dev, fn, offset, val);
    }
}

void pci_write_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint16_t val) {
    /* Do not RMW a dword: adjacent PCI status bits are write-one-to-clear. */
    if ((offset & 1) || offset > 4094) return;
    const pci_mcfg_record_t *rec = pci_find_mcfg_record(seg, bus);
    if (rec) {
        uintptr_t phys = rec->base_address + ((uintptr_t)(bus - rec->start_bus) << 20)
                       + ((uintptr_t)(dev & 31) << 15) + ((uintptr_t)(fn & 7) << 12) + offset;
        if (!pci_ensure_mapped(phys, 2)) return;
        *(volatile uint16_t *)(phys + g_hhdm_offset) = val;
    } else if (seg == 0 && offset < 256) {
        uint32_t address = 0x80000000U | ((uint32_t)bus << 16)
                         | ((uint32_t)(dev & 31) << 11) | ((uint32_t)(fn & 7) << 8)
                         | (offset & 0xfc);
        outl(PCI_CONFIG_ADDRESS_PORT, address);
        outw((uint16_t)(PCI_CONFIG_DATA_PORT + (offset & 2)), val);
    }
}

/* Parse ACPI MCFG Table */
static void pci_parse_mcfg(void) {
    acpi_sdt_header_t *mcfg_hdr = acpi_find_table("MCFG");
    if (!mcfg_hdr) {
        serial_puts("[PCI] ACPI MCFG table not found. Using Legacy PCI configuration (0xCF8/0xCFC).\n");
        g_mcfg_available = false;
        return;
    }

    if (mcfg_hdr->length < sizeof(acpi_sdt_header_t) + 8) {
        serial_puts("[PCI] ACPI MCFG table too short. Falling back to Legacy PCI configuration.\n");
        g_mcfg_available = false;
        return;
    }

    size_t payload_len = mcfg_hdr->length - (sizeof(acpi_sdt_header_t) + 8);
    size_t entry_count = payload_len / sizeof(acpi_mcfg_allocation_t);

    if (entry_count == 0) {
        serial_puts("[PCI] ACPI MCFG contains 0 allocation entries.\n");
        g_mcfg_available = false;
        return;
    }

    const uint8_t *entry_bytes = (const uint8_t *)mcfg_hdr + sizeof(acpi_sdt_header_t) + 8;
    g_mcfg_record_count = 0;

    for (size_t i = 0; i < entry_count && g_mcfg_record_count < MAX_PCI_SEGMENTS; i++) {
        const acpi_mcfg_allocation_t *alloc = (const acpi_mcfg_allocation_t *)(entry_bytes + i * sizeof(acpi_mcfg_allocation_t));
        g_mcfg_records[g_mcfg_record_count].base_address = alloc->base_address;
        g_mcfg_records[g_mcfg_record_count].segment      = alloc->pci_segment_group;
        g_mcfg_records[g_mcfg_record_count].start_bus    = alloc->start_bus_number;
        g_mcfg_records[g_mcfg_record_count].end_bus      = alloc->end_bus_number;

        serial_puts("[PCI] Discovered ECAM Segment ");
        serial_print_dec(alloc->pci_segment_group);
        serial_puts(" (Buses ");
        serial_print_dec(alloc->start_bus_number);
        serial_puts("..");
        serial_print_dec(alloc->end_bus_number);
        serial_puts(") at Base 0x");
        serial_print_hex(alloc->base_address);
        serial_puts("\n");

        g_mcfg_record_count++;
    }

    if (g_mcfg_record_count > 0) {
        g_mcfg_available = true;
        serial_puts("[ OK ] PCIe ECAM configuration mechanism active (");
        serial_print_dec(g_mcfg_record_count);
        serial_puts(" segment allocation(s))\n");
    }
}

/* Initialize PCI Subsystem */
void pci_init(uintptr_t hhdm_offset) {
    g_hhdm_offset = hhdm_offset;
    g_mcfg_record_count = 0;
    g_mcfg_available = false;

    serial_puts("[PCI] Initializing PCI / PCIe bus subsystem...\n");
    pci_parse_mcfg();
}

bool pci_is_mcfg_available(void) {
    return g_mcfg_available;
}

size_t pci_get_segment_count(void) {
    return g_mcfg_record_count;
}

/* Phase 9G.1a intentionally stops at firmware-assigned PCI metadata. */
void pci_report_xhci(void) {
    /* Keep this reporting limit in sync with XHCI_MAX_CONTROLLERS in xhci.h. */
    enum { max_controllers = 4 };
    pci_device_t controllers[max_controllers];
    serial_puts("[USB 9G.1a] PCI discovery only\n");
    size_t count = pci_find_all_devices(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                        PCI_PROGIF_USB_XHCI, controllers, max_controllers);
    if (count == 0) {
        serial_puts("[USB 9G.1a] No xHCI controller found; continuing without USB storage\n");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const pci_device_t *d = &controllers[i];
        serial_puts("[USB 9G.1a] xHCI controller ");
        serial_print_dec(i + 1);
        serial_puts("/");
        serial_print_dec(count);
        serial_puts(": BDF=");
        pci_print_bdf(d->segment, d->bus, d->device, d->function);
        serial_puts(" vendor=");
        serial_print_hex(d->vendor_id);
        serial_puts(" device=");
        serial_print_hex(d->device_id);
        serial_puts("\n");
    }

    pci_device_t dev = controllers[0];
    if ((dev.header_type & 0x7f) != PCI_HEADER_TYPE_NORMAL) {
        serial_puts("[USB 9G.1a] Unsupported PCI header; BAR inspection skipped\n");
        return;
    }
    uint32_t raw = pci_read_config32(dev.segment, dev.bus, dev.device,
                                     dev.function, PCI_REG_BAR0);
    uint32_t type = raw & PCI_BAR_MEM_TYPE_MASK;
    if (raw == 0xffffffff || dev.bar[0] == 0 || (raw & PCI_BAR_IO_SPACE) ||
        (type != PCI_BAR_MEM_TYPE_32 && type != PCI_BAR_MEM_TYPE_64)) {
        serial_puts("[USB 9G.1a] BAR0 unavailable or unsupported; raw=");
        serial_print_hex(raw);
        serial_puts("; controller left untouched\n");
        return;
    }

    serial_puts("[USB 9G.1a] BAR0=");
    serial_print_hex(dev.bar[0]);
    serial_puts(dev.bar_is_64[0] ? " memory64" : " memory32");
    serial_puts(dev.bar_prefetch[0] ? " prefetch=yes" : " prefetch=no");
    serial_puts("\n[USB 9G.1a] Discovery complete; BAR extent/MMIO unverified; USB storage not initialized\n");
}

/* Non-destructive inspection of a device's Base Address Registers (BARs) */
static void pci_inspect_bars(pci_device_t *dev) {
    for (int b = 0; b < 6; ) {
        uint32_t bar_val = pci_read_config32(dev->segment, dev->bus, dev->device, dev->function,
                                            (uint16_t)(PCI_REG_BAR0 + b * 4));

        if (bar_val == 0 || bar_val == 0xFFFFFFFF) {
            dev->bar[b] = 0;
            dev->bar_is_io[b] = false;
            dev->bar_is_64[b] = false;
            dev->bar_prefetch[b] = false;
            b++;
            continue;
        }

        if (bar_val & PCI_BAR_IO_SPACE) {
            /* I/O Space BAR */
            dev->bar[b] = (uint64_t)(bar_val & PCI_BAR_IO_ADDR_MASK);
            dev->bar_is_io[b] = true;
            dev->bar_is_64[b] = false;
            dev->bar_prefetch[b] = false;
            b++;
        } else {
            /* Memory Space BAR */
            dev->bar_is_io[b] = false;
            dev->bar_prefetch[b] = (bar_val & PCI_BAR_MEM_PREFETCH) != 0;

            uint32_t mem_type = bar_val & PCI_BAR_MEM_TYPE_MASK;
            if (mem_type == PCI_BAR_MEM_TYPE_64 && b + 1 < 6) {
                /* 64-bit Memory BAR */
                uint32_t bar_hi = pci_read_config32(dev->segment, dev->bus, dev->device, dev->function,
                                                   (uint16_t)(PCI_REG_BAR0 + (b + 1) * 4));
                dev->bar[b] = ((uint64_t)bar_hi << 32) | (uint64_t)(bar_val & PCI_BAR_MEM_ADDR_MASK);
                dev->bar_is_64[b] = true;

                /* The high 32 bits occupy the next slot */
                dev->bar[b + 1] = 0;
                dev->bar_is_io[b + 1] = false;
                dev->bar_is_64[b + 1] = false;
                dev->bar_prefetch[b + 1] = false;

                b += 2;
            } else {
                /* 32-bit Memory BAR */
                dev->bar[b] = (uint64_t)(bar_val & PCI_BAR_MEM_ADDR_MASK);
                dev->bar_is_64[b] = false;
                b++;
            }
        }
    }
}

/* Enumerate a single (segment, bus, device, function) */
static bool pci_probe_function(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, pci_device_t *out_dev) {
    uint16_t vendor_id = pci_read_config16(seg, bus, dev, fn, PCI_REG_VENDOR_ID);
    if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
        return false;
    }

    memset(out_dev, 0, sizeof(pci_device_t));
    out_dev->segment     = seg;
    out_dev->bus         = bus;
    out_dev->device      = dev;
    out_dev->function    = fn;
    out_dev->vendor_id   = vendor_id;
    out_dev->device_id   = pci_read_config16(seg, bus, dev, fn, PCI_REG_DEVICE_ID);
    out_dev->revision_id = pci_read_config8(seg, bus, dev, fn, PCI_REG_REVISION_ID);
    out_dev->prog_if     = pci_read_config8(seg, bus, dev, fn, PCI_REG_PROG_IF);
    out_dev->subclass    = pci_read_config8(seg, bus, dev, fn, PCI_REG_SUBCLASS);
    out_dev->class_code  = pci_read_config8(seg, bus, dev, fn, PCI_REG_CLASS);
    out_dev->header_type = pci_read_config8(seg, bus, dev, fn, PCI_REG_HEADER_TYPE);

    /* If standard PCI device (Header Type 0), inspect firmware-assigned BARs */
    if ((out_dev->header_type & 0x7F) == PCI_HEADER_TYPE_NORMAL) {
        pci_inspect_bars(out_dev);
    }

    return true;
}

/* Scan all buses and collect discovered devices */
size_t pci_scan_all(pci_device_t *out_devices, size_t max_devices) {
    if (!out_devices || max_devices == 0) {
        return 0;
    }

    size_t count = 0;

    /* Determine segments and bus ranges */
    size_t seg_iterations = g_mcfg_available ? g_mcfg_record_count : 1;

    for (size_t s = 0; s < seg_iterations && count < max_devices; s++) {
        uint16_t seg       = g_mcfg_available ? g_mcfg_records[s].segment : 0;
        uint16_t start_bus = g_mcfg_available ? g_mcfg_records[s].start_bus : 0;
        uint16_t end_bus   = g_mcfg_available ? g_mcfg_records[s].end_bus : 255;

        for (uint16_t bus = start_bus; bus <= end_bus && count < max_devices; bus++) {
            for (uint8_t dev = 0; dev < 32 && count < max_devices; dev++) {
                /* Check function 0 to determine device existence and multi-function flag */
                pci_device_t probe_fn0;
                if (!pci_probe_function(seg, (uint8_t)bus, dev, 0, &probe_fn0)) {
                    continue;
                }

                out_devices[count++] = probe_fn0;

                /* If multi-function device, probe functions 1 through 7 */
                bool is_multifn = (probe_fn0.header_type & PCI_HEADER_TYPE_MULTIFN) != 0;
                if (is_multifn) {
                    for (uint8_t fn = 1; fn < 8 && count < max_devices; fn++) {
                        pci_device_t probe_sub;
                        if (pci_probe_function(seg, (uint8_t)bus, dev, fn, &probe_sub)) {
                            out_devices[count++] = probe_sub;
                        }
                    }
                }
            }
        }
    }

    return count;
}

/* Locate the first device matching class, subclass, and prog_if */
bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out_device) {
    if (!out_device) {
        return false;
    }

    size_t seg_iterations = g_mcfg_available ? g_mcfg_record_count : 1;

    for (size_t s = 0; s < seg_iterations; s++) {
        uint16_t seg       = g_mcfg_available ? g_mcfg_records[s].segment : 0;
        uint16_t start_bus = g_mcfg_available ? g_mcfg_records[s].start_bus : 0;
        uint16_t end_bus   = g_mcfg_available ? g_mcfg_records[s].end_bus : 255;

        for (uint16_t bus = start_bus; bus <= end_bus; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                pci_device_t probe_fn0;
                if (!pci_probe_function(seg, (uint8_t)bus, dev, 0, &probe_fn0)) {
                    continue;
                }

                if (probe_fn0.class_code == class_code &&
                    probe_fn0.subclass == subclass &&
                    probe_fn0.prog_if == prog_if) {
                    *out_device = probe_fn0;
                    return true;
                }

                bool is_multifn = (probe_fn0.header_type & PCI_HEADER_TYPE_MULTIFN) != 0;
                if (is_multifn) {
                    for (uint8_t fn = 1; fn < 8; fn++) {
                        pci_device_t probe_sub;
                        if (pci_probe_function(seg, (uint8_t)bus, dev, fn, &probe_sub)) {
                            if (probe_sub.class_code == class_code &&
                                probe_sub.subclass == subclass &&
                                probe_sub.prog_if == prog_if) {
                                *out_device = probe_sub;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

/* Collect matching devices in the same order as pci_find_device. */
size_t pci_find_all_devices(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                            pci_device_t *out_array, size_t max_count) {
    if (!out_array || max_count == 0) {
        return 0;
    }

    size_t found = 0;
    size_t seg_iterations = g_mcfg_available ? g_mcfg_record_count : 1;

    for (size_t s = 0; s < seg_iterations; s++) {
        uint16_t seg       = g_mcfg_available ? g_mcfg_records[s].segment : 0;
        uint16_t start_bus = g_mcfg_available ? g_mcfg_records[s].start_bus : 0;
        uint16_t end_bus   = g_mcfg_available ? g_mcfg_records[s].end_bus : 255;

        for (uint16_t bus = start_bus; bus <= end_bus; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                pci_device_t probe_fn0;
                if (!pci_probe_function(seg, (uint8_t)bus, dev, 0, &probe_fn0)) {
                    continue;
                }

                if (probe_fn0.class_code == class_code &&
                    probe_fn0.subclass == subclass &&
                    probe_fn0.prog_if == prog_if) {
                    out_array[found++] = probe_fn0;
                    if (found == max_count) return found;
                }

                bool is_multifn = (probe_fn0.header_type & PCI_HEADER_TYPE_MULTIFN) != 0;
                if (is_multifn) {
                    for (uint8_t fn = 1; fn < 8; fn++) {
                        pci_device_t probe_sub;
                        if (pci_probe_function(seg, (uint8_t)bus, dev, fn, &probe_sub)) {
                            if (probe_sub.class_code == class_code &&
                                probe_sub.subclass == subclass &&
                                probe_sub.prog_if == prog_if) {
                                out_array[found++] = probe_sub;
                                if (found == max_count) return found;
                            }
                        }
                    }
                }
            }
        }
    }

    return found;
}

/* Helper to get class description string */
static const char *pci_class_to_str(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    switch (class_code) {
        case PCI_CLASS_STORAGE:
            switch (subclass) {
                case PCI_SUBCLASS_STORAGE_SCSI:  return "Storage (SCSI)";
                case PCI_SUBCLASS_STORAGE_IDE:   return "Storage (IDE)";
                case PCI_SUBCLASS_STORAGE_SATA:  return "Storage (SATA/AHCI)";
                case PCI_SUBCLASS_STORAGE_NVME:
                    if (prog_if == PCI_PROGIF_STORAGE_NVME) return "Storage (NVMe Controller)";
                    return "Storage (Non-Volatile Memory)";
                default:                         return "Storage Controller";
            }
        case PCI_CLASS_NETWORK:
            return "Network Controller";
        case PCI_CLASS_DISPLAY:
            return "Display Controller (VGA/GOP)";
        case PCI_CLASS_MULTIMEDIA:
            return "Multimedia Device (Audio)";
        case PCI_CLASS_BRIDGE:
            return "Bridge Device (Host/ISA/PCI)";
        case PCI_CLASS_COMMUNICATION:
            return "Communication Controller";
        case PCI_CLASS_SYSTEM_PERIPH:
            return "System Peripheral";
        case PCI_CLASS_INPUT_DEVICE:
            return "Input Controller";
        case PCI_CLASS_SERIAL_BUS:
            return "Serial Bus (USB/SMBus)";
        default:
            return "Other Device";
    }
}

/* Fixed-width hexadecimal printing helper without '0x' prefix */
static void pci_print_hex_digits(uint64_t val, int digits) {
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        serial_putc(hex_chars[(val >> i) & 0xF]);
    }
}

/* Print formatted BDF string (e.g. 0000:00:03.0) */
void pci_print_bdf(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn) {
    pci_print_hex_digits(seg, 4);
    serial_putc(':');
    pci_print_hex_digits(bus, 2);
    serial_putc(':');
    pci_print_hex_digits(dev, 2);
    serial_putc('.');
    serial_print_dec(fn);
}

/* Print tabular inventory of discovered PCI devices */
void pci_print_inventory(const pci_device_t *devices, size_t count) {
    serial_puts("===================================================================================================\n");
    serial_puts("  SEG:BUS:DEV.FN  | VENDOR:DEVICE | CLASS:SUB:IF | DESCRIPTION             | PRIMARY BAR\n");
    serial_puts("===================================================================================================\n");

    for (size_t i = 0; i < count; i++) {
        const pci_device_t *d = &devices[i];

        /* Format SEG:BUS:DEV.FN (e.g. 0000:00:03.0) */
        serial_puts("  ");
        pci_print_bdf(d->segment, d->bus, d->device, d->function);
        serial_puts("  |   ");

        /* Format VENDOR:DEVICE (e.g. 1B36:0010) */
        pci_print_hex_digits(d->vendor_id, 4);
        serial_puts(":");
        pci_print_hex_digits(d->device_id, 4);
        serial_puts("   |   ");

        /* Format CLASS:SUB:IF (e.g. 01:08:02) */
        pci_print_hex_digits(d->class_code, 2);
        serial_puts(":");
        pci_print_hex_digits(d->subclass, 2);
        serial_puts(":");
        pci_print_hex_digits(d->prog_if, 2);
        serial_puts("   | ");

        /* Format Description */
        const char *desc = pci_class_to_str(d->class_code, d->subclass, d->prog_if);
        serial_puts(desc);
        size_t desc_len = strlen(desc);
        for (size_t pad = desc_len; pad < 23; pad++) {
            serial_putc(' ');
        }
        serial_puts(" | ");

        /* Format Primary BAR0 */
        if (d->bar[0] != 0) {
            serial_puts("0x");
            pci_print_hex_digits(d->bar[0], d->bar_is_64[0] ? 16 : 8);
            if (d->bar_is_io[0]) {
                serial_puts(" (I/O Port)");
            } else if (d->bar_is_64[0]) {
                serial_puts(" (64-bit MMIO)");
            } else {
                serial_puts(" (32-bit MMIO)");
            }
        } else {
            serial_puts("None");
        }
        serial_puts("\n");
    }
    serial_puts("===================================================================================================\n");
}
