#include "e1000.h"
#include "serial.h"
#include "vmm.h"
#include "string.h"
#include "pmm.h"
#include "spinlock.h"
#include "console.h"
#include "netctl_abi.h"
#include "apic.h"
#include <stddef.h>

static bool e1000_init_rings(void);
static bool e1000_i219_init(void);
static bool e1000_wait_for_cable(void);
static e1000_device_t s_e1000_dev;

/* Linux v6.12 e1000e hw.h: LM4=SPT, LM6/LM7=CNP. Older discovery IDs
 * deliberately retain their discovery-only behavior. */
static bool is_i219(void) {
    uint16_t id = s_e1000_dev.pci.device_id;
    return id == E1000_DEV_I219_LM_15D7 ||
           id == E1000_DEV_I219_LM_15BD ||
           id == E1000_DEV_I219_LM_15BB ||
           id == E1000_DEV_I219_LM_1A1E;
}

static bool s_e1000_found = false;

static inline uint32_t e1000_read32(uintptr_t base, uint32_t reg) {
#ifdef E1000_PCH_HOST_TEST
    return e1000_mock_read(base, reg);
#else
    volatile uint32_t *addr = (volatile uint32_t *)(base + reg);
    return *addr;
#endif
}

static inline void e1000_write32(uintptr_t base, uint32_t reg, uint32_t val) {
#ifdef E1000_PCH_HOST_TEST
    e1000_mock_write(base, reg, val);
#else
    volatile uint32_t *addr = (volatile uint32_t *)(base + reg);
    *addr = val;
#endif
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
        case E1000_DEV_I219_LM_1A1E:   /* Dell 5530 */
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
    if (is_i219() && aperture < 0x5bc0u) {
        console_set_quiet(false);
        serial_puts("[NET 2b] I219 BAR aperture too small; DMA unavailable\n");
        return false;
    }

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
        /* PCH NVM differs from legacy EERD. Do not guess its access format. */
        if (is_i219()) {
            console_set_quiet(false);
            serial_puts("[NET 2b] Invalid firmware MAC; PCH NVM recovery unavailable\n");
            return false;
        }
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
    /* Keep the Phase 2a QEMU path unchanged; PCH has separate takeover/reset. */
    if (s_e1000_dev.pci.device_id == E1000_DEV_82540EM ||
        s_e1000_dev.pci.device_id == E1000_DEV_82574L)
        return s_e1000_dev.link_up ? e1000_init_rings() : e1000_wait_for_cable();
    if (is_i219()) return e1000_i219_init();
    return true;
}

const e1000_device_t *e1000_get_active_device(void) {
    return s_e1000_found ? &s_e1000_dev : NULL;
}

/* Intel 8254x SDM chapters 3/14: legacy descriptors, polling only.
 * x86 coherent DMA: volatile status observes device writes; compiler barriers
 * order descriptor stores before MMIO doorbells and payload reads after DD. */
#define NET_RING_COUNT 64u
#define NET_RX_POOL 80u
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH   0x2810
#define REG_RDT   0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH   0x3810
#define REG_TDT   0x3818
#define REG_TIPG  0x0410
#define REG_RFCTL 0x5008
#define DESC_DD   1u
#define RX_EOP    2u
#define NET_POLL_LIMIT 1000000u

typedef struct {
    uint64_t buffer_addr;
    uint16_t length, csum;
    uint8_t status, errors;
    uint16_t special;
} net_rx_desc_t;
typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} net_tx_desc_t;
_Static_assert(sizeof(net_rx_desc_t) == 16, "RX legacy descriptor");
_Static_assert(sizeof(net_tx_desc_t) == 16, "TX legacy descriptor");
_Static_assert(sizeof(pbuf_t) <= PAGE_SIZE, "pbuf must fit a DMA page");
_Static_assert(offsetof(pbuf_t, data) % 16 == 0, "payload alignment");

static spinlock_t g_net_dev_lock = SPINLOCK_RANKED(1, "net_dev");
static bool g_net_fatal;
static bool s_dma_attempted;
static net_dev_t s_net_dev;
/* Link state is independent of DMA ownership: DOWN retains activated rings. */
static enum { LINK_UNINITIALIZED, LINK_WAITING, LINK_ONLINE, LINK_DOWN,
              LINK_FAILED } s_link_state;
static uint64_t s_link_check;
static uint64_t s_rx_packet_count;
static uint64_t s_tx_packet_count;

static void e1000_publish_interface(void) {
    memcpy(s_net_dev.name, "eth0", 5);
    memcpy(s_net_dev.mac_addr, s_e1000_dev.mac_addr, 6);
    s_net_dev.mtu = 1500;
    s_net_dev.send_packet = e1000_send_raw;
    s_net_dev.poll_rx = e1000_poll_rx;
    s_net_dev.recycle_rx = e1000_recycle_rx;
    s_net_dev.priv = &s_e1000_dev;
}

static bool e1000_wait_for_cable(void) {
    e1000_publish_interface();
    s_link_state = LINK_WAITING;
    serial_puts("[NET link] Waiting for cable; DMA not allocated\n");
    return true;
}
static volatile net_rx_desc_t *s_rx;
static volatile net_tx_desc_t *s_tx;
static uintptr_t s_rx_phys, s_tx_phys;
static uintptr_t s_rx_pages[NET_RX_POOL], s_tx_pages[NET_RING_COUNT];
static pbuf_t *s_rx_packets[NET_RX_POOL];
/* Pool state: 0 spare, 1 ring-owned, 2 detached/caller-owned. */
static uint8_t s_rx_state[NET_RX_POOL];
static unsigned s_rx_slot[NET_RING_COUNT];
static bool s_tx_busy[NET_RING_COUNT];
static unsigned s_rx_next, s_tx_next;
static unsigned s_tx_pending;
static bool s_rx_discard;

static void dma_barrier(void) { __asm__ volatile("" ::: "memory"); }
static void net_command(uint16_t command) {
    const pci_device_t *p = &s_e1000_dev.pci;
    pci_write_config16(p->segment, p->bus, p->device, p->function,
                       PCI_REG_COMMAND, command);
}
static uint16_t net_command_read(void) {
    const pci_device_t *p = &s_e1000_dev.pci;
    return pci_read_config16(p->segment, p->bus, p->device, p->function,
                             PCI_REG_COMMAND);
}

/* PCH SPT/CNP only. Register/bit definitions and ordering are from Intel's
 * e1000e v6.12 ich8lan.{c,h}, netdev.c, mac.c, regs.h and defines.h.
 * See docs/roadmap/net-phase2b.md for the operation-by-operation rationale.
 * This is a MAC-only takeover of an already negotiated PHY, not PHY recovery. */
#define PCH_CTRL_EXT 0x0018u
#define PCH_MDIC 0x0020u
#define PCH_EXTCNF 0x0f00u
#define PCH_IOSFPC 0x0f28u
#define PCH_ECC 0x100cu
#define PCH_KABGTXD 0x3004u
#define PCH_GCR 0x5b00u
#define PCH_FWSM 0x5b54u
#define PCH_FEXTNVM11 0x5bbcu
#define PCH_SWFLAG (1u << 5)
#define PCH_MASTER_DISABLE (1u << 2)
#define PCH_MASTER_ACTIVE (1u << 19)
#define PCH_RESET (1u << 26)
#define PCH_PHY_RESET (1u << 31)
#define PCH_DESC_STATUS 0xe4u
#define PCH_FLUSH_REQUIRED (1u << 8)

static const uint32_t s_pch_diag_regs[] = {
    E1000_REG_STATUS, E1000_REG_CTRL, PCH_CTRL_EXT, PCH_MDIC,
    PCH_EXTCNF, PCH_FWSM, E1000_REG_RCTL, E1000_REG_TCTL,
    REG_RDBAL, REG_RDBAH, REG_RDLEN, REG_RDH, REG_RDT,
    REG_TDBAL, REG_TDBAH, REG_TDLEN, REG_TDH, REG_TDT,
    0x3828, 0x3840, REG_RFCTL, PCH_ECC, PCH_FEXTNVM11
};
typedef struct {
    uint32_t regs[sizeof(s_pch_diag_regs) / sizeof(s_pch_diag_regs[0])];
    uint16_t command, desc_status, device_id;
    unsigned rx_next, tx_next, tx_pending;
    const char *reason; /* Static literal, never a borrowed caller string. */
} pch_snapshot_t;
static pch_snapshot_t s_pch_failure, s_pch_before_reset;
static bool s_pch_failed, s_pch_attempted, s_pch_mmio_safe = true;
static bool s_pch_tx_trial;
static bool s_tx_report_done;
static bool s_tx_bringup_report;
static const char *s_pch_reset_error;
static const char *s_pch_stop_result;
static const char *s_pch_stop_reset_error;

/* PIT channel 2, as xhci.c's boot delay: independent of scheduler/IF.
 * No concurrent PCH lifecycle operations: boot/first-link takeover and a
 * latched fatal stop are BSP-owned. Restore speaker/gate on every outcome. */
static bool pch_delay_ms(unsigned ms) {
#ifdef E1000_PCH_HOST_TEST
    return e1000_mock_delay(ms);
#else
    for (unsigned n = 0; n < ms; ++n) {
        uint8_t saved = inb(0x61);
        outb(0x61, saved & ~3u);
        outb(0x43, 0xb0);
        outb(0x42, (uint8_t)1193);
        outb(0x42, (uint8_t)(1193 >> 8));
        outb(0x61, (saved & ~3u) | 1);
        bool done = false;
        for (unsigned i = 0; i < NET_POLL_LIMIT; ++i) {
            if (inb(0x61) & 0x20) { done = true; break; }
            __asm__ volatile("pause" ::: "memory");
        }
        outb(0x61, saved);
        if (!done) return false;
    }
    return true;
#endif
}

/* Caller holds net_dev lock. No allocation, logging, DMA dereference or
 * clear-on-read ICR/statistics. The first failure is immutable until reboot. */
static void pch_snapshot(pch_snapshot_t *record, const char *reason) {
    for (unsigned i = 0; i < sizeof(s_pch_diag_regs) / sizeof(s_pch_diag_regs[0]); ++i)
        record->regs[i] = e1000_read32(s_e1000_dev.mmio_virt, s_pch_diag_regs[i]);
    const pci_device_t *p = &s_e1000_dev.pci;
    record->command = net_command_read();
    record->desc_status = pci_read_config16(p->segment, p->bus, p->device,
                                          p->function, PCH_DESC_STATUS);
    record->device_id = p->device_id;
    record->rx_next = s_rx_next;
    record->tx_next = s_tx_next;
    record->tx_pending = s_tx_pending;
    record->reason = reason;
}
static void pch_capture(const char *reason) {
    if (s_pch_failed) return;
    if (s_pch_mmio_safe) pch_snapshot(&s_pch_failure, reason);
    else {
        s_pch_failure = s_pch_before_reset;
        s_pch_failure.reason = "post-reset timer failed; values are PRE-RESET";
    }
    s_pch_failed = true;
}
/* serial_puts already mirrors to the initialized, non-quiet framebuffer and
 * retains the dmesg log. Do not print twice to the framebuffer. */
static void pch_puts(const char *s) { serial_puts(s); }
static void pch_hex(uint32_t value) {
    char text[9];
    for (unsigned i = 0; i < 8; ++i) text[i] = "0123456789ABCDEF"[(value >> (28 - i * 4)) & 15];
    text[8] = 0;
    pch_puts(text);
}
/* Read-only TX checkpoints, independent of fatal containment. These are
 * documented e1000e register offsets (regs.h), not clear-on-read counters. */
static const uint32_t s_pch_tx_regs[] = {
    REG_TDBAL, REG_TDBAH, REG_TDLEN, REG_TDH, REG_TDT,
    REG_TIPG, E1000_REG_TCTL, 0x3828,
    0x1000, 0x1008, PCH_GCR, PCH_IOSFPC, 0x3928, 0x3940,
    0x3820, 0x382c, 0x0010, 0x00e4, 0x5bb4, PCH_FEXTNVM11,
    0x3410, 0x3418, 0x3420, 0x3428, 0x3430
};
#define PCH_TX_REG_COUNT (sizeof(s_pch_tx_regs) / sizeof(s_pch_tx_regs[0]))
static void pch_tx_core_copy(uint32_t *values) {
    values[0] = e1000_read32(s_e1000_dev.mmio_virt, REG_TIPG);
    values[1] = e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_TCTL);
    values[2] = e1000_read32(s_e1000_dev.mmio_virt, 0x3828);
    values[3] = e1000_read32(s_e1000_dev.mmio_virt, 0x3840);
    values[4] = e1000_read32(s_e1000_dev.mmio_virt, 0x3940);
}
static void pch_tx_core_report(const char *stage, const uint32_t *values) {
    spin_debug_assert_unheld();
    pch_puts("[NET 2b] TX checkpoint: "); pch_puts(stage); pch_puts("\n");
    const uint32_t offsets[] = {REG_TIPG, E1000_REG_TCTL, 0x3828, 0x3840, 0x3940};
    for (unsigned i = 0; i < 5; ++i) {
        pch_puts(" tx-reg "); pch_hex(offsets[i]); pch_puts(" = ");
        pch_hex(values[i]); pch_puts("\n");
    }
    pch_puts("[NET 2b] TIPG IPGT/IPGR1/IPGR2 (hex)=");
    pch_hex(values[0] & 0x3ffu); pch_puts("/");
    pch_hex((values[0] >> 10) & 0x3ffu); pch_puts("/");
    pch_hex((values[0] >> 20) & 0x3ffu); pch_puts("\n");
    pch_puts("[NET 2b] TARC0 SPT request field (expected 20000000)=");
    pch_hex(values[3] & 0x30000000u);
    pch_puts(" required bits23/24/26/27 (expected 0D800000)=");
    pch_hex(values[3] & 0x0d800000u); pch_puts("\n");
}
/* Read-only boot diagnostic. HHDM mappings are static after VMM init.
 * CPU page tables do not define device DMA permissions or prove NIC visibility. */
typedef struct { uint64_t entries[4]; uintptr_t phys; unsigned level; bool mapped; } pch_page_view_t;
static pch_page_view_t pch_page_view(uint64_t *root, uintptr_t va) {
    pch_page_view_t view = {0};
    uint64_t *table = root;
    if (!table) return view;
    for (unsigned level = 0; level < 4; ++level) {
        unsigned shift = 39 - 9 * level;
        uint64_t entry = table[(va >> shift) & 511u];
        view.entries[level] = entry; view.level = level;
        if (!(entry & PTE_PRESENT)) return view;
        if (level == 3 || ((level == 1 || level == 2) && (entry & PTE_HUGE))) {
            uintptr_t mask = ((uintptr_t)1 << shift) - 1;
            view.phys = (entry & PTE_ADDR_MASK & ~mask) | (va & mask);
            view.mapped = true; return view;
        }
        if (level == 0 && (entry & PTE_HUGE)) return view;
        table = vmm_phys_to_virt(entry & PTE_ADDR_MASK);
    }
    return view;
}
static void pch_hex64(uint64_t value) { pch_hex(value >> 32); pch_hex(value); }
static void pch_tx_memory_report(const uint8_t *descriptors, uintptr_t phys,
                                  uintptr_t va, const pch_page_view_t *page) {
    for (unsigned d = 0; d < 2; ++d) {
        pch_puts("[NET 2b] TX descriptor "); pch_hex(d);
        pch_puts(" physical-derived CPU PRE-TDT bytes:");
        for (unsigned n = 0; n < 16; ++n) {
            uint8_t b = descriptors[d * 16 + n];
            char byte[4] = {' ', "0123456789ABCDEF"[b >> 4], "0123456789ABCDEF"[b & 15], 0};
            pch_puts(byte);
        }
        pch_puts("\n");
    }
    pch_puts("[NET 2b] TX PMM-owned frame page physical/CPU VA=");
    pch_hex64(phys); pch_puts("/"); pch_hex64(va); pch_puts("\n");
    pch_puts("[NET 2b] CPU page walk mapped/leaf-level=");
    pch_hex(page->mapped); pch_puts("/"); pch_hex(page->level); pch_puts("\n");
    for (unsigned i = 0; i <= page->level; ++i) {
        pch_puts(" page-entry "); pch_hex(i); pch_puts(" = ");
        pch_hex64(page->entries[i]); pch_puts("\n");
    }
    if (page->mapped) {
        uintptr_t last = page->phys + PAGE_SIZE - 1;
        pch_puts("[NET 2b] CPU resolved first/last physical=");
        pch_hex64(page->phys); pch_puts("/"); pch_hex64(last);
        pch_puts(page->phys == phys ? " MATCH\n" : " MISMATCH\n");
        uint64_t leaf = page->entries[page->level];
        unsigned pat = (leaf & PTE_PWT ? 1 : 0) | (leaf & PTE_PCD ? 2 : 0) |
                       (leaf & (page->level == 3 ? 1ull << 7 : 1ull << 12) ? 4 : 0);
        pch_puts("[NET 2b] CPU leaf PAT index (effective type also depends on PAT/MTRRs)=");
        pch_hex(pat); pch_puts("\n");
    }
    pch_puts("[NET 2b] CPU mapping/readback is not a NIC visibility or DMA permission proof\n");
}
static void pch_tx_copy(uint32_t *values) {
    for (unsigned i = 0; i < PCH_TX_REG_COUNT; ++i)
        values[i] = e1000_read32(s_e1000_dev.mmio_virt, s_pch_tx_regs[i]);
}
#include "e1000_diag.h"
static void pch_tx_report(const char *stage, const uint32_t *values) {
    spin_debug_assert_unheld();
    pch_puts("[NET 2b] TX checkpoint: "); pch_puts(stage); pch_puts("\n");
    pch_puts("[NET 2b] 00003410=TDFH, 00003418=TDFT: internal TX data FIFO pointers, not ring-base mirrors\n");
    for (unsigned i = 0; i < PCH_TX_REG_COUNT; ++i) {
        pch_puts(" tx-reg "); pch_hex(s_pch_tx_regs[i]);
        pch_puts(" = "); pch_hex(values[i]);
        if (s_pch_tx_regs[i] == PCH_GCR)
            pch_puts((values[i] & 0x3fu) ? " GCR: no-snoop bits set (PCH reference expects clear)" :
                     " GCR: no-snoop bits clear (matches PCH reference mask; upper bits not compared)");
        if (s_pch_tx_regs[i] == PCH_IOSFPC && s_e1000_dev.pci.device_id == E1000_DEV_I219_LM_15D7)
            pch_puts((values[i] & (1u << 16)) ? " IOSFPC: SPT workaround bit 16 set (matches reference)" :
                     " IOSFPC: SPT workaround bit 16 CLEAR");
        pch_puts("\n");
    }
}
/* Thread/boot caller, no locks held. Print only copied state after containment. */
static void pch_report(void) {
    spin_debug_assert_unheld();
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    pch_snapshot_t record = s_pch_failure;
    const char *result = s_pch_stop_result;
    const char *reset_error = s_pch_stop_reset_error;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    console_set_quiet(false);
    pch_puts("[NET 2b] STOP: "); pch_puts(record.reason); pch_puts("\n");
    for (unsigned i = 0; i < sizeof(s_pch_diag_regs) / sizeof(s_pch_diag_regs[0]); ++i) {
        pch_puts(" reg "); pch_hex(s_pch_diag_regs[i]);
        pch_puts(" = "); pch_hex(record.regs[i]); pch_puts("\n");
    }
    pch_puts(" PCI command/descriptor-status (at capture): ");
    pch_hex(record.command); pch_puts("/"); pch_hex(record.desc_status);
    pch_puts("\n SW rx-next/tx-next/pending: "); pch_hex(record.rx_next);
    pch_puts("/"); pch_hex(record.tx_next); pch_puts("/"); pch_hex(record.tx_pending);
    pch_puts("\n[NET 2b] Networking unavailable; DMA retained until reboot\n");
    if (result) { pch_puts("[NET 2b] Containment: "); pch_puts(result); pch_puts("\n"); }
    if (reset_error) { pch_puts("[NET 2b] Reset: "); pch_puts(reset_error); pch_puts("\n"); }
}
static bool pch_wait(uint32_t reg, uint32_t mask, bool set, unsigned ms) {
    for (unsigned i = 0; i <= ms; ++i) {
        if (!!(e1000_read32(s_e1000_dev.mmio_virt, reg) & mask) == set) return true;
        if (i == ms || !pch_delay_ms(1)) break;
    }
    return false;
}

/* Called with net_dev held and IRQs excluded. No scheduler, IF changes or
 * logging. Abandon an unsafe reset rather than flushing unknown firmware
 * descriptors or submitting an extra packet on a fatal DMA path. */
static bool pch_reset(void) {
    uintptr_t base = s_e1000_dev.mmio_virt;
    const pci_device_t *p = &s_e1000_dev.pci;
    s_pch_reset_error = "pending PCIe master requests";
    e1000_write32(base, E1000_REG_CTRL,
                  e1000_read32(base, E1000_REG_CTRL) | PCH_MASTER_DISABLE);
    if (!pch_wait(E1000_REG_STATUS, PCH_MASTER_ACTIVE, false, 100)) return false;
    e1000_write32(base, E1000_REG_IMC, 0xffffffffu);
    e1000_write32(base, E1000_REG_RCTL, 0);
    e1000_write32(base, E1000_REG_TCTL, 1u << 3);
    (void)e1000_read32(base, E1000_REG_STATUS);
    s_pch_reset_error = "pre-reset timer failed";
    if (!pch_delay_ms(10)) return false;
    /* netdev.c e1000_flush_desc_rings: never reset an I219 requiring flush.
     * We retain DMA and stop instead; no restart/PM path requires reclamation. */
    e1000_write32(base, PCH_FEXTNVM11,
                  e1000_read32(base, PCH_FEXTNVM11) | (1u << 13));
    s_pch_reset_error = "descriptor flush required; reset skipped";
    if (e1000_read32(base, REG_TDLEN) &&
        (pci_read_config16(p->segment, p->bus, p->device, p->function,
                           PCH_DESC_STATUS) & PCH_FLUSH_REQUIRED)) return false;
    s_pch_reset_error = "firmware/software ownership busy";
    if (!pch_wait(PCH_EXTCNF, PCH_SWFLAG, false, 100)) return false;
    e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) | PCH_SWFLAG);
    if (!pch_wait(PCH_EXTCNF, PCH_SWFLAG, true, 100)) {
        e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) & ~PCH_SWFLAG);
        return false;
    }
    pch_snapshot(&s_pch_before_reset, "before MAC reset");
    /* Preserve the negotiated PHY/CSME state: no PHY_RST during reset.
     * SPT link-up MDIC configuration runs separately after this reset.
     * ich8lan.c warns MMIO flush immediately after CTRL.RST can hang. */
    uint32_t ctrl = e1000_read32(base, E1000_REG_CTRL) & ~PCH_PHY_RESET;
    s_pch_reset_error = "post-reset timer failed";
    e1000_write32(base, E1000_REG_CTRL, ctrl | PCH_RESET);
    s_pch_mmio_safe = false;
    if (!pch_delay_ms(20)) return false; /* no further MMIO if delay fails */
    s_pch_mmio_safe = true;
    s_pch_reset_error = "MAC reset timed out";
    bool done = pch_wait(E1000_REG_CTRL, PCH_RESET, false, 100);
    /* Only release a semaphore we acquired; never steal firmware's flag. */
    e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) & ~PCH_SWFLAG);
    net_command(net_command_read() & ~PCI_COMMAND_BUS_MASTER);
    return done;
}

static void pch_configure_dma(uint32_t tctl) {
    uintptr_t base = s_e1000_dev.mmio_virt;
    /* The initial reset does not preserve this setting on the Dell 5590
     * (user log: FEXTNVM11=FB21C1C2). Reapply before opening the datapath,
     * as iPXE intel_open does for INTEL_I219/INTEL_RST_HANG devices.
     * Pre-reset programming in pch_reset remains independently required. */
    e1000_write32(base, PCH_FEXTNVM11,
                  e1000_read32(base, PCH_FEXTNVM11) | (1u << 13));
    e1000_write32(base, PCH_CTRL_EXT,
                  e1000_read32(base, PCH_CTRL_EXT) | (1u << 22) | (1u << 17) | (1u << 28));
    e1000_write32(base, PCH_GCR, e1000_read32(base, PCH_GCR) & ~0x3fu);
    for (unsigned q = 0; q < 2; ++q) {
        uint32_t off = 0x3828u + q * 0x100u;
        uint32_t value = e1000_read32(base, off);
        /* PCH legacy TXDCTL: PTHRESH=31, HTHRESH=1, WTHRESH=1,
         * descriptor granularity and the shared-code raw bit 22 setting.
         * HTHRESH must be programmed too; preserving reset zero omitted it.
         * Reference: FreeBSD e1000 commit 59709be69b07, em_legacy_txdctl.
         * Bit 25 is not the igb/ixgbe queue-enable field on this MAC. */
        value = (value & ~0x003f3f3fu) | (1u << 22) | 0x0101011fu;
        e1000_write32(base, off, value);
    }
    uint32_t tarc = e1000_read32(base, 0x3840) |
                    (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27);
    if (s_e1000_dev.pci.device_id == E1000_DEV_I219_LM_15D7) {
        /* netdev.c SPT/KBL data-corruption and Tx-hang errata. Not CNP. */
        e1000_write32(base, PCH_IOSFPC, e1000_read32(base, PCH_IOSFPC) | (1u << 16));
        tarc = (tarc & ~(3u << 28)) | (2u << 28);
    }
    e1000_write32(base, 0x3840, tarc);
    /* ich8lan initialize_hw_bits: TARC1 bit28 is inverse of TCTL.MULR. */
    uint32_t tarc1 = e1000_read32(base, 0x3940) | (1u << 24) | (1u << 26) | (1u << 30);
    if (tctl & (1u << 28)) tarc1 &= ~(1u << 28);
    else tarc1 |= (1u << 28);
    e1000_write32(base, 0x3940, tarc1);
    e1000_write32(base, REG_RFCTL, 0xc0u); /* legacy, NFS filters disabled */
    e1000_write32(base, PCH_ECC, e1000_read32(base, PCH_ECC) | (1u << 16));
    e1000_write32(base, E1000_REG_CTRL,
                  (e1000_read32(base, E1000_REG_CTRL) & ~PCH_MASTER_DISABLE) | (1u << 19));
    e1000_write32(base, PCH_KABGTXD, e1000_read32(base, PCH_KABGTXD) | 0x50000u);
    /* Restore only our primary address after MAC reset; do not touch ME's
     * shared receive-address slots. Disable multicast hash acceptance. */
    uint32_t low = 0, high = E1000_RAH_AV;
    for (unsigned i = 0; i < 4; ++i) low |= (uint32_t)s_e1000_dev.mac_addr[i] << (i * 8);
    high |= s_e1000_dev.mac_addr[4] | ((uint32_t)s_e1000_dev.mac_addr[5] << 8);
    e1000_write32(base, E1000_REG_RAL0, low);
    e1000_write32(base, E1000_REG_RAH0, high);
    for (unsigned i = 0; i < 32; ++i) e1000_write32(base, 0x5200 + i * 4, 0);
    /* PCH copper IPGT=8 at 1 Gb/s; full-duplex 10/100 uses 12. */
    uint32_t status = e1000_read32(base, E1000_REG_STATUS);
    unsigned ipgt = (status & E1000_STATUS_SPEED_MASK) == E1000_STATUS_SPEED_1000 ? 8 : 12;
    e1000_write32(base, REG_TIPG, ipgt | (8u << 10) | (6u << 20));
}

static bool pch_dma_ready(void) {
    uintptr_t base = s_e1000_dev.mmio_virt;
    return (e1000_read32(base, E1000_REG_STATUS) & E1000_STATUS_LU) &&
        (e1000_read32(base, E1000_REG_RCTL) & (1u << 1)) &&
        (e1000_read32(base, E1000_REG_TCTL) & (1u << 1)) &&
        !(e1000_read32(base, E1000_REG_CTRL) & PCH_MASTER_DISABLE) &&
        (e1000_read32(base, PCH_ECC) & (1u << 16)) &&
        (e1000_read32(base, PCH_FEXTNVM11) & (1u << 13)) &&
        e1000_read32(base, REG_RFCTL) == 0xc0u &&
        (e1000_read32(base, 0x3828) & 0x017f3f3fu) == 0x0141011fu &&
        (e1000_read32(base, 0x3928) & 0x017f3f3fu) == 0x0141011fu &&
        e1000_read32(base, REG_TDBAL) == (uint32_t)s_tx_phys &&
        e1000_read32(base, REG_TDBAH) == (uint32_t)(s_tx_phys >> 32) &&
        e1000_read32(base, REG_RDBAL) == (uint32_t)s_rx_phys &&
        e1000_read32(base, REG_RDBAH) == (uint32_t)(s_rx_phys >> 32) &&
        e1000_read32(base, REG_TDLEN) == NET_RING_COUNT * 16 &&
        e1000_read32(base, REG_RDLEN) == NET_RING_COUNT * 16;
}

/* Narrow SPT link-up subset of e1000_check_for_copper_link_ich8lan (v6.12).
 * HV pages >=768 use PHY address 1 and register 31 selects page*32.
 * Device lock held; no PHY reset, autoneg restart or SMBus override. */
static bool pch_mdic(bool write, unsigned reg, uint16_t *data, bool *idle) {
    uintptr_t base = s_e1000_dev.mmio_virt;
    uint32_t command = (1u << 21) | (reg << 16) |
                       (write ? 0x04000000u | *data : 0x08000000u);
    *idle = false;
    e1000_write32(base, PCH_MDIC, command);
    for (unsigned i = 0; i < 10; ++i) {
        if (!pch_delay_ms(1)) return false;
        uint32_t result = e1000_read32(base, PCH_MDIC);
        if (!(result & 0x10000000u)) continue;
        *idle = true;
        if ((result & 0x40000000u) ||
            (result & 0x03ff0000u) != (command & 0x03ff0000u)) return false;
        if (!write) *data = (uint16_t)result;
        /* PCH requires a delay after each completed MDIC transaction. */
        return pch_delay_ms(1);
    }
    return false;
}
static bool pch_phy_rmw(unsigned page, unsigned reg, uint16_t clear,
                        uint16_t set, bool minimum_gap, bool *idle) {
    uint16_t value = (uint16_t)(page << 5);
    if (!pch_mdic(true, 31, &value, idle) ||
        !pch_mdic(false, reg, &value, idle)) return false;
    if (minimum_gap && ((value >> 2) & 0x3ffu) >= 0x18u) return true;
    value = (value & ~clear) | set;
    if (!pch_mdic(true, reg, &value, idle)) return false;
    uint16_t check = 0;
    return pch_mdic(false, reg, &check, idle) && check == value;
}
static bool pch_spt_link_setup(void) {
    if (s_e1000_dev.pci.device_id != E1000_DEV_I219_LM_15D7) return true;
    uintptr_t base = s_e1000_dev.mmio_virt;
    if (!pch_wait(PCH_EXTCNF, PCH_SWFLAG, false, 100)) return false;
    e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) | PCH_SWFLAG);
    if (!pch_wait(PCH_EXTCNF, PCH_SWFLAG, true, 100)) {
        e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) & ~PCH_SWFLAG);
        return false;
    }
    bool idle = true;
    uint16_t saved_page = 0;
    bool saved = pch_mdic(false, 31, &saved_page, &idle);
    uint32_t status = e1000_read32(base, E1000_REG_STATUS);
    bool gigabit = (status & E1000_STATUS_SPEED_MASK) == E1000_STATUS_SPEED_1000;
    bool ok = saved && pch_phy_rmw(772, 28, 0x07ff, gigabit ? 0x00fa : 0x03e8, false, &idle);
    if (ok && gigabit) ok = pch_phy_rmw(770, 17, 0, 0x0200, false, &idle);
    if (ok) ok = pch_phy_rmw(776, 20, gigabit ? 0x0ffc : 0xffff,
                             gigabit ? 0x0060 : 0xc023, gigabit, &idle);
    /* Do not issue another command over an uncompleted MDIC transaction. */
    if (saved && idle) {
        bool restored = pch_mdic(true, 31, &saved_page, &idle);
        ok = ok && restored;
    } else ok = false;
    if (ok) {
        uint32_t ctrl = e1000_read32(base, E1000_REG_CTRL);
        e1000_write32(base, E1000_REG_CTRL, (ctrl | 0x40u) & ~0x1800u);
        /* Reference beacon duration =8us; SPT K1-off follows PCIEANACFG. */
        e1000_write32(base, 0x24, e1000_read32(base, 0x24) | 7u);
        uint32_t fext = e1000_read32(base, 0x10);
        fext = (fext & ~0x80000000u) | (e1000_read32(base, 0xf18) & 0x80000000u);
        e1000_write32(base, 0x10, fext);
    }
    e1000_write32(base, PCH_EXTCNF, e1000_read32(base, PCH_EXTCNF) & ~PCH_SWFLAG);
    return ok;
}
static bool e1000_i219_init(void) {
    spin_debug_assert_unheld();
    if (s_pch_attempted || g_net_fatal) return false;
    if (!pmm_high_memory_enabled()) return false;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    bool ready=false, timer_ok=true;
    for (unsigned i=0; i<=500; ++i) {
        if (e1000_read32(s_e1000_dev.mmio_virt,E1000_REG_STATUS)&E1000_STATUS_LU) {
            ready=true; break;
        }
        if (i==500) break;
        if (!pch_delay_ms(1)) { timer_ok=false; break; }
    }
    if (!ready && timer_ok) {
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        return e1000_wait_for_cable();
    }
    s_pch_attempted = true; /* Only an actual takeover consumes the one attempt. */
    const char *reason = "link observation timer failed";
    if (ready) { ready = pch_reset(); reason = s_pch_reset_error; }
    if (ready) {
        ready = pch_wait(E1000_REG_STATUS, E1000_STATUS_LU, true, 500);
        reason = "link lost after MAC reset";
    }
    if (ready) {
        ready = pch_spt_link_setup();
        reason = "SPT PHY link-up configuration failed; no DMA started";
    }
    if (!ready) pch_capture(reason);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    if (!ready) { (void)e1000_quiesce(); return false; }
    if (s_e1000_dev.pci.device_id == E1000_DEV_I219_LM_15D7)
        pch_puts("[NET 2b] SPT PHY link-up configuration verified (PLL/K1/FIFO gap)\n");
    s_e1000_dev.link_up = true;
    return e1000_init_rings();
}

bool e1000_quiesce(void) {
    spin_debug_assert_unheld();
    if (!s_e1000_found) return false;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    s_link_state = LINK_FAILED;
    if (is_i219()) {
        bool already_fatal = g_net_fatal;
        g_net_fatal = true;
        s_net_dev.flags = 0;
        pch_capture("fatal DMA stop"); /* capture before disabling/resetting */
        bool stopped = false;
        /* A bring-up failure has already made one bounded attempt. Do not
         * retry ownership/reset, and do not touch MMIO after a timer failure. */
        if (!already_fatal && s_pch_mmio_safe) {
            uintptr_t base = s_e1000_dev.mmio_virt;
            e1000_write32(base, E1000_REG_IMC, 0xffffffffu);
            e1000_write32(base, E1000_REG_RCTL, 0);
            e1000_write32(base, E1000_REG_TCTL, 0);
            if (!strcmp(s_pch_failure.reason, "fatal DMA stop"))
                stopped = pch_reset();
        }
        net_command((net_command_read() & ~PCI_COMMAND_BUS_MASTER) | PCI_COMMAND_INT_DISABLE);
        bool mastering_off = !(net_command_read() & PCI_COMMAND_BUS_MASTER);
        if (!already_fatal) s_pch_stop_result = !mastering_off ?
            "PCI bus-master disable FAILED; all DMA quarantined" :
            stopped ? "MAC reset complete; PCI bus mastering verified off" :
            "reset skipped/failed; PCI bus mastering verified off; DMA quarantined";
        if (!already_fatal && !stopped) s_pch_stop_reset_error = s_pch_reset_error;
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        pch_report();
        return stopped && mastering_off;
    }
    g_net_fatal = true;
    s_net_dev.flags = 0;
    uintptr_t base = s_e1000_dev.mmio_virt;
    e1000_write32(base, E1000_REG_IMC, 0xffffffffu);
    e1000_write32(base, E1000_REG_RCTL, 0);
    e1000_write32(base, E1000_REG_TCTL, 0);
    (void)e1000_read32(base, E1000_REG_STATUS); /* flush posted writes */
    net_command(net_command_read() & ~PCI_COMMAND_BUS_MASTER);
    /* e1000 has no xHCI-like HALTED bit. A bounded global reset supplies
     * a completion indication; engine-disable readback alone is not proof.
     * No path frees/reuses these pages, irrespective of reset completion. */
    e1000_write32(base, E1000_REG_CTRL,
                  e1000_read32(base, E1000_REG_CTRL) | (1u << 26));
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    bool reset_done = false;
    for (unsigned i = 0; i < NET_POLL_LIMIT; ++i) {
        if (!(e1000_read32(base, E1000_REG_CTRL) & (1u << 26))) {
            reset_done = true;
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    /* Reset may alter PCI command state. Keep mastering disabled. */
    irq = spin_lock_irqsave(&g_net_dev_lock);
    net_command(net_command_read() & ~PCI_COMMAND_BUS_MASTER);
    bool mastering_off = !(net_command_read() & PCI_COMMAND_BUS_MASTER);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    return reset_done && mastering_off;
}

static bool e1000_init_rings(void) {
    spin_debug_assert_unheld();
    if (s_dma_attempted) return false;
    s_dma_attempted = true;
    if (!pmm_high_memory_enabled()) return false;
    uintptr_t base = s_e1000_dev.mmio_virt;
    /* Linux configure_tx preserves post-reset TCTL outside configured fields.
     * Capture before our engine-disable write; do not invent unnamed bit29. */
    uint32_t inherited_tctl = is_i219() ? e1000_read32(base, E1000_REG_TCTL) : 0;
    e1000_write32(base, E1000_REG_IMC, 0xffffffffu);
    (void)e1000_read32(base, E1000_REG_ICR);
    e1000_write32(base, E1000_REG_RCTL, 0);
    e1000_write32(base, E1000_REG_TCTL, 0);
    s_rx_phys = pmm_alloc_page();
    s_tx_phys = pmm_alloc_page();
    if (!s_rx_phys || !s_tx_phys) goto failed;
    s_rx = vmm_phys_to_virt(s_rx_phys);
    s_tx = vmm_phys_to_virt(s_tx_phys);
    memset((void *)s_rx, 0, PAGE_SIZE);
    memset((void *)s_tx, 0, PAGE_SIZE);
    for (unsigned i = 0; i < NET_RX_POOL; ++i) {
        s_rx_pages[i] = pmm_alloc_page();
        if (!s_rx_pages[i]) goto failed;
        s_rx_packets[i] = vmm_phys_to_virt(s_rx_pages[i]);
        memset(s_rx_packets[i], 0, PAGE_SIZE);
        pbuf_init(s_rx_packets[i]);
        s_rx_packets[i]->flags = PBUF_FLAG_DMA_BACKED;
        if (i < NET_RING_COUNT) {
            s_rx_slot[i] = i;
            s_rx_state[i] = 1;
            s_rx[i].buffer_addr = s_rx_pages[i] + offsetof(pbuf_t, data);
        }
    }
    for (unsigned i = 0; i < NET_RING_COUNT; ++i) {
        s_tx_pages[i] = pmm_alloc_page();
        if (!s_tx_pages[i]) goto failed;
        memset(vmm_phys_to_virt(s_tx_pages[i]), 0, PAGE_SIZE);
        s_tx[i].buffer_addr = s_tx_pages[i];
        s_tx[i].status = DESC_DD;
    }
    e1000_write32(base, REG_TDBAL, (uint32_t)s_tx_phys);
    e1000_write32(base, REG_TDBAH, (uint32_t)(s_tx_phys >> 32));
    e1000_write32(base, REG_TDLEN, NET_RING_COUNT * 16);
    e1000_write32(base, REG_TDH, 0);
    e1000_write32(base, REG_TDT, 0);
    e1000_write32(base, REG_RDBAL, (uint32_t)s_rx_phys);
    e1000_write32(base, REG_RDBAH, (uint32_t)(s_rx_phys >> 32));
    e1000_write32(base, REG_RDLEN, NET_RING_COUNT * 16);
    e1000_write32(base, REG_RDH, 0);
    e1000_write32(base, REG_RDT, NET_RING_COUNT - 1);
    if (is_i219()) {
        uint32_t tctl = (inherited_tctl & ~((1u << 1) | 0xff0u | 0x3ff000u)) |
                       (1u << 1) | (1u << 3) | (15u << 4) | (63u << 12) | (1u << 24);
        pch_configure_dma(tctl);
        /* netdev.c configure_tx + mac.c collision distance for PCH. */
        e1000_write32(base, E1000_REG_TCTL, tctl);
        e1000_write32(base, E1000_REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    }
    else {
        /* 82574: EXSTEN=0 selects the legacy RX descriptor format. */
        e1000_write32(base, REG_RFCTL, 0);
        /* Full duplex: CT=15, COLD=64; copper IPG 10/8/6. */
        e1000_write32(base, REG_TIPG, 10u | (8u << 10) | (6u << 20));
        e1000_write32(base, E1000_REG_TCTL, (1u << 1) | (1u << 3) |
                      (15u << 4) | (64u << 12));
        /* EN, BAM, 2048-byte buffers, strip CRC; no promiscuous reception. */
        e1000_write32(base, E1000_REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    }
    dma_barrier();
    (void)e1000_read32(base, E1000_REG_STATUS);
    net_command(net_command_read() | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE);
    if (!(net_command_read() & PCI_COMMAND_BUS_MASTER)) goto failed;
    if (is_i219() && !pch_dma_ready()) goto failed;
    e1000_publish_interface();
    s_net_dev.flags = NET_UP | (s_e1000_dev.link_up ? NET_RUNNING : 0);
    s_link_state = s_e1000_dev.link_up ? LINK_ONLINE : LINK_DOWN;
    if (is_i219()) {
        pch_puts("[NET 2b] TCTL post-reset/final="); pch_hex(inherited_tctl);
        pch_puts("/"); pch_hex(e1000_read32(base, E1000_REG_TCTL)); pch_puts("\n");
        pch_puts("[NET 2b] I219 eth0: polling DMA ready; physical acceptance pending\n");
    }
    else serial_puts("[NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)\n");
    return true;
failed:
    (void)e1000_quiesce();
    if (!is_i219()) serial_puts("[NET 2a] Bring-up failed; DMA pages retained, networking unavailable\n");
    return false;
}

net_dev_t *e1000_get_net_device(void) {
    spin_debug_assert_unheld();
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    net_dev_t *dev = !g_net_fatal && (s_net_dev.flags || s_link_state == LINK_WAITING) ? &s_net_dev : NULL;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    return dev;
}

bool e1000_network_online(net_dev_t *dev) {
    spin_debug_assert_unheld();
    uint64_t irq=spin_lock_irqsave(&g_net_dev_lock);
    bool online=dev==&s_net_dev && !g_net_fatal && s_link_state==LINK_ONLINE &&
        (s_net_dev.flags&NET_UP) &&
        (e1000_read32(s_e1000_dev.mmio_virt,E1000_REG_STATUS)&E1000_STATUS_LU);
    spin_unlock_irqrestore(&g_net_dev_lock,irq);
    return online;
}

uint32_t e1000_link_state_abi(const net_dev_t *dev) {
    if (dev != &s_net_dev) return NET_IF_LINK_UNINITIALIZED;
    spin_debug_assert_unheld();
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    if (g_net_fatal || s_link_state == LINK_FAILED) {
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        return NET_IF_LINK_FAILED;
    }
    uint32_t state = NET_IF_LINK_UNINITIALIZED;
    switch (s_link_state) {
        case LINK_UNINITIALIZED: state = NET_IF_LINK_UNINITIALIZED; break;
        case LINK_WAITING:       state = NET_IF_LINK_WAITING; break;
        case LINK_ONLINE:        state = NET_IF_LINK_ONLINE; break;
        case LINK_DOWN:          state = NET_IF_LINK_DOWN; break;
        case LINK_FAILED:        state = NET_IF_LINK_FAILED; break;
    }
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    return state;
}

void e1000_get_stats(const net_dev_t *dev, uint64_t *rx_packets, uint64_t *tx_packets) {
    if (dev != &s_net_dev) {
        if (rx_packets) *rx_packets = 0;
        if (tx_packets) *tx_packets = 0;
        return;
    }
    spin_debug_assert_unheld();
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    if (rx_packets) *rx_packets = s_rx_packet_count;
    if (tx_packets) *tx_packets = s_tx_packet_count;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
}

bool e1000_service_link(net_dev_t *dev, uint64_t now, uint64_t tick_hz) {
    spin_debug_assert_unheld();
    if (dev != &s_net_dev || g_net_fatal || s_link_state == LINK_FAILED ||
        s_link_state == LINK_UNINITIALIZED) return false;
    /* One read per 250 ms while waiting; online/down reads stay cheap and
     * observe loss before protocol sweeps. No PHY reset or autoneg restart. */
    if (s_link_state == LINK_WAITING && now < s_link_check) return false;
    uint64_t interval = tick_hz / 4;
    if (!interval) interval = 1;
    s_link_check = now > UINT64_MAX - interval ? UINT64_MAX : now + interval;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    bool up = (e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    if (s_link_state == LINK_WAITING) {
        if (!up) return false;
        s_e1000_dev.link_up = true;
        /* Same one-shot, bounded takeover as cable-present boot. No DMA or
         * protocol-table reinitialization ever runs on subsequent link flaps. */
        bool ready = is_i219() ? e1000_i219_init() : e1000_init_rings();
        if (!ready && !g_net_fatal) (void)e1000_quiesce();
        return ready && e1000_network_online(dev);
    }
    if (up != (s_link_state == LINK_ONLINE)) {
        /* Speed-dependent SPT PLL/K1/FIFO programming belongs to each link
         * acquisition. Reuse the validated bounded helper; no MAC reset,
         * autoneg restart, descriptor rewrite or allocation on a replug. */
        if (up && is_i219()) {
            irq=spin_lock_irqsave(&g_net_dev_lock);
            bool configured=pch_spt_link_setup();
            if (!configured) pch_capture("runtime link-up PHY configuration failed");
            spin_unlock_irqrestore(&g_net_dev_lock,irq);
            if (!configured) { (void)e1000_quiesce(); return false; }
        }
        s_link_state = up ? LINK_ONLINE : LINK_DOWN;
        s_e1000_dev.link_up = up;
        irq = spin_lock_irqsave(&g_net_dev_lock);
        if (up) s_net_dev.flags |= NET_RUNNING;
        else s_net_dev.flags &= ~NET_RUNNING;
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        serial_puts(up ? "[NET link] Online; retained rings\n" :
                        "[NET link] Cable disconnected; retained rings\n");
    }
    return up;
}

int e1000_send_raw(net_dev_t *dev, const void *buf, size_t len) {
    spin_debug_assert_unheld();
    if (dev != &s_net_dev || !buf || len < 14 || len > 1514) return -1;
    uint8_t posted[16];
    uint8_t physical_posted[16];
    uint8_t frame_view[60];
    size_t frame_view_len = len < sizeof(frame_view) ? len : sizeof(frame_view);
    bool frame_matches = false;
    uintptr_t physical_view = 0;
    uint32_t tx_checkpoint[PCH_TX_REG_COUNT];
    uint32_t core_checkpoint[5];
    uint8_t two_descriptors[32];
    pch_page_view_t page_view = {0};
    uintptr_t frame_va = 0;
    net_diag_t access_checkpoint;
    uint64_t posted_phys = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    unsigned slot = s_tx_next;
    if (g_net_fatal || !(dev->flags & NET_UP) ||
        !(e1000_read32(s_e1000_dev.mmio_virt, E1000_REG_STATUS) & E1000_STATUS_LU) ||
        s_tx_pending >= NET_RING_COUNT - 1 ||
        s_tx_busy[slot] || !(s_tx[slot].status & DESC_DD)) {
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        return -1;
    }
    s_tx_busy[slot] = true; /* stays reserved until this waiter consumes DD */
    /* Leave one entry unused: equal hardware head/tail means empty. */
    ++s_tx_pending;
    memcpy(vmm_phys_to_virt(s_tx_pages[slot]), buf, len);
    s_tx[slot].length = (uint16_t)len;
    s_tx[slot].cmd = 1u | 2u | 8u; /* EOP | IFCS | RS */
    s_tx[slot].status = 0;
    /* Preserve selftest TX bring-up evidence without logging normal traffic.
     * Claim under the device lock; print copied diagnostics after unlocking. */
    bool trace = is_i219() && slot == 0 && s_tx_bringup_report && !s_tx_report_done && !s_pch_tx_trial;
    bool core_trace = is_i219() && slot == 0 && s_tx_bringup_report && !s_tx_report_done && s_pch_tx_trial;
    if (trace || core_trace) s_tx_report_done = true;
    if (core_trace) {
        pch_tx_core_copy(core_checkpoint);
        __asm__ volatile("sfence" ::: "memory");
        const volatile uint8_t *raw = vmm_phys_to_virt(s_tx_phys);
        for (unsigned n = 0; n < 32; ++n) two_descriptors[n] = raw[n];
        frame_va = (uintptr_t)vmm_phys_to_virt(s_tx_pages[slot]);
        page_view = pch_page_view(vmm_get_kernel_pml4_virt(), frame_va);
    }
    if (trace) {
        /* Copy before the doorbell: hardware may immediately overwrite DD.
         * Print this immutable copy only after releasing the device lock. */
        const volatile uint8_t *raw = (const volatile uint8_t *)&s_tx[slot];
        for (unsigned n = 0; n < sizeof(posted); ++n) posted[n] = raw[n];
        posted_phys = s_tx_pages[slot];
        /* This is the same HHDM mapping used to establish s_tx, not an
         * uncached alias or a measurement of the NIC's DMA-visible bytes. */
        __asm__ volatile("sfence" ::: "memory");
        physical_view = (uintptr_t)vmm_phys_to_virt(s_tx_phys + slot * sizeof(*s_tx));
        const volatile uint8_t *physical_raw = (const volatile uint8_t *)physical_view;
        for (unsigned n = 0; n < sizeof(physical_posted); ++n)
            physical_posted[n] = physical_raw[n];
        const volatile uint8_t *frame_raw = vmm_phys_to_virt(s_tx_pages[slot]);
        for (size_t n = 0; n < frame_view_len; ++n) frame_view[n] = frame_raw[n];
        frame_matches = !memcmp(frame_view, buf, frame_view_len);
        pch_tx_copy(tx_checkpoint);
        net_diag_copy(&access_checkpoint);
    }
    s_tx_next = (slot + 1) % NET_RING_COUNT;
    dma_barrier();
    /* Explicit x86 store fence immediately before publishing the tail.
     * SFENCE orders stores; it does not invalidate/flush WB cache lines. */
    __asm__ volatile("sfence" ::: "memory");
    e1000_write32(s_e1000_dev.mmio_virt, REG_TDT, s_tx_next);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    if (core_trace) {
        pch_tx_memory_report(two_descriptors, s_tx_pages[slot], frame_va, &page_view);
        pch_tx_core_report("PRE-TDT", core_checkpoint);
    }
    if (trace) {
        console_set_quiet(false);
        pch_puts("[NET 2b] TX descriptor 0 PRE-TDT bytes:");
        for (unsigned n = 0; n < sizeof(posted); ++n) {
            char byte[4] = {' ', "0123456789ABCDEF"[posted[n] >> 4],
                           "0123456789ABCDEF"[posted[n] & 15], 0};
            pch_puts(byte);
        }
        pch_puts("\n[NET 2b] TX buffer physical expected: ");
        pch_hex((uint32_t)(posted_phys >> 32)); pch_hex((uint32_t)posted_phys);
        pch_puts("\n");
        pch_puts("[NET 2b] TX descriptor 0 physical-derived HHDM PRE-TDT bytes:");
        for (unsigned n = 0; n < sizeof(physical_posted); ++n) {
            char byte[4] = {' ', "0123456789ABCDEF"[physical_posted[n] >> 4],
                           "0123456789ABCDEF"[physical_posted[n] & 15], 0};
            pch_puts(byte);
        }
        pch_puts("\n[NET 2b] TX ring physical: ");
        pch_hex((uint32_t)(s_tx_phys >> 32)); pch_hex((uint32_t)s_tx_phys);
        pch_puts(" CPU descriptor VA: ");
        uintptr_t descriptor_va = (uintptr_t)&s_tx[slot];
        pch_hex((uint32_t)(descriptor_va >> 32)); pch_hex((uint32_t)descriptor_va);
        pch_puts(" physical-derived VA: ");
        pch_hex((uint32_t)(physical_view >> 32)); pch_hex((uint32_t)physical_view);
        pch_puts(descriptor_va == physical_view ? " (same CPU mapping)\n" : " (different CPU mapping)\n");
        pch_puts("[NET 2b] TX frame physical-derived HHDM PRE-TDT bytes (up to 60):\n");
        for (size_t n = 0; n < frame_view_len; ++n) {
            char byte[4] = {' ', "0123456789ABCDEF"[frame_view[n] >> 4],
                           "0123456789ABCDEF"[frame_view[n] & 15], 0};
            pch_puts(byte);
            if (n % 16 == 15 || n + 1 == frame_view_len) pch_puts("\n");
        }
        pch_puts(frame_matches ? "[NET 2b] TX frame readback MATCH submitted bytes (same CPU mapping; not a NIC visibility proof)\n" :
                 "[NET 2b] TX frame readback MISMATCH submitted bytes\n");
        pch_tx_report("PRE-TDT", tx_checkpoint);
        net_diag_report("PRE-TDT", &access_checkpoint);
    }
    bool pch = is_i219(), timer_ok = true;
    unsigned waited_ms = 0;
    /* One bounded fast stage before the existing PIT fallback. No timer
     * ownership, IRQ changes or polling under the device lock. A failed or
     * unavailable clock selects the original 100 verified 1ms intervals. */
    uint64_t fast_hz = pch ? apic_poll_clock_hz() : 0;
    uint64_t fast_start = fast_hz ? apic_poll_clock_read() : 0;
    uint64_t fast_last = fast_start;
    unsigned fast_turns = 0;
    for (unsigned i = 0; i < (pch ? 101u : NET_POLL_LIMIT);) {
        irq = spin_lock_irqsave(&g_net_dev_lock);
        bool done = (s_tx[slot].status & DESC_DD) != 0;
        bool fatal = g_net_fatal;
        if (done && !fatal) {
            s_tx_busy[slot] = false;
            --s_tx_pending;
            ++s_tx_packet_count;
        }
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        if (fatal) return -1;
        if (done) {
            if (trace && pch) {
                pch_puts("[NET 2b] TX completion PIT wait-ms="); pch_hex(waited_ms); pch_puts("\n");
            }
            return 0;
        }
        if (pch) {
            if (fast_hz) {
                uint64_t now = apic_poll_clock_read();
                if (now >= fast_last && now - fast_start < fast_hz / 50000u &&
                    ++fast_turns < 4096u) {
                    fast_last = now;
                    __asm__ volatile("pause" ::: "memory");
                    continue;
                }
                fast_hz = 0;
            }
            if (i == 100) break;
            /* Boot polling only: PIT does not depend on IF/scheduler ticks.
             * Wait outside the device lock; 100 verified 1ms intervals. */
            if (!pch_delay_ms(1)) { timer_ok = false; break; }
            ++waited_ms;
        } else __asm__ volatile("pause" ::: "memory");
        ++i;
    }
    if (is_i219()) {
        irq = spin_lock_irqsave(&g_net_dev_lock);
        /* Never read MAC MMIO if another caller has already contained it. */
        trace = !s_pch_tx_trial && !g_net_fatal && s_pch_mmio_safe;
        if (trace) { pch_tx_copy(tx_checkpoint); net_diag_copy(&access_checkpoint); }
        core_trace = s_pch_tx_trial && !g_net_fatal && s_pch_mmio_safe;
        if (core_trace) pch_tx_core_copy(core_checkpoint);
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
    }
    (void)e1000_quiesce();
    if (core_trace) pch_tx_core_report("TX TIMEOUT, captured BEFORE containment", core_checkpoint);
    if (trace) {
        pch_puts("[NET 2b] TX PIT wait-ms="); pch_hex(waited_ms);
        pch_puts(timer_ok ? " (100ms budget; diagnostic/CPU overhead excluded)\n" : " (timer failed; stopped early)\n");
        pch_tx_report("TX TIMEOUT, captured BEFORE containment", tx_checkpoint);
        net_diag_report("TX TIMEOUT, captured BEFORE containment", &access_checkpoint);
    }
    return -1;
}

pbuf_t *e1000_poll_rx(net_dev_t *dev) {
    spin_debug_assert_unheld();
    if (dev != &s_net_dev) return NULL;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    unsigned slot = s_rx_next;
    if (g_net_fatal || !(dev->flags & NET_UP) || !(s_rx[slot].status & DESC_DD)) {
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        return NULL;
    }
    dma_barrier();
    unsigned old = s_rx_slot[slot], spare = NET_RX_POOL;
    uint16_t length = s_rx[slot].length;
    bool eop = (s_rx[slot].status & RX_EOP) != 0;
    bool valid = !s_rx_discard && eop && !s_rx[slot].errors &&
                 length >= 14 && length <= 1514;
    /* Never expose the final fragment of a multi-descriptor frame. */
    s_rx_discard = !eop;
    for (unsigned i = 0; i < NET_RX_POOL; ++i)
        if (!s_rx_state[i]) { spare = i; break; }
    pbuf_t *packet = NULL;
    if (valid && spare != NET_RX_POOL) {
        packet = s_rx_packets[old];
        packet->payload = packet->data;
        packet->length = packet->total_len = length;
        packet->flags = PBUF_FLAG_DMA_BACKED | PBUF_FLAG_ALLOCATED;
        s_rx_state[old] = 2;
        s_rx_state[spare] = 1;
        s_rx_slot[slot] = spare;
        s_rx[slot].buffer_addr = s_rx_pages[spare] + offsetof(pbuf_t, data);
        ++s_rx_packet_count;
    }
    /* Invalid frames or spare exhaustion drop/rearm the original buffer. */
    s_rx[slot].length = 0;
    s_rx[slot].errors = 0;
    s_rx[slot].status = 0;
    dma_barrier();
    e1000_write32(s_e1000_dev.mmio_virt, REG_RDT, slot);
    s_rx_next = (slot + 1) % NET_RING_COUNT;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    return packet;
}

void e1000_recycle_rx(net_dev_t *dev, pbuf_t *packet) {
    spin_debug_assert_unheld();
    if (dev != &s_net_dev || !packet) return;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    /* Identity/state checks prevent foreign, ring-owned and duplicate returns. */
    for (unsigned i = 0; i < NET_RX_POOL; ++i) {
        if (s_rx_packets[i] == packet && s_rx_state[i] == 2 && !g_net_fatal) {
            pbuf_reset(packet);
            packet->flags = PBUF_FLAG_DMA_BACKED;
            s_rx_state[i] = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
}

/* Experiment before any local TX: only the existing RX ring and RDH are read.
 * No PCI/VT-d probes, new register dump, PHY write or reset is performed. */
static void pch_rx_liveness(void) {
    console_set_quiet(false);
    pch_puts("[NET 2b] RX liveness START: 30s PIT budget; peer sends 60-byte 88B5 FORTRESS-NET-2B-RX to Dell MAC\n");
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    uint32_t initial_head = e1000_read32(s_e1000_dev.mmio_virt, REG_RDH);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    unsigned completions = 0, waited = 0;
    bool match = false, timer_ok = true;
    for (unsigned ms = 0; ms <= 30000; ++ms) {
        /* Capture DD before the normal poller clears it during recycling. */
        irq = spin_lock_irqsave(&g_net_dev_lock);
        unsigned slot = s_rx_next;
        bool fatal = g_net_fatal;
        uint8_t status = fatal ? 0 : s_rx[slot].status;
        uint8_t errors = fatal ? 0 : s_rx[slot].errors;
        uint16_t length = fatal ? 0 : s_rx[slot].length;
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        if (fatal) break;
        if (status & DESC_DD) {
            ++completions;
            if (completions == 1) {
                pch_puts("[NET 2b] RX DD observed slot/status/errors/length=");
                pch_hex(slot); pch_puts("/"); pch_hex(status); pch_puts("/");
                pch_hex(errors); pch_puts("/"); pch_hex(length); pch_puts("\n");
            }
            pbuf_t *packet = e1000_poll_rx(&s_net_dev);
            if (packet) {
                const uint8_t *bytes = packet->payload;
                match = packet->length == 60 && !memcmp(bytes, s_net_dev.mac_addr, 6) &&
                        bytes[12] == 0x88 && bytes[13] == 0xb5 &&
                        !memcmp(bytes + 14, "FORTRESS-NET-2B-RX", 18);
                for (unsigned i = 32; match && i < 60; ++i) match = bytes[i] == 0xa5;
                e1000_recycle_rx(&s_net_dev, packet);
                if (match) break;
            }
        }
        if (ms == 30000) break;
        if (!pch_delay_ms(1)) { timer_ok = false; break; }
        ++waited;
    }
    irq = spin_lock_irqsave(&g_net_dev_lock);
    uint32_t final_head = g_net_fatal ? initial_head : e1000_read32(s_e1000_dev.mmio_virt, REG_RDH);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    pch_puts(match ? "[NET 2b] RX PASS: peer frame byte-checked and recycled\n" :
                    "[NET 2b] RX peer frame NOT observed; outcome inconclusive\n");
    pch_puts("[NET 2b] RX RDH start/end, DD-count, wait-ms (hex)=");
    pch_hex(initial_head); pch_puts("/"); pch_hex(final_head); pch_puts("/");
    pch_hex(completions); pch_puts("/"); pch_hex(waited); pch_puts("\n");
    if (!timer_ok) pch_puts("[NET 2b] RX timer failed; observation window incomplete\n");
    pch_puts("[NET 2b] RX-only experiment complete; no local TX attempted\n");
}
static void pch_fwsm_decode(void) {
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    uint32_t value = e1000_read32(s_e1000_dev.mmio_virt, PCH_FWSM);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    pch_puts("[NET 2b] FWSM (firmware status, not DMA clock/link control)="); pch_hex(value);
    pch_puts(" FW_VALID="); pch_hex(!!(value & 0x8000));
    pch_puts(" RSPCIPHY="); pch_hex(!!(value & 0x40));
    pch_puts(" WLOCK_MAC="); pch_hex((value & 0x380) >> 7);
    pch_puts(" ULP_CFG_DONE="); pch_hex(!!(value & 0x400));
    pch_puts(" PCIM2PCI="); pch_hex(!!(value & 0x01000000));
    pch_puts(" MODE(raw)="); pch_hex((value & 0xe) >> 1); pch_puts("\n");
}
void e1000_raw_selftest(const char *cmdline) {
    /* Match a complete whitespace-delimited token in kernel-owned metadata. */
    bool enabled = false, rx_first = false;
    for (const char *p = cmdline; p && *p;) {
        while (*p == ' ' || *p == '\t') ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t') ++end;
        if ((size_t)(end - p) == 14 && !memcmp(p, "net_test=rings", 14)) enabled = true;
        if ((size_t)(end - p) == 14 && !memcmp(p, "net_rx_first=1", 14)) rx_first = true;
        if ((size_t)(end - p) == 14 && !memcmp(p, "net_tx_trial=1", 14)) s_pch_tx_trial = true;
        p = end;
    }
    if (!enabled || !e1000_get_net_device()) return;
    if (is_i219() && rx_first) {
        pch_rx_liveness(); pch_fwsm_decode(); return;
    }
    uint8_t frame[60];
    memset(frame, 0xa5, sizeof(frame));
    memset(frame, 0xff, 6);
    memcpy(frame + 6, s_net_dev.mac_addr, 6);
    frame[12] = 0x88; frame[13] = 0xb5; /* experimental EtherType */
    memcpy(frame + 14, "FORTRESS-NET-2A-TX", 18);
    if (is_i219()) memcpy(frame + 14, "FORTRESS-NET-2B-TX", 18);
    if (is_i219() && !s_pch_tx_trial) net_vtd_prepare(); /* no locks held */
    s_tx_bringup_report = true;
    int tx_result = e1000_send_raw(&s_net_dev, frame, sizeof(frame));
    s_tx_bringup_report = false;
    if (tx_result) {
        if (is_i219()) { (void)e1000_quiesce(); return; }
        serial_puts("[NET 2a] TX FAIL; DMA quarantined\n");
        return;
    }
    if (is_i219()) {
        console_set_quiet(false);
        pch_puts("[NET 2b] TX PASS: 60-byte 88B5 frame, DD observed\n");
        pch_puts("[NET 2b] Wire NOT observed by kernel; cable-side capture required\n");
        return;
    }
    serial_puts("[NET 2a] TX PASS: 60-byte raw frame, DD observed\n");
    serial_puts("[NET 2a] RX waiting\n");
    /* Test-only bounded wait, no sleeping/IRQ dependency or held lock. */
    for (unsigned i = 0; i < 10000000u; ++i) {
        pbuf_t *packet = e1000_poll_rx(&s_net_dev);
        if (packet) {
            uint8_t expected[60];
            memcpy(expected, frame, sizeof(expected));
            memcpy(expected + 14, "FORTRESS-NET-2A-RX", 18);
            bool match = packet->length == sizeof(expected) &&
                         !memcmp(packet->data, expected, sizeof(expected));
            e1000_recycle_rx(&s_net_dev, packet);
            if (match) {
                serial_puts("[NET 2a] RX PASS: 60 bytes byte-checked; buffer recycled\n");
                return;
            }
        }
        __asm__ volatile("pause" ::: "memory");
    }
    serial_puts("[NET 2a] RX timeout\n");
}
