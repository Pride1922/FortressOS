#include "iwlwifi.h"
#include "../net/wlan/wlan_mgr.h"
#include "serial.h"
#include "vfs.h"
#include "pmm.h"
#include "string.h"

#ifndef IWLWIFI_HOST_TEST
#include "vmm.h"
#include "boot_info.h"
#endif

static iwlwifi_state_t s_iwl_dev = {
    .lock = SPINLOCK_RANKED(2, "iwlwifi_ring"),
};

static uint8_t s_tx_ring_mem[IWL_TX_RING_SIZE * sizeof(iwl_tx_desc_t)] __attribute__((aligned(4096)));
static uint8_t s_rx_ring_mem[IWL_RX_RING_SIZE * sizeof(iwl_rx_desc_t)] __attribute__((aligned(4096)));

static pbuf_t  s_rx_pbufs[IWL_RX_RING_SIZE];
static uint8_t s_rx_buffers[IWL_RX_RING_SIZE][2048] __attribute__((aligned(64)));

static inline uint32_t iwl_read32(uint32_t reg) {
    if (!s_iwl_dev.mmio_base) return 0;
    return *(volatile uint32_t *)(s_iwl_dev.mmio_base + reg);
}

static inline void iwl_write32(uint32_t reg, uint32_t val) {
    if (!s_iwl_dev.mmio_base) return;
    *(volatile uint32_t *)(s_iwl_dev.mmio_base + reg) = val;
}

bool iwlwifi_is_supported_device(uint16_t vendor, uint16_t device) {
    if (vendor != IWL_PCI_VENDOR_ID) return false;
    switch (device) {
        case IWL_DEV_ID_AC_8265:
        case IWL_DEV_ID_AC_8260:
        case IWL_DEV_ID_AC_7265:
        case IWL_DEV_ID_AC_3165:
        case IWL_DEV_ID_AC_9260:
        case IWL_DEV_ID_AX_200:
        case IWL_DEV_ID_AX_201:
        case IWL_DEV_ID_AX_210:
        case IWL_DEV_ID_AX_211:
            return true;
        default:
            return false;
    }
}

static const wlan_hw_ops_t s_iwlwifi_hw_ops = {
    .name = "iwlwifi",
    .get_mac = iwlwifi_get_mac,
    .set_channel = iwlwifi_set_channel,
    .send_raw_80211 = iwlwifi_send_raw,
    .poll_raw_80211 = iwlwifi_poll_raw,
    .recycle_raw_80211 = iwlwifi_recycle_raw,
};

bool iwlwifi_probe(void) {
#ifndef IWLWIFI_HOST_TEST
    pci_device_t pci_devs[MAX_PCI_DEVICES];
    size_t count = pci_scan_all(pci_devs, MAX_PCI_DEVICES);
    for (size_t i = 0; i < count; i++) {
        if (iwlwifi_is_supported_device(pci_devs[i].vendor_id, pci_devs[i].device_id)) {
            s_iwl_dev.pci = pci_devs[i];
            s_iwl_dev.detected = true;
            serial_puts("[IWL] Found Intel Wireless device: ");
            serial_print_hex(pci_devs[i].vendor_id);
            serial_puts(":");
            serial_print_hex(pci_devs[i].device_id);
            serial_puts("\n");
            return true;
        }
    }
    return false;
#else
    return s_iwl_dev.detected;
#endif
}

bool iwlwifi_load_firmware(const void *fw_data, size_t fw_len) {
    if (!fw_data || fw_len < sizeof(iwl_ucode_header_t)) return false;
    const iwl_ucode_header_t *hdr = (const iwl_ucode_header_t *)fw_data;
    if (hdr->magic != IWL_UCODE_TLV_MAGIC) return false;

    const uint8_t *ptr = (const uint8_t *)fw_data + sizeof(iwl_ucode_header_t);
    size_t rem = fw_len - sizeof(iwl_ucode_header_t);

    uint32_t tlv_count = 0;
    while (rem >= sizeof(iwl_ucode_tlv_t)) {
        const iwl_ucode_tlv_t *tlv = (const iwl_ucode_tlv_t *)ptr;
        if (tlv->length > rem - sizeof(iwl_ucode_tlv_t)) break;
        tlv_count++;
        size_t step = sizeof(iwl_ucode_tlv_t) + ((tlv->length + 3) & ~3);
        if (step > rem) break;
        ptr += step;
        rem -= step;
    }
    return tlv_count > 0;
}

bool iwlwifi_map_mmio(void) {
#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.detected) return false;
    uint64_t bar0 = s_iwl_dev.pci.bar[0];
    if (bar0 == 0) return false;

    /* Enable PCI bus master and memory space */
    uint16_t cmd = pci_read_config16(s_iwl_dev.pci.segment, s_iwl_dev.pci.bus,
                                     s_iwl_dev.pci.device, s_iwl_dev.pci.function, PCI_REG_COMMAND);
    cmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    pci_write_config16(s_iwl_dev.pci.segment, s_iwl_dev.pci.bus,
                       s_iwl_dev.pci.device, s_iwl_dev.pci.function, PCI_REG_COMMAND, cmd);

    /* Map BAR0 MMIO (16 KiB window) into master kernel PML4 at boot time */
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    for (uint32_t offset = 0; offset < 0x4000; offset += 4096) {
        uintptr_t va = IWLWIFI_MMIO_VIRT + offset;
        uintptr_t pa = bar0 + offset;
        vmm_map_page(pml4, va, pa, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
    }
    s_iwl_dev.mmio_base = IWLWIFI_MMIO_VIRT;
    return true;
#else
    s_iwl_dev.mmio_base = (uintptr_t)&s_iwl_dev;
    return true;
#endif
}

bool iwlwifi_init_hw(void) {
    if (!s_iwl_dev.detected) return false;
    if (s_iwl_dev.initialized) return true;

    /* Setup DMA Descriptor Rings */
    memset(s_tx_ring_mem, 0, sizeof(s_tx_ring_mem));
    memset(s_rx_ring_mem, 0, sizeof(s_rx_ring_mem));
    s_iwl_dev.tx_head = 0;
    s_iwl_dev.tx_tail = 0;
    s_iwl_dev.rx_head = 0;
    s_iwl_dev.rx_tail = 0;

    for (uint32_t i = 0; i < IWL_RX_RING_SIZE; i++) {
        pbuf_init(&s_rx_pbufs[i]);
        s_rx_pbufs[i].payload = s_rx_buffers[i];
    }

    /* Intel OUI default fallback: 00:24:FD (Intel Corp) */
    s_iwl_dev.mac_addr[0] = 0x00;
    s_iwl_dev.mac_addr[1] = 0x24;
    s_iwl_dev.mac_addr[2] = 0xFD;
    s_iwl_dev.mac_addr[3] = 0x82;
    s_iwl_dev.mac_addr[4] = 0x65;
    s_iwl_dev.mac_addr[5] = 0x01;

    s_iwl_dev.current_channel = 1;
    s_iwl_dev.initialized = true;

    /* Register with Wi-Fi manager without holding any locks */
    wlan_mgr_init(&s_iwlwifi_hw_ops);
    return true;
}

int iwlwifi_send_raw(const void *buf, size_t len) {
    if (!buf || len == 0 || len > 2048) return -1;
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive) return -1;
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);

    /* Post into Tx ring */
    uint32_t next = (s_iwl_dev.tx_head + 1) % IWL_TX_RING_SIZE;
    if (next == s_iwl_dev.tx_tail) {
        spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
        return -1; /* Ring full */
    }

    iwl_tx_desc_t *desc = (iwl_tx_desc_t *)s_tx_ring_mem + s_iwl_dev.tx_head;
    desc->buf_low = (uint32_t)(uintptr_t)buf;
    desc->buf_high = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
    desc->len = (uint16_t)len;
    desc->flags = 1; /* Valid */

    s_iwl_dev.tx_head = next;

    /* Ring doorbell */
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(0), s_iwl_dev.tx_head);

    spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
    return 0;
}

pbuf_t *iwlwifi_poll_raw(void) {
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive) return NULL;
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);
    if (s_iwl_dev.rx_head == s_iwl_dev.rx_tail) {
        spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
        return NULL;
    }

    pbuf_t *p = &s_rx_pbufs[s_iwl_dev.rx_tail];
    s_iwl_dev.rx_tail = (s_iwl_dev.rx_tail + 1) % IWL_RX_RING_SIZE;
    spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
    return p;
}

void iwlwifi_recycle_raw(pbuf_t *p) {
    (void)p;
}

int iwlwifi_set_channel(uint8_t channel) {
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);
    s_iwl_dev.current_channel = channel;
    spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
    return 0;
}

int iwlwifi_get_mac(uint8_t mac[6]) {
    if (!mac) return -1;
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);
    memcpy(mac, s_iwl_dev.mac_addr, 6);
    spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
    return 0;
}

bool iwlwifi_is_detected(void) {
    return s_iwl_dev.detected;
}
