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

#ifndef IWLWIFI_HOST_TEST
static uintptr_t s_fw_chunk_phys = 0;
static uint8_t  *s_fw_chunk_virt = NULL;
#endif

static inline uint32_t iwl_read32(uint32_t reg) {
    if (!s_iwl_dev.mmio_base) return 0;
    return *(volatile uint32_t *)(s_iwl_dev.mmio_base + reg);
}

static inline void iwl_write32(uint32_t reg, uint32_t val) {
    if (!s_iwl_dev.mmio_base) return;
    *(volatile uint32_t *)(s_iwl_dev.mmio_base + reg) = val;
}

#ifndef IWLWIFI_HOST_TEST
static void iwl_write_prph(uint32_t addr, uint32_t val) {
    iwl_write32(HBUS_TARG_PRPH_WADDR, ((addr & 0x000FFFFF) | (3 << 24)));
    iwl_write32(HBUS_TARG_PRPH_WDAT, val);
}

static uint32_t __attribute__((unused)) iwl_read_prph(uint32_t addr) {
    iwl_write32(HBUS_TARG_PRPH_RADDR, ((addr & 0x000FFFFF) | (3 << 24)));
    return iwl_read32(HBUS_TARG_PRPH_RDAT);
}

static bool iwl_nic_lock(void) {
    if (!s_iwl_dev.mmio_base) return false;
    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) | CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    for (int i = 0; i < 50000; i++) {
        uint32_t val = iwl_read32(CSR_GP_CNTRL);
        if ((val & CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) && !(val & CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP)) {
            return true;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

static void iwl_nic_unlock(void) {
    if (!s_iwl_dev.mmio_base) return;
    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) & ~CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

static bool iwl_load_chunk(uint32_t dst_addr, const uint8_t *chunk, uint32_t len) {
    if (!s_fw_chunk_phys || !s_fw_chunk_virt) return false;
    if (len > 4096) len = 4096;
    memcpy(s_fw_chunk_virt, chunk, len);

    if (!iwl_nic_lock()) return false;

    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_FH_SRVC_CHNL), 0);
    iwl_write32(FH_SRVC_CHNL_SRAM_ADDR_REG(IWL_FH_SRVC_CHNL), dst_addr);
    iwl_write32(FH_TFDIB_CTRL0_REG(IWL_FH_SRVC_CHNL), (uint32_t)(s_fw_chunk_phys & 0xFFFFFFFF));
    iwl_write32(FH_TFDIB_CTRL1_REG(IWL_FH_SRVC_CHNL), (((uint32_t)(s_fw_chunk_phys >> 32)) << 24) | len);
    iwl_write32(FH_TCSR_CHNL_TX_BUF_STS_REG(IWL_FH_SRVC_CHNL), (1 << 20) | (1 << 12) | 3);
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_FH_SRVC_CHNL),
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);

    iwl_nic_unlock();

    /* Poll for completion (CSR_INT_BIT_FH_TX) */
    for (int i = 0; i < 100000; i++) {
        uint32_t intr = iwl_read32(CSR_INT);
        if (intr & CSR_INT_BIT_FH_TX) {
            iwl_write32(CSR_FH_INT_STATUS, 0xFFFFFFFF);
            iwl_write32(CSR_INT, CSR_INT_BIT_FH_TX);
            return true;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

static bool iwl_load_section(uint32_t dst_addr, const uint8_t *data, uint32_t byte_cnt) {
    uint32_t offset = 0;
    while (offset < byte_cnt) {
        uint32_t chunk_len = byte_cnt - offset;
        if (chunk_len > 4096) chunk_len = 4096;
        if (!iwl_load_chunk(dst_addr + offset, data + offset, chunk_len)) {
            return false;
        }
        offset += chunk_len;
    }
    return true;
}
#endif

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
    if (tlv_count == 0) return false;

#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.detected || !s_iwl_dev.mmio_base) return true;

    /* Allocate DMA chunk buffer if not yet initialized */
    if (!s_fw_chunk_phys) {
        s_fw_chunk_phys = pmm_alloc_page();
        if (s_fw_chunk_phys) {
            s_fw_chunk_virt = (uint8_t *)vmm_phys_to_virt(s_fw_chunk_phys);
        }
    }
    if (!s_fw_chunk_phys || !s_fw_chunk_virt) {
        serial_puts("[IWL] PMM allocation for FW DMA failed\n");
        return false;
    }

    /* 1. Power up and activate device clock */
    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) | CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    for (int i = 0; i < 50000; i++) {
        if (iwl_read32(CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) break;
        __asm__ volatile("pause" ::: "memory");
    }

    /* Clear RFKILL and legacy interrupts */
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, 0x06);
    iwl_write32(CSR_INT, 0xFFFFFFFF);

    /* 2. Release internal CPU from reset */
    if (iwl_nic_lock()) {
        iwl_write_prph(IWL_RELEASE_CPU_RESET, IWL_RELEASE_CPU_RESET_BIT);
        iwl_nic_unlock();
    }

    /* 3. Stream SEC_INIT (TLV 20) CPU1 and CPU2 sections */
    ptr = (const uint8_t *)fw_data + sizeof(iwl_ucode_header_t);
    rem = fw_len - sizeof(iwl_ucode_header_t);

    int cpu_phase = 1;
    uint32_t sec_num = 1;

    while (rem >= sizeof(iwl_ucode_tlv_t)) {
        const iwl_ucode_tlv_t *tlv = (const iwl_ucode_tlv_t *)ptr;
        size_t step = sizeof(iwl_ucode_tlv_t) + ((tlv->length + 3) & ~3);
        if (step > rem) break;

        if (tlv->type == IWL_UCODE_TLV_SEC_INIT && tlv->length >= 4) {
            const uint8_t *payload = ptr + sizeof(iwl_ucode_tlv_t);
            uint32_t dst_addr = *(const uint32_t *)payload;
            const uint8_t *data = payload + 4;
            uint32_t dlen = tlv->length - 4;

            if (dst_addr == IWL_CPU1_CPU2_SEPARATOR_SECTION) {
                /* Finalize CPU1 */
                if (iwl_nic_lock()) {
                    iwl_write32(FH_UCODE_LOAD_STATUS, 0x0000FFFF);
                    iwl_nic_unlock();
                }
                cpu_phase = 2;
                sec_num = 1;
            } else if (dst_addr == IWL_PAGING_SEPARATOR_SECTION) {
                /* End of non-paged CPU2 sections */
                break;
            } else if (dlen > 0) {
                if (!iwl_load_section(dst_addr, data, dlen)) {
                    serial_puts("[IWL] Failed to stream firmware chunk\n");
                    return false;
                }
                if (iwl_nic_lock()) {
                    uint32_t shift = (cpu_phase == 1) ? 0 : 16;
                    uint32_t val = iwl_read32(FH_UCODE_LOAD_STATUS);
                    val |= (sec_num << shift);
                    iwl_write32(FH_UCODE_LOAD_STATUS, val);
                    sec_num = (sec_num << 1) | 1;
                    iwl_nic_unlock();
                }
            }
        }

        ptr += step;
        rem -= step;
    }

    /* Finalize CPU2 */
    if (iwl_nic_lock()) {
        iwl_write32(FH_UCODE_LOAD_STATUS, 0xFFFFFFFF);
        iwl_nic_unlock();
    }

    /* 4. Release processor from reset */
    iwl_write32(CSR_RESET, 0);

    /* 5. Wait for ALIVE */
    bool alive = false;
    for (int i = 0; i < 100000; i++) {
        uint32_t intr = iwl_read32(CSR_INT);
        if (intr & CSR_INT_BIT_ALIVE) {
            alive = true;
            iwl_write32(CSR_INT, CSR_INT_BIT_ALIVE);
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }

    if (alive) {
        s_iwl_dev.firmware_alive = true;
        serial_puts("[IWL] Firmware ALIVE signaled! Radio operational.\n");
    } else {
        /* Fail closed: without ALIVE the TX path must stay disabled so no
         * descriptors are posted to an unbooted DMA engine. */
        s_iwl_dev.firmware_alive = false;
        serial_puts("[IWL] Firmware ALIVE timeout; radio remains disabled.\n");
        return false;
    }
#else
    s_iwl_dev.firmware_alive = true;
#endif

    return true;
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
