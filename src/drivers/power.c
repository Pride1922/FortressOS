#include "power.h"
#include "acpi.h"
#include "serial.h"
#include "pci.h"
#include "string.h"

static const acpi_fadt_t *g_fadt = NULL;
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;
static bool g_s5_available = false;

static uint16_t parse_aml_val(const uint8_t **pptr, const uint8_t *end) {
    if (*pptr >= end) return 0;
    uint8_t b = **pptr;
    (*pptr)++;
    if (b == 0x0A && *pptr < end) { /* BytePrefix */
        uint16_t val = **pptr;
        (*pptr)++;
        return val;
    }
    if (b == 0x0B && *pptr + 1 < end) { /* WordPrefix */
        uint16_t val = *(const uint16_t *)*pptr;
        *pptr += 2;
        return val;
    }
    if (b == 0x00) return 0;
    if (b == 0x01) return 1;
    if (b == 0xFF) return 0xFF;
    return (uint16_t)b;
}

void power_init(void) {
    acpi_sdt_header_t *facp_hdr = acpi_find_table("FACP");
    if (!facp_hdr || facp_hdr->length < sizeof(acpi_sdt_header_t)) {
        serial_puts("[POWER] ACPI FADT table not found; standard fallback reset/shutdown active\n");
        return;
    }

    g_fadt = (const acpi_fadt_t *)facp_hdr;
    uintptr_t hhdm = acpi_get_hhdm_offset();

    /* Find physical address of DSDT */
    uintptr_t dsdt_phys = 0;
    if (g_fadt->header.length >= 148 && g_fadt->x_dsdt != 0) {
        dsdt_phys = (uintptr_t)g_fadt->x_dsdt;
    } else if (g_fadt->header.length >= 44 && g_fadt->dsdt != 0) {
        dsdt_phys = (uintptr_t)g_fadt->dsdt;
    }

    if (dsdt_phys == 0) {
        serial_puts("[POWER] DSDT address not specified in FADT\n");
        return;
    }

    /* Map DSDT header */
    if (!acpi_ensure_mapped(dsdt_phys, sizeof(acpi_sdt_header_t))) {
        serial_puts("[POWER] Failed to map DSDT header\n");
        return;
    }

    const acpi_sdt_header_t *dsdt_hdr = (const acpi_sdt_header_t *)(dsdt_phys + hhdm);
    if (dsdt_hdr->length < sizeof(acpi_sdt_header_t) || dsdt_hdr->length > MAX_ACPI_TABLE_SIZE) {
        return;
    }

    if (!acpi_ensure_mapped(dsdt_phys, dsdt_hdr->length)) {
        return;
    }

    if (memcmp(dsdt_hdr->signature, "DSDT", 4) != 0 || !acpi_validate_checksum(dsdt_hdr)) {
        serial_puts("[POWER] DSDT signature/checksum validation failed\n");
        return;
    }

    /* Parse DSDT byte stream for _S5_ object */
    const uint8_t *data = (const uint8_t *)dsdt_hdr;
    size_t len = dsdt_hdr->length;

    for (size_t i = 0; i + 7 < len; i++) {
        if (memcmp(data + i, "_S5_", 4) == 0) {
            const uint8_t *ptr = data + i + 4;
            const uint8_t *end = data + len;

            if (*ptr == 0x12 && ptr + 1 < end) { /* PackageOp */
                ptr++;
                uint8_t lead = *ptr;
                size_t pkg_bytes = 1;
                if ((lead & 0xC0) != 0) {
                    pkg_bytes = ((lead >> 6) & 0x03) + 1;
                }
                if (ptr + pkg_bytes >= end) break;
                ptr += pkg_bytes;

                if (ptr < end) {
                    uint8_t num_elements = *ptr++;
                    if (num_elements >= 1) g_slp_typa = parse_aml_val(&ptr, end);
                    if (num_elements >= 2) g_slp_typb = parse_aml_val(&ptr, end);
                    g_s5_available = true;
                    serial_puts("[POWER] ACPI S5 sleep state available (SLP_TYPa=");
                    serial_print_hex(g_slp_typa);
                    serial_puts(")\n");
                    break;
                }
            }
        }
    }
}

void power_reboot(void) {
    __asm__ volatile("cli");
    serial_puts("[POWER] Reboot initiated...\n");

    uintptr_t hhdm = acpi_get_hhdm_offset();

    /* 1. ACPI reset via FADT if supported (ACPI 2.0+) */
    if (g_fadt && (g_fadt->flags & (1 << 10)) && g_fadt->header.length >= 129) {
        if (g_fadt->reset_reg.address_space == 1 && g_fadt->reset_reg.address != 0) { /* System I/O */
            outb((uint16_t)g_fadt->reset_reg.address, g_fadt->reset_value);
            for (volatile int i = 0; i < 50000; i++) io_wait();
        } else if (g_fadt->reset_reg.address_space == 0 && g_fadt->reset_reg.address != 0) { /* MMIO */
            if (acpi_ensure_mapped((uintptr_t)g_fadt->reset_reg.address, 1)) {
                volatile uint8_t *reg = (volatile uint8_t *)(g_fadt->reset_reg.address + hhdm);
                *reg = g_fadt->reset_value;
                for (volatile int i = 0; i < 50000; i++) io_wait();
            }
        }
    }

    /* 2. 8042 Keyboard Controller pulse reset line (standard on PC/Dell EC) */
    for (int i = 0; i < 10000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
        io_wait();
    }
    outb(0x64, 0xFE); /* Pulse CPU reset line */
    for (volatile int i = 0; i < 100000; i++) io_wait();

    /* 3. Port 0xCF9 (PCI / Chipset Reset Control Register - standard Intel PCH) */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06); /* Reset CPU and system */
    for (volatile int i = 0; i < 100000; i++) io_wait();

    /* 4. Triple fault via zero-limit IDT */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));

    /* 5. Fallback infinite halt */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void power_shutdown(void) {
    __asm__ volatile("cli");
    serial_puts("[POWER] Shutdown initiated...\n");

    /* 1. Enable ACPI mode if SCI_EN is not active */
    if (g_fadt && g_fadt->smi_cmd && g_fadt->acpi_enable && g_fadt->pm1a_cnt_blk) {
        uint16_t pm1a_port = (uint16_t)g_fadt->pm1a_cnt_blk;
        if ((inw(pm1a_port) & 1) == 0) { /* SCI_EN is bit 0 */
            outb((uint16_t)g_fadt->smi_cmd, g_fadt->acpi_enable);
            for (int i = 0; i < 10000; i++) {
                if (inw(pm1a_port) & 1) break;
                io_wait();
            }
        }
    }

    /* 2. ACPI S5 Sleep State write to PM1a / PM1b control blocks */
    if (g_s5_available && g_fadt) {
        uint16_t slp_en = (1 << 13);
        if (g_fadt->pm1a_cnt_blk) {
            uint16_t val = (uint16_t)((g_slp_typa << 10) | slp_en);
            outw((uint16_t)g_fadt->pm1a_cnt_blk, val);
        }
        if (g_fadt->pm1b_cnt_blk) {
            uint16_t val = (uint16_t)((g_slp_typb << 10) | slp_en);
            outw((uint16_t)g_fadt->pm1b_cnt_blk, val);
        }
        for (volatile int i = 0; i < 50000; i++) io_wait();
    }

    /* 3. Hypervisor / Emulator standard ACPI shutdown I/O ports */
    outw(0x604, 0x2000);  /* QEMU default ACPI shutdown */
    outw(0xB004, 0x2000); /* Bochs / older QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */

    for (volatile int i = 0; i < 50000; i++) io_wait();

    /* 4. Fallback infinite halt */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
