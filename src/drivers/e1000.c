#include "e1000.h"
#include "serial.h"
#include "vmm.h"
#include "string.h"

static e1000_device_t s_e1000_dev;
static bool s_e1000_found = false;

static inline uint32_t e1000_read32(uintptr_t base, uint32_t reg) {
    volatile uint32_t *addr = (volatile uint32_t *)(base + reg);
    return *addr;
}

static inline void e1000_write32(uintptr_t base, uint32_t reg, uint32_t val) {
    volatile uint32_t *addr = (volatile uint32_t *)(base + reg);
    *addr = val;
}

static void print_hex_digits(uint64_t val, int digits) {
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        serial_putc(hex_chars[(val >> i) & 0xF]);
    }
}

static void print_mac(const uint8_t mac[6]) {
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        if (i > 0) serial_putc(':');
        serial_putc(hex_chars[(mac[i] >> 4) & 0xF]);
        serial_putc(hex_chars[mac[i] & 0xF]);
    }
}

static bool is_valid_mac(const uint8_t mac[6]) {
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xFF) all_ff = false;
    }
    return !all_zero && !all_ff;
}

static uint16_t e1000_read_eeprom(uintptr_t base, uint8_t addr) {
    e1000_write32(base, E1000_REG_EERD, E1000_EERD_START | ((uint32_t)addr << E1000_EERD_ADDR_SHIFT));
    for (int i = 0; i < 10000; i++) {
        uint32_t val = e1000_read32(base, E1000_REG_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)((val >> E1000_EERD_DATA_SHIFT) & 0xFFFF);
        }
        io_wait();
    }
    return 0xFFFF;
}

static bool is_supported_e1000(const pci_device_t *d) {
    if (d->vendor_id != E1000_VENDOR_INTEL) {
        return false;
    }
    switch (d->device_id) {
        case E1000_DEV_82540EM:
        case E1000_DEV_82574L:
        case E1000_DEV_I219_LM:
        case E1000_DEV_I219_LM_ALT:
        case E1000_DEV_I219_LM_15D7:   /* Dell 5590 */
        case E1000_DEV_I219_LM_15BD:   /* Dell 5500 */
        case E1000_DEV_I219_LM_15BB:
            return true;
        default:
            return false;
    }
}

bool e1000_boot_probe(void) {
    pci_device_t pci_devs[MAX_PCI_DEVICES];
    size_t count = pci_scan_all(pci_devs, MAX_PCI_DEVICES);

    pci_device_t *target = NULL;
    for (size_t i = 0; i < count; i++) {
        if (is_supported_e1000(&pci_devs[i])) {
            target = &pci_devs[i];
            break;
        }
    }

    if (!target) {
        return false;
    }

    memset(&s_e1000_dev, 0, sizeof(s_e1000_dev));
    s_e1000_dev.pci = *target;

    /* 1. Validate BAR0 */
    if ((target->header_type & 0x7F) != PCI_HEADER_TYPE_NORMAL) {
        serial_puts("[NET] e1000: Unsupported header type\n");
        return false;
    }

    if (target->bar[0] == 0 || target->bar[0] == 0xFFFFFFFF || target->bar_is_io[0]) {
        serial_puts("[NET] e1000: BAR0 is invalid or I/O space\n");
        return false;
    }

    uint16_t seg = target->segment;
    uint8_t bus  = target->bus;
    uint8_t dev  = target->device;
    uint8_t fn   = target->function;

    /* 2. Sizing BAR0 with decode disabled */
    uint16_t orig_cmd = pci_read_config16(seg, bus, dev, fn, PCI_REG_COMMAND);
    uint16_t disabled = (orig_cmd & ~(PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_IO_SPACE |
                                     PCI_COMMAND_BUS_MASTER)) | PCI_COMMAND_INT_DISABLE;
    pci_write_config16(seg, bus, dev, fn, PCI_REG_COMMAND, disabled);

    pci_write_config32(seg, bus, dev, fn, PCI_REG_BAR0, 0xFFFFFFFF);
    uint32_t mask_low = pci_read_config32(seg, bus, dev, fn, PCI_REG_BAR0);
    pci_write_config32(seg, bus, dev, fn, PCI_REG_BAR0, (uint32_t)target->bar[0]);

    uint32_t aperture = E1000_DEFAULT_APERTURE;
    if (mask_low != 0xFFFFFFFF && mask_low != 0) {
        uint32_t sized = ~(mask_low & PCI_BAR_MEM_ADDR_MASK) + 1;
        if (sized >= 4096 && sized <= (16 * 1024 * 1024) && (sized & (sized - 1)) == 0) {
            aperture = sized;
        }
    }

    s_e1000_dev.bar0_phys = target->bar[0];
    s_e1000_dev.mmio_virt = E1000_MMIO_VIRT;
    s_e1000_dev.aperture_size = aperture;

    /* 3. Map BAR0 into kernel virtual space uncached (PCD | PWT | NX) */
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    for (uint32_t offset = 0; offset < aperture; offset += 4096) {
        uintptr_t va = s_e1000_dev.mmio_virt + offset;
        uintptr_t pa = s_e1000_dev.bar0_phys + offset;
        int map_res = vmm_map_page(pml4, va, pa,
                                   PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
        if (map_res != VMM_OK && map_res != VMM_ERR_ALREADY_MAPPED) {
            serial_puts("[NET] e1000: VMM MMIO map failed at offset 0x");
            print_hex_digits(offset, 8);
            serial_puts("\n");
            return false;
        }
    }

    /* 4. Enable PCI Memory Space decode (bus mastering and interrupts remain disabled in Phase 1a) */
    pci_write_config16(seg, bus, dev, fn, PCI_REG_COMMAND, disabled | PCI_COMMAND_MEMORY_SPACE);

    /* 5. Read MAC Address from RAL0/RAH0 */
    uint32_t ral = e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_RAL0);
    uint32_t rah = e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_RAH0);

    s_e1000_dev.mac_addr[0] = (uint8_t)(ral & 0xFF);
    s_e1000_dev.mac_addr[1] = (uint8_t)((ral >> 8) & 0xFF);
    s_e1000_dev.mac_addr[2] = (uint8_t)((ral >> 16) & 0xFF);
    s_e1000_dev.mac_addr[3] = (uint8_t)((ral >> 24) & 0xFF);
    s_e1000_dev.mac_addr[4] = (uint8_t)(rah & 0xFF);
    s_e1000_dev.mac_addr[5] = (uint8_t)((rah >> 8) & 0xFF);

    /* Fallback to EEPROM if RAL0/RAH0 invalid */
    if (!is_valid_mac(s_e1000_dev.mac_addr) || !(rah & E1000_RAH_AV)) {
        uint16_t w0 = e1000_read_eeprom(s_e1000_dev.mmio_virt, 0);
        uint16_t w1 = e1000_read_eeprom(s_e1000_dev.mmio_virt, 1);
        uint16_t w2 = e1000_read_eeprom(s_e1000_dev.mmio_virt, 2);
        s_e1000_dev.mac_addr[0] = (uint8_t)(w0 & 0xFF);
        s_e1000_dev.mac_addr[1] = (uint8_t)((w0 >> 8) & 0xFF);
        s_e1000_dev.mac_addr[2] = (uint8_t)(w1 & 0xFF);
        s_e1000_dev.mac_addr[3] = (uint8_t)((w1 >> 8) & 0xFF);
        s_e1000_dev.mac_addr[4] = (uint8_t)(w2 & 0xFF);
        s_e1000_dev.mac_addr[5] = (uint8_t)((w2 >> 8) & 0xFF);
    }

    if (!is_valid_mac(s_e1000_dev.mac_addr)) {
        serial_puts("[NET] e1000: Invalid MAC address read\n");
        return false;
    }

    /* 6. Read STATUS register */
    s_e1000_dev.status = e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_STATUS);
    s_e1000_dev.link_up = (s_e1000_dev.status & E1000_STATUS_LU) != 0;
    s_e1000_dev.initialized = true;
    s_e1000_found = true;

    /* 7. Diagnostic output */
    serial_puts("[NET] e1000: ");
    pci_print_bdf(seg, bus, dev, fn);
    serial_putc(' ');
    print_hex_digits(target->vendor_id, 4);
    serial_putc(':');
    print_hex_digits(target->device_id, 4);
    serial_puts(" BAR0 0x");
    print_hex_digits(s_e1000_dev.bar0_phys, target->bar_is_64[0] ? 16 : 8);
    serial_puts(target->bar_is_64[0] ? " (64-bit MMIO) -> 0x" : " (32-bit MMIO) -> 0x");
    print_hex_digits(s_e1000_dev.mmio_virt, 16);
    serial_puts("\n");

    serial_puts("[NET] e1000: MAC ");
    print_mac(s_e1000_dev.mac_addr);
    serial_puts(", STATUS 0x");
    print_hex_digits(s_e1000_dev.status, 8);
    serial_puts(s_e1000_dev.link_up ? " (link up)\n" : " (link down)\n");

    return true;
}

bool net_boot_probe(void) {
    serial_puts("[NET] Probing network controllers...\n");
    if (!e1000_boot_probe()) {
        serial_puts("[NET] No network controller found; networking unavailable\n");
        return false;
    }
    return true;
}

const e1000_device_t *e1000_get_active_device(void) {
    return s_e1000_found ? &s_e1000_dev : NULL;
}
