#include "e1000.h"
#include "serial.h"
#include "vmm.h"
#include "string.h"
#include "pmm.h"
#include "spinlock.h"
#include <stddef.h>

static bool e1000_init_rings(void);

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
    /* Phase 2a only enables DMA on the two QEMU models. Dell stays discovery-only. */
    if (s_e1000_dev.pci.device_id == E1000_DEV_82540EM ||
        s_e1000_dev.pci.device_id == E1000_DEV_82574L)
        return e1000_init_rings();
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

bool e1000_quiesce(void) {
    spin_debug_assert_unheld();
    if (!s_e1000_found) return false;
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
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
    /* 82574: EXSTEN=0 selects the legacy RX descriptor format. */
    e1000_write32(base, REG_RFCTL, 0);
    /* Full duplex: CT=15, COLD=64; copper IPG 10/8/6. */
    e1000_write32(base, REG_TIPG, 10u | (8u << 10) | (6u << 20));
    e1000_write32(base, E1000_REG_TCTL, (1u << 1) | (1u << 3) |
                  (15u << 4) | (64u << 12));
    /* EN, BAM, 2048-byte buffers, strip CRC; no promiscuous reception. */
    e1000_write32(base, E1000_REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    dma_barrier();
    (void)e1000_read32(base, E1000_REG_STATUS);
    net_command(net_command_read() | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE);
    if (!(net_command_read() & PCI_COMMAND_BUS_MASTER)) goto failed;
    memcpy(s_net_dev.name, "eth0", 5);
    memcpy(s_net_dev.mac_addr, s_e1000_dev.mac_addr, 6);
    s_net_dev.mtu = 1500;
    s_net_dev.send_packet = e1000_send_raw;
    s_net_dev.poll_rx = e1000_poll_rx;
    s_net_dev.recycle_rx = e1000_recycle_rx;
    s_net_dev.priv = &s_e1000_dev;
    s_net_dev.flags = NET_UP | (s_e1000_dev.link_up ? NET_RUNNING : 0);
    serial_puts("[NET 2a] eth0: TX/RX 64x16, RX pool 80, DMA ready (polling only)\n");
    return true;
failed:
    (void)e1000_quiesce();
    serial_puts("[NET 2a] Bring-up failed; DMA pages retained, networking unavailable\n");
    return false;
}

net_dev_t *e1000_get_net_device(void) {
    spin_debug_assert_unheld();
    uint64_t irq = spin_lock_irqsave(&g_net_dev_lock);
    net_dev_t *dev = s_net_dev.flags ? &s_net_dev : NULL;
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    return dev;
}

int e1000_send_raw(net_dev_t *dev, const void *buf, size_t len) {
    spin_debug_assert_unheld();
    if (dev != &s_net_dev || !buf || len < 14 || len > 1514) return -1;
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
    s_tx_next = (slot + 1) % NET_RING_COUNT;
    dma_barrier();
    e1000_write32(s_e1000_dev.mmio_virt, REG_TDT, s_tx_next);
    spin_unlock_irqrestore(&g_net_dev_lock, irq);
    for (unsigned i = 0; i < NET_POLL_LIMIT; ++i) {
        irq = spin_lock_irqsave(&g_net_dev_lock);
        bool done = (s_tx[slot].status & DESC_DD) != 0;
        bool fatal = g_net_fatal;
        if (done && !fatal) {
            s_tx_busy[slot] = false;
            --s_tx_pending;
        }
        spin_unlock_irqrestore(&g_net_dev_lock, irq);
        if (fatal) return -1;
        if (done) return 0;
        __asm__ volatile("pause" ::: "memory");
    }
    (void)e1000_quiesce();
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

void e1000_raw_selftest(const char *cmdline) {
    /* Match a complete whitespace-delimited token in kernel-owned metadata. */
    bool enabled = false;
    for (const char *p = cmdline; p && *p;) {
        while (*p == ' ' || *p == '\t') ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t') ++end;
        if ((size_t)(end - p) == 14 && !memcmp(p, "net_test=rings", 14)) enabled = true;
        p = end;
    }
    if (!enabled || !e1000_get_net_device()) return;
    uint8_t frame[60];
    memset(frame, 0xa5, sizeof(frame));
    memset(frame, 0xff, 6);
    memcpy(frame + 6, s_net_dev.mac_addr, 6);
    frame[12] = 0x88; frame[13] = 0xb5; /* experimental EtherType */
    memcpy(frame + 14, "FORTRESS-NET-2A-TX", 18);
    if (e1000_send_raw(&s_net_dev, frame, sizeof(frame))) {
        serial_puts("[NET 2a] TX FAIL; DMA quarantined\n");
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
