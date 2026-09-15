#include "ioapic.h"
#include "vmm.h"
#include "serial.h"

static ioapic_controller_t g_ioapics[MAX_IOAPIC_CONTROLLERS];
static size_t g_ioapic_count = 0;

uint32_t ioapic_read(size_t controller_idx, uint8_t reg) {
    if (controller_idx >= g_ioapic_count) return 0;
    volatile uint32_t *regsel = (volatile uint32_t *)(g_ioapics[controller_idx].virt_base + IOREGSEL);
    volatile uint32_t *win    = (volatile uint32_t *)(g_ioapics[controller_idx].virt_base + IOWIN);
    *regsel = reg;
    return *win;
}

void ioapic_write(size_t controller_idx, uint8_t reg, uint32_t val) {
    if (controller_idx >= g_ioapic_count) return;
    volatile uint32_t *regsel = (volatile uint32_t *)(g_ioapics[controller_idx].virt_base + IOREGSEL);
    volatile uint32_t *win    = (volatile uint32_t *)(g_ioapics[controller_idx].virt_base + IOWIN);
    *regsel = reg;
    *win    = val;
}

bool ioapic_init(const acpi_madt_info_t *madt) {
    if (!madt || madt->ioapic_count == 0 || madt->ioapic_count > MAX_IOAPIC_CONTROLLERS) {
        serial_puts("[WARN] No I/O APIC controllers reported by MADT\n");
        return false;
    }

    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    g_ioapic_count = 0;

    for (size_t i = 0; i < madt->ioapic_count && i < MAX_IOAPIC_CONTROLLERS; i++) {
        uintptr_t phys = madt->ioapics[i].phys_addr;
        uintptr_t virt = IOAPIC_VIRT_BASE + (i * 0x1000);

        int res = vmm_map_page(pml4, virt, phys,
                               PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
        if (res != VMM_OK) {
            serial_puts("[FAIL] Failed to map I/O APIC MMIO page into VMM\n");
            return false;
        }

        g_ioapics[i].id        = madt->ioapics[i].id;
        g_ioapics[i].phys_base = phys;
        g_ioapics[i].virt_base = virt;
        g_ioapics[i].gsi_base  = madt->ioapics[i].gsi_base;

        g_ioapic_count++;

        /* Read version register to determine maximum redirection entries */
        uint32_t ver = ioapic_read(i, IOAPIC_REG_VER);
        g_ioapics[i].pin_count = ((ver >> 16) & 0xFF) + 1;

        if (ver == UINT32_MAX || g_ioapics[i].pin_count > 120 ||
            g_ioapics[i].gsi_base > UINT32_MAX - g_ioapics[i].pin_count) return false;
        for (size_t j = 0; j < i; ++j) {
            if (g_ioapics[i].gsi_base < g_ioapics[j].gsi_base + g_ioapics[j].pin_count &&
                g_ioapics[j].gsi_base < g_ioapics[i].gsi_base + g_ioapics[i].pin_count) return false;
        }
        serial_puts("[ OK ] I/O APIC ");
        serial_print_dec(i);
        serial_puts(" (ID ");
        serial_print_dec(g_ioapics[i].id);
        serial_puts(") initialized at Phys ");
        serial_print_hex(phys);
        serial_puts(", GSI Base: ");
        serial_print_dec(g_ioapics[i].gsi_base);
        serial_puts(", Pins: ");
        serial_print_dec(g_ioapics[i].pin_count);
        serial_puts("\n");
    }

    /* Mask all pins initially across all discovered controllers */
    ioapic_mask_all();

    for (size_t c = 0; c < g_ioapic_count; ++c)
        for (uint32_t pin = 0; pin < g_ioapics[c].pin_count; ++pin)
            if (!(ioapic_read(c, IOAPIC_REG_REDTBL_BASE + 2 * pin) & IOAPIC_REDTBL_MASK)) return false;
    serial_puts("[ OK ] All I/O APIC redirection table entries masked and read back\n");
    return true;
}

size_t ioapic_get_controller_count(void) {
    return g_ioapic_count;
}

uint32_t ioapic_get_pin_count(size_t controller_idx) {
    if (controller_idx >= g_ioapic_count) return 0;
    return g_ioapics[controller_idx].pin_count;
}

void ioapic_mask_all(void) {
    for (size_t c = 0; c < g_ioapic_count; c++) {
        for (uint32_t p = 0; p < g_ioapics[c].pin_count; p++) {
            uint8_t reg_low  = IOAPIC_REG_REDTBL_BASE + (p * 2);
            uint8_t reg_high = reg_low + 1;
            /* Masked bit set, other bits cleared */
            ioapic_write(c, reg_low, IOAPIC_REDTBL_MASK);
            ioapic_write(c, reg_high, 0);
        }
    }
}

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, bool level_triggered, bool active_low) {
    if (vector <= 0x20 || vector == 0xFF || dest_apic_id == 0xFF) return false;
    /* Caller holds interrupts disabled; no SMP or ISR access allowed. */
    /* Find controller matching GSI */
    for (size_t c = 0; c < g_ioapic_count; c++) {
        if (gsi >= g_ioapics[c].gsi_base && gsi < g_ioapics[c].gsi_base + g_ioapics[c].pin_count) {
            uint32_t pin = gsi - g_ioapics[c].gsi_base;
            uint8_t reg_low  = IOAPIC_REG_REDTBL_BASE + (pin * 2);
            uint8_t reg_high = reg_low + 1;

            uint32_t low_val = vector;
            if (level_triggered) low_val |= IOAPIC_TRIGGER_LEVEL;
            if (active_low)      low_val |= IOAPIC_POLARITY_LOW;

            uint32_t high_val = ((uint32_t)dest_apic_id) << 24;

            ioapic_write(c, reg_low, ioapic_read(c, reg_low) | IOAPIC_REDTBL_MASK);
            ioapic_write(c, reg_high, high_val);
            ioapic_write(c, reg_low, low_val); /* Unmasked */
            return true;
        }
    }
    return false;
}

/* ISA defaults are active-high/edge; firmware ISO flags can override them. */
bool ioapic_route_isa(const acpi_madt_info_t *madt, uint8_t irq, uint8_t vector, uint8_t dest_apic_id) {
    if (!madt || irq > 15 || madt->iso_count > MAX_DETECTED_ISOS) return false;
    uint32_t gsi = irq;
    bool active_low = false, level = false, found = false;
    for (size_t i = 0; i < madt->iso_count; ++i) {
        if (madt->isos[i].bus != 0 || madt->isos[i].source_irq != irq) continue;
        if (found) return false;
        found = true;
        uint16_t flags = madt->isos[i].flags;
        unsigned polarity = flags & 3, trigger = (flags >> 2) & 3;
        if ((flags & ~15u) || polarity == 2 || trigger == 2) return false;
        active_low = polarity == 3;
        level = trigger == 3;
        gsi = madt->isos[i].gsi;
    }
    return ioapic_route_gsi(gsi, vector, dest_apic_id, level, active_low);
}
