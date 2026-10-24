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
#define IWL_HW_RX_BUFS          32
#define IWL_CMD_SLOTS           16
#define IWL_CMD_SLOT_SIZE       4096
#define IWL_MAX_PHY_DB_CHUNKS   24
#define IWL_PHY_DB_POOL_SIZE    32768

static uintptr_t         s_fw_chunk_phys = 0;
static uint8_t          *s_fw_chunk_virt = NULL;
static uintptr_t         s_kw_phys       = 0;
static uintptr_t         s_rx_desc_phys  = 0;
static uint32_t         *s_rx_desc_virt  = NULL;
static uintptr_t         s_rx_buf_phys[IWL_HW_RX_BUFS];
static uint8_t          *s_rx_buf_virt[IWL_HW_RX_BUFS];
static uint16_t          s_hw_rx_read_idx = 0;

/* TX Scheduler, Command Queue 0, AUX Queue 1, and Data/Mgmt Queue 5 DMA Buffers */
static uintptr_t         s_sched_phys          = 0;
static iwl_scd_bc_tbl_t *s_sched_virt          = NULL;
static uintptr_t         s_tfd_ring_phys       = 0;
static iwl_tfd_t        *s_tfd_ring_virt       = NULL;
static uintptr_t         s_aux_tfd_ring_phys   = 0;
static iwl_tfd_t        *s_aux_tfd_ring_virt   = NULL;
static uintptr_t         s_txq5_tfd_ring_phys  = 0;
static iwl_tfd_t        *s_txq5_tfd_ring_virt  = NULL;
static uintptr_t         s_cmd_buf_phys        = 0;
static uint8_t          *s_cmd_buf_virt        = NULL;
static uintptr_t         s_txq5_buf_phys       = 0;
static uint8_t          *s_txq5_buf_virt       = NULL;

/* CPU2 Firmware Paging DMA Buffers (1 CSS 4KB + up to 8x 32KB blocks) */
static uintptr_t         s_paging_css_phys    = 0;
static uint8_t          *s_paging_css_virt    = NULL;
static uintptr_t         s_paging_blk_phys[IWL_MAX_PAGING_DATA_BLOCKS];
static uint8_t          *s_paging_blk_virt[IWL_MAX_PAGING_DATA_BLOCKS];
static uint32_t          s_num_paging_blk     = 0;
static uint32_t          s_last_blk_pages     = 0;

/* Calibration PHY_DB Storage (populated by SEC_INIT 0x6B, replayed to SEC_RT 0x6C) */
typedef struct {
    uint16_t type;
    uint16_t length;
    uint32_t offset;
} iwl_phy_db_chunk_t;

static iwl_phy_db_chunk_t s_phy_db_chunks[IWL_MAX_PHY_DB_CHUNKS];
static uint32_t           s_phy_db_count = 0;
static uint8_t            s_phy_db_pool[IWL_PHY_DB_POOL_SIZE];
static uint32_t           s_phy_db_pool_used = 0;

/* Runtime MVM State */
static uint32_t           s_sched_base = 0;
static uint32_t           s_tlv_flags  = IWL_UCODE_TLV_FLAGS_DW_BC_TABLE;
static uint32_t           s_n_scan_channels = 40;
static uint8_t            s_valid_tx_ant = 3;
static uint8_t            s_valid_rx_ant = 3;
static uint8_t            s_cmd_idx = 0;
static uint8_t            s_txq5_idx = 0;
static uint8_t            s_pending_cmd_idx = 0xFF;
static bool               s_cmd_done = false;
static bool               s_cmd_failed = false;
static uint8_t            s_cmd_resp_buf[64];
static bool               s_alive_received = false;
static uint16_t           s_alive_status = 0;
static bool               s_init_complete_received = false;
static bool               s_scan_complete = false;
static bool               s_te_started = false;
static bool               s_mac_active = false;
static bool               s_binding_active = false;
static bool               s_sta_active = false;
static bool               s_is_associated = false;
static uint8_t            s_assoc_bssid[6];
static uint16_t           s_assoc_bi = 100;
static uint16_t           s_assoc_aid = 0;
static uint32_t           s_last_err_intr = 0;
static uint16_t           s_last_rx_wide_id = 0;
static uint8_t            s_phy_db_scratch[4096];
static uint8_t            s_scan_cmd_buf[2048];

/* Last RX PHY Info (0xC0) */
static iwl_rx_phy_info_t  s_last_phy_info;
static int8_t             s_last_rx_rssi = -60;
static uint8_t            s_last_rx_chan = 1;

/* Scan channels: 2.4 GHz (1..13) + 5 GHz UNII-1 (36..48) */
static const uint8_t s_scan_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48
};
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
static bool iwl_delay_ms(unsigned ms) {
    for (unsigned m = 0; m < ms; m++) {
        uint16_t ticks = 1193u; /* ~1 ms at 1.193182 MHz PIT */
        uint8_t saved = inb(0x61);
        outb(0x61, saved & ~3u);
        outb(0x43, 0xb0);
        outb(0x42, (uint8_t)ticks);
        outb(0x42, (uint8_t)(ticks >> 8));
        outb(0x61, (saved & ~3u) | 1u);
        bool done = false;
        for (unsigned i = 0; i < 1000000u; i++) {
            if (inb(0x61) & 0x20u) {
                done = true;
                break;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        outb(0x61, saved);
        if (!done) return false;
    }
    return true;
}

static void iwl_write_prph(uint32_t addr, uint32_t val) {
    iwl_write32(HBUS_TARG_PRPH_WADDR, ((addr & 0x000FFFFFu) | (3u << 24)));
    iwl_write32(HBUS_TARG_PRPH_WDAT, val);
}

static uint32_t iwl_read_prph(uint32_t addr) {
    iwl_write32(HBUS_TARG_PRPH_RADDR, ((addr & 0x000FFFFFu) | (3u << 24)));
    return iwl_read32(HBUS_TARG_PRPH_RDAT);
}

static void iwl_write_mem32(uint32_t addr, uint32_t val) {
    iwl_write32(HBUS_TARG_MEM_WADDR, addr);
    iwl_write32(HBUS_TARG_MEM_WDAT, val);
}

static bool iwl_nic_lock(void) {
    if (!s_iwl_dev.mmio_base) return false;
    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) | CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    for (int k = 0; k < 200; k++) {
        __asm__ volatile("pause" ::: "memory");
    }
    for (int i = 0; i < 200; i++) {
        for (int j = 0; j < 500; j++) {
            uint32_t val = iwl_read32(CSR_GP_CNTRL);
            if ((val & CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) && !(val & CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP)) {
                return true;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        iwl_delay_ms(1);
    }
    return false;
}

static void iwl_nic_unlock(void) {
    if (!s_iwl_dev.mmio_base) return;
    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) & ~CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

static uint32_t iwl_fh_cbbc_queue_reg(uint32_t chnl) {
    if (chnl < 16) return 0x19D0u + 4u * chnl;
    if (chnl < 20) return 0x1BF0u + 4u * (chnl - 16u);
    return 0x1B20u + 4u * (chnl - 20u);
}

static uint32_t iwl_scd_queue_rdptr_reg(uint32_t chnl) {
    if (chnl < 20) return IWL_SCD_BASE + 0x68u + chnl * 4u;
    return IWL_SCD_BASE + 0x2B4u + chnl * 4u;
}

static uint32_t iwl_scd_queue_status_bits_reg(uint32_t chnl) {
    if (chnl < 20) return IWL_SCD_BASE + 0x10Cu + chnl * 4u;
    return IWL_SCD_BASE + 0x334u + chnl * 4u;
}

static void iwl_enable_pci_and_bridges(void) {
    uint16_t seg = s_iwl_dev.pci.segment;
    uint8_t target_bus = s_iwl_dev.pci.bus;

    /* Ensure any parent PCI-to-PCI bridge decoding target_bus has Bus Master & Mem Space enabled */
    if (target_bus > 0) {
        for (uint16_t bus = 0; bus < target_bus; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                uint16_t vid = pci_read_config16(seg, (uint8_t)bus, dev, 0, PCI_REG_VENDOR_ID);
                if (vid == 0xFFFF || vid == 0x0000) continue;
                uint8_t hdr0 = pci_read_config8(seg, (uint8_t)bus, dev, 0, PCI_REG_HEADER_TYPE);
                uint8_t max_fn = (hdr0 & PCI_HEADER_TYPE_MULTIFN) ? 8 : 1;
                for (uint8_t fn = 0; fn < max_fn; fn++) {
                    if (fn > 0) {
                        uint16_t fvid = pci_read_config16(seg, (uint8_t)bus, dev, fn, PCI_REG_VENDOR_ID);
                        if (fvid == 0xFFFF || fvid == 0x0000) continue;
                    }
                    uint8_t hdr = pci_read_config8(seg, (uint8_t)bus, dev, fn, PCI_REG_HEADER_TYPE);
                    if ((hdr & 0x7Fu) != PCI_HEADER_TYPE_BRIDGE) continue;
                    uint32_t buses = pci_read_config32(seg, (uint8_t)bus, dev, fn, 0x18);
                    uint8_t sec_bus = (uint8_t)((buses >> 8) & 0xFFu);
                    uint8_t sub_bus = (uint8_t)((buses >> 16) & 0xFFu);
                    if (sec_bus > 0 && target_bus >= sec_bus && target_bus <= sub_bus) {
                        uint16_t bcmd = pci_read_config16(seg, (uint8_t)bus, dev, fn, PCI_REG_COMMAND);
                        if ((bcmd & (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)) !=
                            (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)) {
                            bcmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
                            pci_write_config16(seg, (uint8_t)bus, dev, fn, PCI_REG_COMMAND, bcmd);
                        }
                        /* Clear sticky primary and secondary status error bits on bridge */
                        pci_write_config16(seg, (uint8_t)bus, dev, fn, PCI_REG_STATUS, 0xF900u);
                        pci_write_config16(seg, (uint8_t)bus, dev, fn, 0x1E, 0xF900u);
                    }
                }
            }
        }
    }

    /* Disable PCI RETRY_TIMEOUT register (0x41) per OpenBSD iwm / Linux iwlwifi */
    uint32_t reg40 = pci_read_config32(seg, s_iwl_dev.pci.bus, s_iwl_dev.pci.device, s_iwl_dev.pci.function, 0x40);
    if (reg40 & 0x0000FF00u) {
        pci_write_config32(seg, s_iwl_dev.pci.bus, s_iwl_dev.pci.device, s_iwl_dev.pci.function, 0x40, reg40 & ~0x0000FF00u);
    }

    /* Enable Memory Space & Bus Master and clear PCI_COMMAND_INT_DISABLE (hardware bug workaround) */
    uint16_t cmd = pci_read_config16(seg, s_iwl_dev.pci.bus, s_iwl_dev.pci.device, s_iwl_dev.pci.function, PCI_REG_COMMAND);
    cmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    cmd &= (uint16_t)~PCI_COMMAND_INT_DISABLE;
    pci_write_config16(seg, s_iwl_dev.pci.bus, s_iwl_dev.pci.device, s_iwl_dev.pci.function, PCI_REG_COMMAND, cmd);

    /* Clear sticky PCI status error bits (Master Abort, Target Abort, Parity) */
    pci_write_config16(seg, s_iwl_dev.pci.bus, s_iwl_dev.pci.device, s_iwl_dev.pci.function, PCI_REG_STATUS, 0xF900u);
}

static bool iwl_alloc_dma_buffers(void) {
    if (!s_fw_chunk_phys) {
        s_fw_chunk_phys = pmm_alloc_page();
        if (s_fw_chunk_phys) {
            s_fw_chunk_virt = (uint8_t *)vmm_phys_to_virt(s_fw_chunk_phys);
            memset(s_fw_chunk_virt, 0, 4096);
        }
    }
    if (!s_kw_phys) {
        s_kw_phys = pmm_alloc_page();
        if (s_kw_phys) {
            memset(vmm_phys_to_virt(s_kw_phys), 0, 4096);
        }
    }
    if (!s_rx_desc_phys) {
        s_rx_desc_phys = pmm_alloc_page();
        if (s_rx_desc_phys) {
            s_rx_desc_virt = (uint32_t *)vmm_phys_to_virt(s_rx_desc_phys);
            memset(s_rx_desc_virt, 0, 4096);
        }
    }
    if (!s_sched_phys) {
        s_sched_phys = pmm_alloc_pages(5); /* 20 KiB for 31 * 640B Byte Count tables */
        if (s_sched_phys) {
            s_sched_virt = (iwl_scd_bc_tbl_t *)vmm_phys_to_virt(s_sched_phys);
            memset(s_sched_virt, 0, 5 * 4096);
        }
    }
    if (!s_tfd_ring_phys) {
        s_tfd_ring_phys = pmm_alloc_pages(8); /* 32 KiB for 256 * 128B TFDs (Queue 0) */
        if (s_tfd_ring_phys) {
            s_tfd_ring_virt = (iwl_tfd_t *)vmm_phys_to_virt(s_tfd_ring_phys);
            memset(s_tfd_ring_virt, 0, 8 * 4096);
        }
    }
    if (!s_aux_tfd_ring_phys) {
        s_aux_tfd_ring_phys = pmm_alloc_pages(8); /* 32 KiB for 256 * 128B TFDs (Queue 1) */
        if (s_aux_tfd_ring_phys) {
            s_aux_tfd_ring_virt = (iwl_tfd_t *)vmm_phys_to_virt(s_aux_tfd_ring_phys);
            memset(s_aux_tfd_ring_virt, 0, 8 * 4096);
        }
    }
    if (!s_txq5_tfd_ring_phys) {
        s_txq5_tfd_ring_phys = pmm_alloc_pages(8); /* 32 KiB for 256 * 128B TFDs (Queue 5) */
        if (s_txq5_tfd_ring_phys) {
            s_txq5_tfd_ring_virt = (iwl_tfd_t *)vmm_phys_to_virt(s_txq5_tfd_ring_phys);
            memset(s_txq5_tfd_ring_virt, 0, 8 * 4096);
        }
    }
    if (!s_cmd_buf_phys) {
        s_cmd_buf_phys = pmm_alloc_pages(16); /* 64 KiB: 16 slots * 4096B contiguous command buffers */
        if (s_cmd_buf_phys) {
            s_cmd_buf_virt = (uint8_t *)vmm_phys_to_virt(s_cmd_buf_phys);
            memset(s_cmd_buf_virt, 0, 16 * 4096);
        }
    }
    if (!s_txq5_buf_phys) {
        s_txq5_buf_phys = pmm_alloc_pages(16); /* 64 KiB: 32 slots * 2048B TX frame buffers */
        if (s_txq5_buf_phys) {
            s_txq5_buf_virt = (uint8_t *)vmm_phys_to_virt(s_txq5_buf_phys);
            memset(s_txq5_buf_virt, 0, 16 * 4096);
        }
    }
    if (!s_paging_css_phys) {
        s_paging_css_phys = pmm_alloc_page();
        if (s_paging_css_phys) {
            s_paging_css_virt = (uint8_t *)vmm_phys_to_virt(s_paging_css_phys);
            memset(s_paging_css_virt, 0, 4096);
        }
    }
    for (uint32_t i = 0; i < IWL_MAX_PAGING_DATA_BLOCKS; i++) {
        if (!s_paging_blk_phys[i]) {
            s_paging_blk_phys[i] = pmm_alloc_pages(8); /* 32 KiB per paging block */
            if (!s_paging_blk_phys[i]) return false;
            s_paging_blk_virt[i] = (uint8_t *)vmm_phys_to_virt(s_paging_blk_phys[i]);
            memset(s_paging_blk_virt[i], 0, 8 * 4096);
        }
    }

    if (!s_fw_chunk_phys || !s_kw_phys || !s_rx_desc_phys ||
        !s_sched_phys || !s_tfd_ring_phys || !s_aux_tfd_ring_phys ||
        !s_txq5_tfd_ring_phys || !s_cmd_buf_phys || !s_txq5_buf_phys ||
        !s_paging_css_phys) {
        return false;
    }

    for (uint32_t i = 0; i < IWL_HW_RX_BUFS; i++) {
        if (!s_rx_buf_phys[i]) {
            s_rx_buf_phys[i] = pmm_alloc_page();
            if (!s_rx_buf_phys[i]) return false;
            s_rx_buf_virt[i] = (uint8_t *)vmm_phys_to_virt(s_rx_buf_phys[i]);
            memset(s_rx_buf_virt[i], 0, 4096);
        }
    }
    return true;
}

static bool iwl_prepare_card_hw(void) {
    for (int tries = 0; tries < 5; tries++) {
        iwl_write32(CSR_HW_IF_CONFIG_REG, iwl_read32(CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
        for (int i = 0; i < 50; i++) {
            if (iwl_read32(CSR_HW_IF_CONFIG_REG) & CSR_HW_IF_CONFIG_REG_BIT_NIC_READY) {
                iwl_write32(CSR_MBOX_SET_REG, iwl_read32(CSR_MBOX_SET_REG) | CSR_MBOX_SET_REG_OS_ALIVE);
                return true;
            }
            iwl_delay_ms(1);
        }
        iwl_write32(CSR_HW_IF_CONFIG_REG, iwl_read32(CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_CONFIG_REG_PREPARE);
        iwl_delay_ms(25);
    }
    return false;
}

static bool iwl_apm_init(void) {
    iwl_write32(CSR_GIO_CHICKEN_BITS, iwl_read32(CSR_GIO_CHICKEN_BITS) | CSR_GIO_CHICKEN_BITS_L1A_NO_L0S_RX);
    iwl_write32(CSR_DBG_HPET_MEM_REG, iwl_read32(CSR_DBG_HPET_MEM_REG) | CSR_DBG_HPET_MEM_REG_VAL);
    iwl_write32(CSR_HW_IF_CONFIG_REG, iwl_read32(CSR_HW_IF_CONFIG_REG) | CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
    iwl_write32(CSR_GIO_REG, iwl_read32(CSR_GIO_REG) | CSR_GIO_REG_VAL_L0S_DISABLED);

    iwl_write32(CSR_GP_CNTRL, iwl_read32(CSR_GP_CNTRL) | CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
    bool clock_ready = false;
    for (int i = 0; i < 200; i++) {
        if (iwl_read32(CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) {
            clock_ready = true;
            break;
        }
        iwl_delay_ms(1);
    }
    if (!clock_ready) return false;

    /* Enable WFPM peripheral access on Family 8000 and wait for BISR shift */
    if (iwl_nic_lock()) {
        iwl_write_prph(IWL_WFPM_CTRL_REG, iwl_read_prph(IWL_WFPM_CTRL_REG) | IWL_ENABLE_WFPM);
        iwl_write_prph(IWL_WFPM_GP2, 0x01010101u);
        iwl_nic_unlock();
    }
    iwl_delay_ms(1);
    return true;
}

static void iwl_read_hw_mac(void) {
    if (!s_iwl_dev.mmio_base) return;
    if (!iwl_nic_lock()) return;
    uint32_t mac0 = iwl_read_prph(IWL_WFMP_MAC_ADDR_0);
    uint32_t mac1 = iwl_read_prph(IWL_WFMP_MAC_ADDR_1);
    iwl_nic_unlock();

    uint8_t m[6];
    m[0] = (uint8_t)((mac0 >> 24) & 0xFFu);
    m[1] = (uint8_t)((mac0 >> 16) & 0xFFu);
    m[2] = (uint8_t)((mac0 >>  8) & 0xFFu);
    m[3] = (uint8_t)((mac0 >>  0) & 0xFFu);
    m[4] = (uint8_t)((mac1 >>  8) & 0xFFu);
    m[5] = (uint8_t)((mac1 >>  0) & 0xFFu);

    bool non_zero = (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
    bool not_bcast = !(m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF &&
                       m[3] == 0xFF && m[4] == 0xFF && m[5] == 0xFF);
    if (non_zero && not_bcast && !(m[0] & 0x01u)) {
        memcpy(s_iwl_dev.mac_addr, m, 6);
    }
}

static void iwl_nic_config(uint32_t fw_phy_config) {
    uint32_t hw_rev = iwl_read32(CSR_HW_REV);
    hw_rev = (hw_rev & 0xFFF0u) | ((((hw_rev << 2) & 0xCu) >> 2) << 2);
    if (iwl_nic_lock()) {
        uint32_t hw_step = iwl_read_prph(IWL_WFPM_CTRL_REG);
        hw_step |= IWL_ENABLE_WFPM;
        iwl_write_prph(IWL_WFPM_CTRL_REG, hw_step);
        hw_step = (iwl_read_prph(IWL_AUX_MISC_REG) >> 24) & 0xFu;
        if (hw_step == 0x3u) {
            hw_rev = (hw_rev & 0xFFFFFFF3u) | (2u << 2); /* SILICON_C_STEP */
        }
        iwl_nic_unlock();
    }

    uint32_t reg_val = 0;
    uint8_t mac_step = (uint8_t)((hw_rev & 0x0Cu) >> 2);
    uint8_t mac_dash = (uint8_t)(hw_rev & 0x03u);
    uint8_t radio_type = (uint8_t)((fw_phy_config >> 0) & 0x3u);
    uint8_t radio_step = (uint8_t)((fw_phy_config >> 2) & 0x3u);
    uint8_t radio_dash = (uint8_t)((fw_phy_config >> 4) & 0x3u);

    reg_val |= ((uint32_t)mac_dash << 0) |
               ((uint32_t)mac_step << 2) |
               ((uint32_t)radio_type << 10) |
               ((uint32_t)radio_dash << 12) |
               ((uint32_t)radio_step << 14);

    uint32_t mask = 0x0000FFFFu;
    uint32_t val = iwl_read32(CSR_HW_IF_CONFIG_REG);
    val = (val & ~mask) | reg_val;
    iwl_write32(CSR_HW_IF_CONFIG_REG, val);
}

static bool iwl_setup_rx_dma(void) {
    if (!iwl_alloc_dma_buffers()) return false;

    for (uint32_t i = 0; i < IWL_HW_RX_BUFS; i++) {
        memset(s_rx_buf_virt[i], 0, 4096);
    }
    memset(s_sched_virt, 0, 5 * 4096);
    memset(s_tfd_ring_virt, 0, 8 * 4096);
    memset(s_aux_tfd_ring_virt, 0, 8 * 4096);
    memset(s_txq5_tfd_ring_virt, 0, 8 * 4096);
    memset(s_cmd_buf_virt, 0, 16 * 4096);
    memset(s_txq5_buf_virt, 0, 16 * 4096);
    s_txq5_idx = 0;

    /* Populate 256-entry RX descriptor table wrapping our 32 4-KiB pages */
    for (uint32_t i = 0; i < 256; i++) {
        s_rx_desc_virt[i] = (uint32_t)(s_rx_buf_phys[i % IWL_HW_RX_BUFS] >> 8);
    }
    /* Clear rb_status at offset 2048 of s_rx_desc_virt */
    memset((uint8_t *)s_rx_desc_virt + 2048, 0, 512);
    s_hw_rx_read_idx = 0;

    if (!iwl_nic_lock()) return false;

    /* Stop all TX DMA channels 0..7 and Service Channel 9, clear sticky FH TX errors and load status */
    iwl_write_prph(IWL_SCD_TXFACT, 0);
    for (uint32_t chnl = 0; chnl < 8; chnl++) {
        iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(chnl), 0);
    }
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_FH_SRVC_CHNL), 0);
    iwl_write32(FH_TSSR_TX_ERROR_REG, 0xFFFFFFFFu);
    iwl_write32(FH_UCODE_LOAD_STATUS, 0);

    /* Stop Legacy RX DMA Channel 0 and wait for idle */
    iwl_write32(FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
    for (int i = 0; i < 200; i++) {
        if (iwl_read32(0x1C44u) & (1u << 24)) break;
        __asm__ volatile("pause" ::: "memory");
    }

    iwl_write32(FH_MEM_RCSR_CHNL0_RBDCB_WPTR, 0);
    iwl_write32(FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ, 0);
    iwl_write32(FH_RSCSR_CHNL0_RDPTR, 0);
    iwl_write32(FH_RSCSR_CHNL0_WPTR, 0);
    iwl_write32(FH_RSCSR_CHNL0_RBDCB_BASE_REG, (uint32_t)(s_rx_desc_phys >> 8));
    iwl_write32(FH_RSCSR_CHNL0_STTS_WPTR_REG, (uint32_t)((s_rx_desc_phys + 2048) >> 4));
    iwl_write32(FH_MEM_RCSR_CHNL0_CONFIG_REG,
                0x80000000u | /* CHNL_EN_ENABLE */
                0x00000004u | /* IGNORE_RXF_EMPTY */
                0x00001000u | /* IRQ_DEST_INT_HOST */
                (0x11u << 4) | /* RX_RB_TIMEOUT */
                (8u << 20));   /* RBDCB_SIZE_LOG = 8 (256 entries) */

    /* Write 8-bit CSR_INT_COALESCING (do not overwrite byte 1 CSR_INT_PERIODIC_REG) */
    if (s_iwl_dev.mmio_base) {
        *(volatile uint8_t *)(s_iwl_dev.mmio_base + CSR_INT_COALESCING) = 0x40u;
    }

    /* Configure Keep-Warm page, TX CBBC queues, and SCD_GP_CTRL */
    iwl_write32(FH_KW_MEM_ADDR_REG, (uint32_t)(s_kw_phys >> 4));
    iwl_write32(iwl_fh_cbbc_queue_reg(0), (uint32_t)(s_tfd_ring_phys >> 8));
    iwl_write32(iwl_fh_cbbc_queue_reg(1), (uint32_t)(s_aux_tfd_ring_phys >> 8));
    for (uint32_t qid = 2; qid < IWL_MAX_QUEUES; qid++) {
        if (qid == 5) {
            iwl_write32(iwl_fh_cbbc_queue_reg(5), (uint32_t)(s_txq5_tfd_ring_phys >> 8));
        } else {
            iwl_write32(iwl_fh_cbbc_queue_reg(qid), (uint32_t)((s_rx_desc_phys + 2560) >> 8));
        }
    }
    iwl_write_prph(IWL_SCD_GP_CTRL, iwl_read_prph(IWL_SCD_GP_CTRL) | (1u << 18) | (1u << 0));

    iwl_nic_unlock();

    iwl_write32(CSR_MAC_SHADOW_REG_CTRL, iwl_read32(CSR_MAC_SHADOW_REG_CTRL) | 0x800FFFFFu);
    /* Post initial 24 RX buffers */
    iwl_write32(FH_RSCSR_CHNL0_WPTR, 24);
    return true;
}

static bool iwl_load_chunk(uint32_t dst_addr, const uint8_t *chunk, uint32_t len) {
    if (!s_fw_chunk_phys || !s_fw_chunk_virt) return false;
    if (len > 4096) len = 4096;
    memcpy(s_fw_chunk_virt, chunk, len);
    __asm__ volatile("mfence" ::: "memory");

    bool ext_addr = (dst_addr >= IWL_FW_MEM_EXTENDED_START && dst_addr <= IWL_FW_MEM_EXTENDED_END);
    if (ext_addr) {
        if (!iwl_nic_lock()) return false;
        iwl_write_prph(IWL_LMPM_CHICK, iwl_read_prph(IWL_LMPM_CHICK) | IWL_LMPM_CHICK_EXTENDED_ADDR_SPACE);
        iwl_nic_unlock();
    }

    if (!iwl_nic_lock()) return false;

    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_FH_SRVC_CHNL), 0);
    iwl_write32(FH_SRVC_CHNL_SRAM_ADDR_REG(IWL_FH_SRVC_CHNL), dst_addr);
    iwl_write32(FH_TFDIB_CTRL0_REG(IWL_FH_SRVC_CHNL), (uint32_t)(s_fw_chunk_phys & 0xFFFFFFFFu));
    iwl_write32(FH_TFDIB_CTRL1_REG(IWL_FH_SRVC_CHNL),
                (((uint32_t)((s_fw_chunk_phys >> 32) & 0xFu)) << 28) | len);
    iwl_write32(FH_TCSR_CHNL_TX_BUF_STS_REG(IWL_FH_SRVC_CHNL), (1u << 20) | (1u << 12) | 3u);
    iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(IWL_FH_SRVC_CHNL),
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
                FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);

    iwl_nic_unlock();

    bool ok = false;
    for (int ms = 0; ms < 500; ms++) {
        for (int j = 0; j < 200; j++) {
            uint32_t intr = iwl_read32(CSR_INT);
            uint32_t fh_intr = iwl_read32(CSR_FH_INT_STATUS);
            if ((intr & CSR_INT_BIT_FH_TX) || (fh_intr & 0x00030003u)) {
                iwl_write32(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
                iwl_write32(CSR_INT, CSR_INT_BIT_FH_TX);
                ok = true;
                break;
            }
            if ((intr & (CSR_INT_BIT_HW_ERR | CSR_INT_BIT_SW_ERR)) || (fh_intr & 0x80000000u)) {
                ms = 500;
                break;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        if (ok) break;
        iwl_delay_ms(1);
    }

    if (ext_addr) {
        if (iwl_nic_lock()) {
            iwl_write_prph(IWL_LMPM_CHICK, iwl_read_prph(IWL_LMPM_CHICK) & ~IWL_LMPM_CHICK_EXTENDED_ADDR_SPACE);
            iwl_nic_unlock();
        }
    }
    return ok;
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

/* Compute dBm RSSI from iwl_rx_phy_info_t non_cfg_phy[1] energy byte */
static int8_t iwl_extract_rssi(const iwl_rx_phy_info_t *phy) {
    uint32_t val = phy->non_cfg_phy[1];
    int ea = (int)(val & 0xFFu);
    int eb = (int)((val >> 8) & 0xFFu);
    int ec = (int)((val >> 16) & 0xFFu);
    int best = -100;
    if (ea > 0 && -ea > best) best = -ea;
    if (eb > 0 && -eb > best) best = -eb;
    if (ec > 0 && -ec > best) best = -ec;
    if (best == -100) best = -65;
    if (best < -100) best = -100;
    if (best > -10)  best = -10;
    return (int8_t)best;
}

/* Drain completed RX RBs and demux notifications, command responses, and 802.11 MPDUs */
static void iwl_drain_rx_ring(void) {
    if (!s_rx_desc_virt) return;

    uint32_t intr = iwl_read32(CSR_INT);
    uint32_t fh_intr = iwl_read32(CSR_FH_INT_STATUS);
    if (intr & (CSR_INT_BIT_HW_ERR | CSR_INT_BIT_SW_ERR)) {
        s_last_err_intr = intr;
    }
    if (intr) iwl_write32(CSR_INT, intr);
    if (fh_intr) iwl_write32(CSR_FH_INT_STATUS, fh_intr);

    volatile uint16_t *rb_stts = (volatile uint16_t *)((uint8_t *)s_rx_desc_virt + 2048);
    uint16_t closed_rb = (*rb_stts) & 0x0FFFu;

    while ((s_hw_rx_read_idx & 0x0FFFu) != closed_rb) {
        uint32_t buf_slot = s_hw_rx_read_idx % IWL_HW_RX_BUFS;
        uint8_t *rb = s_rx_buf_virt[buf_slot];

        if (rb) {
            uint32_t offset = 0;
            while (offset + sizeof(iwl_rx_packet_t) < 4096u) {
                const iwl_rx_packet_t *pkt = (const iwl_rx_packet_t *)(rb + offset);
                if (pkt->len_n_flags == IWL_FH_RSCSR_FRAME_INVALID) break;
                if (pkt->hdr.qid == 0 && pkt->hdr.idx == 0 &&
                    pkt->hdr.code == 0 && pkt->hdr.flags == 0) {
                    break;
                }

                uint32_t pkt_len = pkt->len_n_flags & IWL_FH_RSCSR_FRAME_SIZE_MSK;
                if (pkt_len < sizeof(iwl_cmd_header_t) || (4u + pkt_len) > (4096u - offset)) {
                    break;
                }

                uint32_t payload_len = pkt_len - sizeof(iwl_cmd_header_t);
                uint8_t  code = pkt->hdr.code;
                uint8_t  grp  = pkt->hdr.flags;
                uint16_t wide_id = IWL_WIDE_ID(grp, code);
                s_last_rx_wide_id = wide_id;

                if (grp == 0 && code == IWL_HCMD_ALIVE) {
                    if (payload_len >= 2) {
                        s_alive_status = *(const uint16_t *)(pkt->data + 0);
                        if (payload_len >= 44) {
                            s_sched_base = *(const uint32_t *)(pkt->data + 40);
                        }
                        s_alive_received = true;
                    }
                } else if (grp == 0 && code == IWL_CALIB_RES_NOTIF_PHY_DB) {
                    if (payload_len >= 4 && s_phy_db_count < IWL_MAX_PHY_DB_CHUNKS) {
                        uint16_t db_type = *(const uint16_t *)(pkt->data + 0);
                        uint16_t db_len  = *(const uint16_t *)(pkt->data + 2);
                        if ((uint32_t)(4u + db_len) <= payload_len &&
                            s_phy_db_pool_used + db_len <= IWL_PHY_DB_POOL_SIZE) {
                            iwl_phy_db_chunk_t *c = &s_phy_db_chunks[s_phy_db_count++];
                            c->type = db_type;
                            c->length = db_len;
                            c->offset = s_phy_db_pool_used;
                            if (db_len > 0) {
                                memcpy(s_phy_db_pool + s_phy_db_pool_used, pkt->data + 4, db_len);
                                s_phy_db_pool_used += db_len;
                            }
                        }
                    }
                } else if (grp == 0 && code == IWL_INIT_COMPLETE_NOTIF) {
                    s_init_complete_received = true;
                } else if (grp == 0 && code == IWL_HCMD_RX_PHY) {
                    if (payload_len >= sizeof(iwl_rx_phy_info_t)) {
                        memcpy(&s_last_phy_info, pkt->data, sizeof(iwl_rx_phy_info_t));
                        if (s_last_phy_info.channel > 0 && s_last_phy_info.channel < 200) {
                            s_last_rx_chan = (uint8_t)s_last_phy_info.channel;
                        }
                        s_last_rx_rssi = iwl_extract_rssi(&s_last_phy_info);
                    }
                } else if (grp == 0 && code == IWL_HCMD_RX_MPDU) {
                    if (payload_len >= 4) {
                        uint16_t mpdu_len = *(const uint16_t *)(pkt->data + 0);
                        if (mpdu_len >= 24 && mpdu_len <= 2036 &&
                            (uint32_t)(4u + mpdu_len + 4u) <= payload_len) {
                            uint32_t rx_status = *(const uint32_t *)(pkt->data + 4 + mpdu_len);
                            /* Verify CRC_OK (bit 0) and OVERRUN_OK (bit 1) */
                            if ((rx_status & 0x03u) == 0x03u) {
                                uint32_t next_rx = (s_iwl_dev.rx_head + 1) % IWL_RX_RING_SIZE;
                                if (next_rx != s_iwl_dev.rx_tail) {
                                    pbuf_t *dst = &s_rx_pbufs[s_iwl_dev.rx_head];
                                    pbuf_init(dst);
                                    dst->payload = s_rx_buffers[s_iwl_dev.rx_head];
                                    memcpy(dst->payload, pkt->data + 4, mpdu_len);
                                    dst->length = mpdu_len;
                                    dst->total_len = mpdu_len;
                                    dst->flags = IWL_PBUF_PACK_META(s_last_rx_rssi, s_last_rx_chan);
                                    s_iwl_dev.rx_head = next_rx;
                                }
                            }
                        }
                    }
                } else if (wide_id == IWL_WIDE_ID(IWL_LONG_GROUP, IWL_SCAN_COMPLETE_UMAC) ||
                           (grp == 0 && code == IWL_SCAN_COMPLETE_UMAC)) {
                    s_scan_complete = true;
                } else if (grp == 0 && code == IWL_HCMD_TIME_EVENT_NOTIF) {
                    if (payload_len >= 24) {
                        uint32_t action = *(const uint32_t *)(pkt->data + 16);
                        uint32_t status = *(const uint32_t *)(pkt->data + 20);
                        if ((action & 1u) && status != 0) {
                            s_te_started = true;
                        }
                    }
                } else if (grp == 0 && code == 0x02 /* IWM_REPLY_ERROR */) {
                    serial_puts("[IWL] FW REPLY_ERROR payload_len=");
                    serial_print_dec(payload_len);
                    if (payload_len >= 8) {
                        serial_puts(" err_type=");
                        serial_print_hex(*(const uint32_t *)(pkt->data + 0));
                        serial_puts(" cmd_id=");
                        serial_print_hex(pkt->data[4]);
                    }
                    serial_puts("\n");
                }

                /* Match synchronous host command response on Queue 0 (bit 7 of qid is 0 for command acks) */
                if (pkt->hdr.qid == 0 && pkt->hdr.idx == s_pending_cmd_idx &&
                    !(grp == 0 && code == IWL_HCMD_ALIVE)) {
                    s_cmd_failed = (grp == 0 && (pkt->hdr.flags & 0x40u) != 0);
                    uint32_t copy_len = (payload_len < sizeof(s_cmd_resp_buf)) ? payload_len : sizeof(s_cmd_resp_buf);
                    if (copy_len > 0) {
                        memcpy(s_cmd_resp_buf, pkt->data, copy_len);
                    }
                    s_cmd_done = true;
                }

                offset += (4u + pkt_len + (IWL_FH_RSCSR_FRAME_ALIGN - 1u)) & ~(IWL_FH_RSCSR_FRAME_ALIGN - 1u);
            }

            /* Clear RB before recycling so stale packets are never re-parsed on ring wrap */
            memset(rb, 0, 4096);
        }

        s_rx_desc_virt[s_hw_rx_read_idx & 0xFFu] = (uint32_t)(s_rx_buf_phys[buf_slot] >> 8);
        s_hw_rx_read_idx = (s_hw_rx_read_idx + 1) & 0x0FFFu;
        uint32_t wptr = ((uint32_t)s_hw_rx_read_idx + 24u) & ~7u;
        iwl_write32(FH_RSCSR_CHNL0_WPTR, wptr & 0xFFu);

        closed_rb = (*rb_stts) & 0x0FFFu;
    }
}

/* Initialize TX Scheduler and enable Command Queue 0 after ALIVE */
static bool iwl_post_alive(void) {
    if (!iwl_nic_lock()) return false;
    uint32_t base = iwl_read_prph(IWL_SCD_SRAM_BASE_ADDR);
    if (s_sched_base == 0) {
        s_sched_base = base;
    }

    /* Clear TX scheduler context, status, and translation tables in SRAM while holding NIC lock */
    uint32_t nwords = (IWL_SCD_TRANS_TBL_MEM_UPPER_BOUND - IWL_SCD_CONTEXT_MEM_LOWER_BOUND) / 4u;
    iwl_write32(HBUS_TARG_MEM_WADDR, s_sched_base + IWL_SCD_CONTEXT_MEM_LOWER_BOUND);
    for (uint32_t i = 0; i < nwords; i++) {
        iwl_write32(HBUS_TARG_MEM_WDAT, 0);
    }

    /* Set physical address of TX scheduler Byte Count tables (1KB aligned) */
    iwl_write_prph(IWL_SCD_DRAM_BASE_ADDR, (uint32_t)(s_sched_phys >> 10));
    iwl_write_prph(IWL_SCD_CHAINEXT_EN, 0);

    /* Enable Command Queue (qid = 0, fifo = 7 [IWM_TX_FIFO_CMD]) */
    iwl_write32(HBUS_TARG_WRPTR, (0u << 8) | 0u);
    iwl_write_prph(iwl_scd_queue_status_bits_reg(0), (0u << 3) | (1u << 19));
    iwl_write_prph(IWL_SCD_AGGR_SEL, iwl_read_prph(IWL_SCD_AGGR_SEL) & ~(1u << 0));
    iwl_write_prph(iwl_scd_queue_rdptr_reg(0), 0);

    iwl_write_mem32(s_sched_base + IWL_SCD_CONTEXT_QUEUE_OFFSET(0), 0);
    iwl_write_mem32(s_sched_base + IWL_SCD_CONTEXT_QUEUE_OFFSET(0) + 4u,
                    (64u << 0) | (64u << 16));

    iwl_write_prph(iwl_scd_queue_status_bits_reg(0),
                   (1u << 3) | (7u << 0) | (1u << 4) | 0x017F0000u);
    iwl_write_prph(IWL_SCD_EN_CTRL, iwl_read_prph(IWL_SCD_EN_CTRL) | (1u << 0));

    /* Activate TX scheduler */
    iwl_write_prph(IWL_SCD_TXFACT, 0xFFu);

    /* Enable FH TX DMA channels 0..7 with credit enable */
    for (uint32_t chnl = 0; chnl < 8; chnl++) {
        iwl_write32(FH_TCSR_CHNL_TX_CONFIG_REG(chnl),
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
                    FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE);
    }

    iwl_write32(FH_TX_CHICKEN_BITS_REG, iwl_read32(FH_TX_CHICKEN_BITS_REG) | 2u);
    iwl_nic_unlock();

    s_cmd_idx  = 0;
    s_txq5_idx = 0;
    return true;
}

/* Send a Host Command on Command Queue 0 and optionally wait for completion */
static bool iwl_send_cmd_pdu(uint32_t cmd_id, const void *data, uint16_t len, bool wait_resp) {
    uint8_t idx = s_cmd_idx;
    uint32_t slot = idx & (IWL_CMD_SLOTS - 1u);
    uint8_t *buf_virt = s_cmd_buf_virt + slot * IWL_CMD_SLOT_SIZE;
    uintptr_t buf_phys = s_cmd_buf_phys + slot * IWL_CMD_SLOT_SIZE;

    uint8_t group_id = (uint8_t)((cmd_id >> 8) & 0xFFu);
    uint8_t opcode   = (uint8_t)(cmd_id & 0xFFu);
    uint8_t ver      = (uint8_t)((cmd_id >> 16) & 0xFFu);

    uint16_t hdr_len = (group_id != 0) ? sizeof(iwl_cmd_header_wide_t) : sizeof(iwl_cmd_header_t);
    if ((uint32_t)hdr_len + len > IWL_CMD_SLOT_SIZE) return false;

    memset(buf_virt, 0, IWL_CMD_SLOT_SIZE);
    if (group_id != 0) {
        iwl_cmd_header_wide_t *whdr = (iwl_cmd_header_wide_t *)buf_virt;
        whdr->opcode   = opcode;
        whdr->group_id = group_id;
        whdr->idx      = idx;
        whdr->qid      = 0;
        whdr->length   = len;
        whdr->reserved = 0;
        whdr->version  = ver;
    } else {
        iwl_cmd_header_t *hdr = (iwl_cmd_header_t *)buf_virt;
        hdr->code  = opcode;
        hdr->flags = 0;
        hdr->idx   = idx;
        hdr->qid   = 0;
    }

    if (len > 0 && data) {
        memcpy(buf_virt + hdr_len, data, len);
    }

    uint16_t total_len = hdr_len + len;
    iwl_tfd_t *tfd = &s_tfd_ring_virt[idx];
    memset(tfd, 0, sizeof(*tfd));
    tfd->num_tbs = 1;
    tfd->tbs[0].lo = (uint32_t)(buf_phys & 0xFFFFFFFFu);
    tfd->tbs[0].hi_n_len = (uint16_t)(((buf_phys >> 32) & 0xFu) | ((uint32_t)total_len << 4));

    /* Update Byte Count table for Queue 0 (per OpenBSD iwm_update_sched(sc, 0, idx, 0, 0)) */
    uint16_t bc_len = 8;
    if (s_tlv_flags & IWL_UCODE_TLV_FLAGS_DW_BC_TABLE) {
        bc_len = (bc_len + 3u) / 4u;
    }
    uint16_t bc_val = (0u << 12) | bc_len;
    s_sched_virt[0].tfd_offset[idx] = bc_val;
    if (idx < IWL_TFD_QUEUE_SIZE_BC_DUP) {
        s_sched_virt[0].tfd_offset[IWL_TFD_QUEUE_SIZE_MAX + idx] = bc_val;
    }

    s_pending_cmd_idx = idx;
    s_cmd_done = false;
    s_cmd_failed = false;
    memset(s_cmd_resp_buf, 0, sizeof(s_cmd_resp_buf));

    s_cmd_idx = (uint8_t)((idx + 1u) & 0xFFu);
    __asm__ volatile("mfence" ::: "memory");

    /* Kick Command Queue 0 write pointer */
    if (iwl_nic_lock()) {
        iwl_write32(HBUS_TARG_WRPTR, (0u << 8) | (uint32_t)s_cmd_idx);
        iwl_nic_unlock();
    } else {
        iwl_write32(HBUS_TARG_WRPTR, (0u << 8) | (uint32_t)s_cmd_idx);
    }

    if (!wait_resp) {
        s_pending_cmd_idx = 0xFF;
        iwl_delay_ms(2);
        uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);
        iwl_drain_rx_ring();
        spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
        return true;
    }

    for (int ms = 0; ms < 1000; ms++) {
        for (int j = 0; j < 50; j++) {
            uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);
            iwl_drain_rx_ring();
            bool done = s_cmd_done;
            bool failed = s_cmd_failed;
            spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
            if (done) {
                s_pending_cmd_idx = 0xFF;
                return !failed;
            }
            if (s_last_err_intr & (CSR_INT_BIT_HW_ERR | CSR_INT_BIT_SW_ERR)) {
                s_pending_cmd_idx = 0xFF;
                return false;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        iwl_delay_ms(1);
    }

    serial_puts("[IWL] HCMD timeout: grp=");
    serial_print_hex(group_id);
    serial_puts(" op=");
    serial_print_hex(opcode);
    serial_puts(" idx=");
    serial_print_dec(idx);
    serial_puts(" err_int=");
    serial_print_hex(s_last_err_intr);
    serial_puts(" last_rx=");
    serial_print_hex(s_last_rx_wide_id);
    serial_puts("\n");

    s_pending_cmd_idx = 0xFF;
    return false;
}

static bool iwl_send_paging_cmd(void) {
    if (s_num_paging_blk == 0) return true;

    iwl_fw_paging_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.flags = IWL_PAGING_CMD_IS_SECURED |
                IWL_PAGING_CMD_IS_ENABLED |
                (s_last_blk_pages & 0xFFu);
    cmd.block_size = 15; /* 2^15 = 32 KiB */
    cmd.block_num  = s_num_paging_blk;

    cmd.device_phy_addr[0] = (uint32_t)(s_paging_css_phys >> 12);
    for (uint32_t i = 0; i < s_num_paging_blk; i++) {
        cmd.device_phy_addr[i + 1] = (uint32_t)(s_paging_blk_phys[i] >> 12);
    }

    return iwl_send_cmd_pdu(IWL_CMD_ID(IWL_FW_PAGING_BLOCK_CMD, IWL_LONG_GROUP, 0),
                            &cmd, sizeof(cmd), true);
}

static bool iwl_send_sf_config(uint32_t state) {
    static const uint32_t full_def[IWL_SF_NUM_SCENARIO][IWL_SF_NUM_TIMEOUT_TYPES] = {
        { 400, 160 }, { 400, 160 }, { 400, 160 }, { 400, 160 }, { 400, 160 }
    };
    iwl_sf_cfg_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.state = state;
    cmd.watermark[IWL_SF_LONG_DELAY_ON] = 4096;
    cmd.watermark[IWL_SF_FULL_ON]       = 8192;
    for (int i = 0; i < IWL_SF_NUM_SCENARIO; i++) {
        for (int j = 0; j < IWL_SF_NUM_TIMEOUT_TYPES; j++) {
            cmd.long_delay_timeouts[i][j] = 1000000u;
            cmd.full_on_timeouts[i][j]    = full_def[i][j];
        }
    }
    return iwl_send_cmd_pdu(IWL_HCMD_SF_CFG, &cmd, sizeof(cmd), false);
}

static bool iwl_send_tx_ant_cfg(uint8_t valid_ant) {
    iwl_tx_ant_cfg_cmd_t cmd = { .valid = valid_ant };
    return iwl_send_cmd_pdu(IWL_HCMD_TX_ANT_CFG, &cmd, sizeof(cmd), true);
}

static bool iwl_send_phy_cfg(uint32_t phy_cfg, uint32_t flow_trig, uint32_t event_trig) {
    iwl_phy_cfg_cmd_t cmd = {
        .phy_cfg       = phy_cfg,
        .flow_trigger  = flow_trig,
        .event_trigger = event_trig,
    };
    return iwl_send_cmd_pdu(IWL_HCMD_PHY_CFG, &cmd, sizeof(cmd), true);
}

static bool iwl_send_phy_db_section(uint16_t type, uint16_t length, const uint8_t *data) {
    if ((uint32_t)sizeof(iwl_phy_db_cmd_t) + length > sizeof(s_phy_db_scratch)) {
        serial_puts("[IWL] PHY_DB section too large: type=");
        serial_print_dec(type);
        serial_puts(" len=");
        serial_print_dec(length);
        serial_puts("\n");
        return false;
    }
    iwl_phy_db_cmd_t *hdr = (iwl_phy_db_cmd_t *)s_phy_db_scratch;
    hdr->type   = type;
    hdr->length = length;
    if (length > 0 && data) {
        memcpy(s_phy_db_scratch + sizeof(iwl_phy_db_cmd_t), data, length);
    }
    bool ok = iwl_send_cmd_pdu(IWL_HCMD_PHY_DB, s_phy_db_scratch,
                               (uint16_t)(sizeof(iwl_phy_db_cmd_t) + length), false);
    iwl_delay_ms(1);
    return ok;
}

static bool iwl_send_all_phy_db(void) {
    const uint16_t order[4] = { 1 /* CFG */, 2 /* CALIB_NCH */, 4 /* PAPD */, 5 /* TXP */ };
    for (int t = 0; t < 4; t++) {
        uint16_t target_type = order[t];
        for (uint32_t i = 0; i < s_phy_db_count; i++) {
            if (s_phy_db_chunks[i].type == target_type && s_phy_db_chunks[i].length > 0) {
                if (!iwl_send_phy_db_section(target_type,
                                             s_phy_db_chunks[i].length,
                                             s_phy_db_pool + s_phy_db_chunks[i].offset)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool iwl_enable_txq(uint8_t sta_id, uint8_t qid, uint8_t fifo, uint8_t tid) {
    if (iwl_nic_lock()) {
        iwl_write32(HBUS_TARG_WRPTR, ((uint32_t)qid << 8) | 0u);
        iwl_nic_unlock();
    }
    if (qid == 5) {
        s_txq5_idx = 0;
    }
    iwl_scd_txq_cfg_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.tid       = tid;
    cmd.scd_queue = qid;
    cmd.enable    = 1;
    cmd.sta_id    = sta_id;
    cmd.tx_fifo   = fifo;
    cmd.aggregate = 0;
    cmd.ssn       = 0;
    cmd.window    = 64;
    bool ok = iwl_send_cmd_pdu(IWL_HCMD_SCD_QUEUE_CFG, &cmd, sizeof(cmd), true);
    if (ok && qid == 5 && iwl_nic_lock()) {
        iwl_write_prph(iwl_scd_queue_rdptr_reg(5), 0);
        iwl_write_mem32(s_sched_base + IWL_SCD_CONTEXT_QUEUE_OFFSET(5), 0);
        iwl_write_mem32(s_sched_base + IWL_SCD_CONTEXT_QUEUE_OFFSET(5) + 4u,
                        (64u << 0) | (64u << 16));
        iwl_write_prph(iwl_scd_queue_status_bits_reg(5),
                       (1u << 3) | (((uint32_t)fifo & 7u) << 0) | (1u << 4) | 0x017F0000u);
        iwl_nic_unlock();
    }
    return ok;
}

static bool iwl_add_aux_sta(void) {
    /* Enable Queue 1 (IWM_DQA_AUX_QUEUE) on FIFO 5 (IWM_TX_FIFO_MCAST), TID 8 (MAX_TID) */
    if (!iwl_enable_txq(IWL_AUX_STA_ID, 1, 5, 8)) {
        return false;
    }

    iwl_add_sta_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.sta_id         = IWL_AUX_STA_ID;
    cmd.station_type   = IWL_STA_AUX_ACTIVITY;
    cmd.mac_id_n_color = IWL_FW_CMD_ID_AND_COLOR(IWL_MAC_INDEX_AUX, 0);
    cmd.tfd_queue_msk  = (1u << 1);
    cmd.tid_disable_tx = 0xFFFFu;

    if (!iwl_send_cmd_pdu(IWL_HCMD_ADD_STA, &cmd, sizeof(cmd), true)) {
        return false;
    }
    uint32_t status = *(const uint32_t *)s_cmd_resp_buf;
    if ((status & 0xFFu) != 1u /* ADD_STA_SUCCESS */) {
        serial_puts("[IWL] AUX ADD_STA status=");
        serial_print_hex(status);
        serial_puts("\n");
        return false;
    }
    return true;
}

static bool iwl_send_phy_context_cmd(uint32_t id, uint32_t action, uint8_t channel) {
    iwl_phy_context_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = IWL_FW_CMD_ID_AND_COLOR(id, 0);
    cmd.action       = action;
    cmd.apply_time   = 0;
    cmd.ci.band      = (channel <= 14) ? IWL_PHY_BAND_24 : IWL_PHY_BAND_5;
    cmd.ci.channel   = channel;
    cmd.ci.width     = 0; /* 20 MHz */
    cmd.ci.ctrl_pos  = 0;
    cmd.txchain_info = s_valid_tx_ant;
    cmd.rxchain_info = ((uint32_t)s_valid_rx_ant << 1) | (1u << 10) | (1u << 12);

    return iwl_send_cmd_pdu(IWL_HCMD_PHY_CONTEXT, &cmd, sizeof(cmd), true);
}

static bool iwl_config_umac_scan(void) {
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    iwl_scan_config_t *cfg = (iwl_scan_config_t *)buf;

    uint32_t rates = 0x0FFFu; /* All 12 CCK + OFDM basic/supported rates */
    cfg->tx_chains          = s_valid_tx_ant;
    cfg->rx_chains          = s_valid_rx_ant;
    cfg->legacy_rates       = rates | (rates << 16);
    cfg->out_of_channel_time = 0;
    cfg->suspend_time       = 0;
    cfg->dwell_active       = 10;
    cfg->dwell_passive      = 110;
    cfg->dwell_fragmented   = 44;
    cfg->dwell_extended     = 90;
    memcpy(cfg->mac_addr, s_iwl_dev.mac_addr, 6);
    cfg->bcast_sta_id       = IWL_AUX_STA_ID;
    cfg->channel_flags      = 0;

    uint32_t nchan = sizeof(s_scan_channels);
    if (nchan > s_n_scan_channels) nchan = s_n_scan_channels;
    for (uint32_t i = 0; i < nchan; i++) {
        cfg->channel_array[i] = s_scan_channels[i];
    }

    cfg->flags = (1u << 0)  | /* ACTIVATE */
                 (1u << 3)  | /* ALLOW_CHUB_REQS */
                 (1u << 8)  | /* SET_TX_CHAINS */
                 (1u << 9)  | /* SET_RX_CHAINS */
                 (1u << 10) | /* SET_AUX_STA_ID */
                 (1u << 11) | /* SET_ALL_TIMES */
                 (1u << 13) | /* SET_CHANNEL_FLAGS */
                 (1u << 14) | /* SET_LEGACY_RATES */
                 (1u << 15) | /* SET_MAC_ADDR */
                 (1u << 17) | /* CLEAR_FRAGMENTED */
                 (nchan << 26);

    uint16_t cmd_len = (uint16_t)(sizeof(iwl_scan_config_t) + s_n_scan_channels);
    return iwl_send_cmd_pdu(IWL_CMD_ID(IWL_SCAN_CFG_CMD, IWL_LONG_GROUP, 0),
                            buf, cmd_len, true);
}

static bool iwl_send_mac_context_cmd(uint32_t action, bool is_assoc, uint16_t assoc_id) {
    iwl_mac_ctx_cmd_t mac_cmd;
    memset(&mac_cmd, 0, sizeof(mac_cmd));
    mac_cmd.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    mac_cmd.action       = action;
    mac_cmd.mac_type     = IWL_FW_MAC_TYPE_BSS_STA;
    mac_cmd.tsf_id       = 0;
    memcpy(mac_cmd.node_addr, s_iwl_dev.mac_addr, 6);
    memcpy(mac_cmd.bssid_addr, s_assoc_bssid, 6);
    mac_cmd.cck_rates    = (s_iwl_dev.current_channel <= 14) ? 0x0Fu : 0;
    mac_cmd.ofdm_rates   = (s_iwl_dev.current_channel <= 14) ? 0x15u : 0xFFu;
    mac_cmd.short_slot   = (1u << 4); /* IWM_MAC_FLG_SHORT_SLOT (0x10) */
    mac_cmd.filter_flags = IWL_MAC_FILTER_ACCEPT_GRP | (is_assoc ? 0u : IWL_MAC_FILTER_IN_BEACON);
    mac_cmd.qos_flags    = (1u << 0); /* IWM_MAC_QOS_FLG_UPDATE_EDCA */

    /* EDCA AC parameters indexed by TX FIFO: 0=BK, 1=BE, 2=VI, 3=VO */
    static const uint16_t cw_min[4] = { 15, 15, 7, 3 };
    static const uint16_t cw_max[4] = { 1023, 1023, 15, 7 };
    static const uint8_t  aifsn[4]  = { 7, 3, 2, 2 };
    static const uint16_t txop[4]   = { 0, 0, 3008, 1504 };
    for (int f = 0; f < 4; f++) {
        mac_cmd.ac[f].cw_min     = cw_min[f];
        mac_cmd.ac[f].cw_max     = cw_max[f];
        mac_cmd.ac[f].aifsn      = aifsn[f];
        mac_cmd.ac[f].fifos_mask = (uint8_t)(1u << f);
        mac_cmd.ac[f].edca_txop  = txop[f];
    }

    if (is_assoc) {
        uint32_t bi = (s_assoc_bi >= 20) ? s_assoc_bi : 100u;
        uint32_t dtim_off = bi * 1024u;
        mac_cmd.sta.is_assoc                 = 1u;
        mac_cmd.sta.dtim_time                = s_last_phy_info.system_timestamp + dtim_off;
        mac_cmd.sta.dtim_tsf                 = s_last_phy_info.timestamp + (uint64_t)dtim_off;
        mac_cmd.sta.bi                       = bi;
        mac_cmd.sta.bi_reciprocal            = 0xFFFFFFFFu / bi;
        mac_cmd.sta.dtim_interval            = bi;
        mac_cmd.sta.dtim_reciprocal          = 0xFFFFFFFFu / bi;
        mac_cmd.sta.listen_interval          = 10u;
        mac_cmd.sta.assoc_id                 = assoc_id;
        mac_cmd.sta.assoc_beacon_arrive_time = s_last_phy_info.system_timestamp;
    }

    return iwl_send_cmd_pdu(IWL_HCMD_MAC_CONTEXT, &mac_cmd, sizeof(mac_cmd), true);
}

static bool iwl_send_binding_context_cmd(uint32_t action) {
    iwl_binding_cmd_v1_t bind_cmd;
    memset(&bind_cmd, 0, sizeof(bind_cmd));
    bind_cmd.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    bind_cmd.action       = action;
    bind_cmd.macs[0]      = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    bind_cmd.macs[1]      = 0xFFFFFFFFu;
    bind_cmd.macs[2]      = 0xFFFFFFFFu;
    bind_cmd.phy          = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    return iwl_send_cmd_pdu(IWL_HCMD_BINDING_CONTEXT, &bind_cmd, sizeof(bind_cmd), true);
}

static bool iwl_send_add_sta_cmd(bool update, uint16_t assoc_id) {
    iwl_add_sta_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.add_modify        = update ? 1u : 0u;
    cmd.sta_id            = IWL_STATION_ID;
    cmd.station_type      = IWL_STA_LINK;
    cmd.mac_id_n_color    = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    if (!update) {
        memcpy(cmd.addr, s_assoc_bssid, 6);
    } else {
        cmd.modify_mask = (1u << 7) | (1u << 1); /* IWM_STA_MODIFY_QUEUES | IWM_STA_MODIFY_TID_DISABLE_TX */
    }
    cmd.assoc_id          = assoc_id;
    cmd.tfd_queue_msk     = (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    cmd.tid_disable_tx    = 0xFFFFu;
    cmd.station_flags_msk = (3u << 6) | (3u << 8); /* FAT_EN_MSK | MIMO_EN_MSK */
    if (!iwl_send_cmd_pdu(IWL_HCMD_ADD_STA, &cmd, sizeof(cmd), true)) {
        return false;
    }
    uint32_t status = *(const uint32_t *)s_cmd_resp_buf;
    if ((status & 0xFFu) != 1u /* ADD_STA_SUCCESS */) {
        serial_puts("[IWL] STA 0 ADD_STA status=");
        serial_print_hex(status);
        serial_puts("\n");
        return false;
    }
    return true;
}

static bool iwl_send_lq_cmd(uint8_t channel) {
    iwl_lq_cmd_t lq;
    memset(&lq, 0, sizeof(lq));
    lq.sta_id                = IWL_STATION_ID;
    lq.single_stream_ant_msk = (s_valid_tx_ant & 1u) ? 1u : 2u;
    lq.dual_stream_ant_msk   = (s_valid_tx_ant & 3u) ? (s_valid_tx_ant & 3u) : 1u;
    lq.agg_time_limit        = 4000;
    lq.agg_disable_start_th  = 3;
    lq.agg_frame_cnt_limit   = 0x3Fu;

    uint32_t ant_msk = ((uint32_t)lq.single_stream_ant_msk) << 14;
    static const uint8_t ofdm_plcp[8] = { 3, 1, 11, 9, 7, 5, 15, 13 }; /* 54,48,36,24,18,12,9,6 Mbps */
    for (int i = 0; i < 16; i++) {
        if (i < 8) {
            lq.rs_table[i] = ant_msk | ofdm_plcp[i];
        } else if (channel <= 14) {
            static const uint8_t cck_plcp[4] = { 110, 55, 20, 10 }; /* 11, 5.5, 2, 1 Mbps */
            uint8_t plcp = (i - 8 < 4) ? cck_plcp[i - 8] : 10u;
            lq.rs_table[i] = ant_msk | (1u << 9) /* CCK */ | plcp;
        } else {
            lq.rs_table[i] = ant_msk | 13u; /* 6 Mbps OFDM */
        }
    }
    return iwl_send_cmd_pdu(IWL_HCMD_LQ, &lq, sizeof(lq), true);
}

static bool iwl_send_time_quota_cmd(bool running) {
    iwl_time_quota_cmd_t quota_cmd;
    memset(&quota_cmd, 0, sizeof(quota_cmd));
    if (running) {
        quota_cmd.quotas[0].id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
        quota_cmd.quotas[0].quota        = 128;
        quota_cmd.quotas[0].max_duration = 0;
        quota_cmd.quotas[0].low_latency  = 0;
    } else {
        quota_cmd.quotas[0].id_and_color = 0xFFFFFFFFu;
    }
    for (int i = 1; i < 4; i++) {
        quota_cmd.quotas[i].id_and_color = 0xFFFFFFFFu;
    }
    return iwl_send_cmd_pdu(IWL_HCMD_TIME_QUOTA, &quota_cmd, sizeof(quota_cmd), true);
}

static bool iwl_send_mcast_filter_cmd(void) {
    iwl_mcast_filter_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.filter_own = 1;
    cmd.port_id    = 0;
    cmd.count      = 0;
    cmd.pass_all   = 1;
    memcpy(cmd.bssid, s_assoc_bssid, 6);
    return iwl_send_cmd_pdu(IWL_HCMD_MCAST_FILTER, &cmd, sizeof(cmd), true);
}

static bool iwl_send_mac_power_cmd(void) {
    iwl_mac_power_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color       = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    cmd.flags              = 0; /* Keep radio continuously awake during active session */
    cmd.keep_alive_seconds = 25;
    return iwl_send_cmd_pdu(IWL_HCMD_MAC_PM_POWER_TABLE, &cmd, sizeof(cmd), true);
}

static bool iwl_protect_session(uint32_t duration_tu, uint32_t max_delay_tu) {
    iwl_time_event_cmd_t te;
    memset(&te, 0, sizeof(te));
    te.id_and_color = IWL_FW_CMD_ID_AND_COLOR(0, 0);
    te.action       = IWL_FW_CTXT_ACTION_ADD;
    te.id           = 0; /* IWM_TE_BSS_STA_AGGRESSIVE_ASSOC */
    te.apply_time   = 0;
    te.max_delay    = max_delay_tu;
    te.depends_on   = 0;
    te.interval     = 1;
    te.duration     = duration_tu;
    te.repeat       = 1;
    te.max_frags    = 0; /* IWM_TE_V2_FRAG_NONE */
    te.policy       = (1u << 0) | /* HOST_EVENT_START */
                      (1u << 1) | /* HOST_EVENT_END */
                      (1u << 11); /* START_IMMEDIATELY */

    s_te_started = false;
    if (!iwl_send_cmd_pdu(IWL_HCMD_TIME_EVENT, &te, sizeof(te), true)) {
        return false;
    }

    /* Wait up to 30ms for the firmware to notify that the Time Event has started on-channel */
    for (int ms = 0; ms < 30; ms++) {
        iwl_drain_rx_ring();
        if (s_te_started) break;
        iwl_delay_ms(1);
    }
    return true;
}

/* Boot either SEC_INIT (20) or SEC_RT (19), run post_alive, and configure CPU2 paging */
static bool iwl_boot_ucode_image(const void *fw_data, size_t fw_len,
                                 uint32_t target_sec_tlv, uint32_t fw_phy_config,
                                 uint32_t paging_mem_size) {
    s_alive_received = false;
    s_alive_status   = 0;
    s_sched_base     = 0;
    s_num_paging_blk = 0;
    s_last_blk_pages = 0;
    s_last_err_intr  = 0;
    s_last_rx_wide_id = 0;

    if (!iwl_prepare_card_hw()) {
        serial_puts("[IWL] WARN: iwl_prepare_card_hw NIC_READY not asserted, continuing\n");
    }
    iwl_write32(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    iwl_delay_ms(5);

    (void)iwl_apm_init();
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    (void)iwl_apm_init();
    iwl_nic_config(fw_phy_config);

    if (!iwl_setup_rx_dma()) {
        serial_puts("[IWL] Failed to configure RX DMA ring\n");
        return false;
    }

    /* Clear RFKILL and legacy interrupts; enable FH_TX interrupt for firmware loading */
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, 0x06u);
    iwl_write32(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    iwl_write32(CSR_INT, 0xFFFFFFFFu);
    iwl_write32(CSR_INT_MASK, CSR_INT_BIT_FH_TX);
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, 0x02u);
    iwl_write32(CSR_UCODE_DRV_GP1_CLR, 0x02u);

    /* Release internal CPU from reset */
    if (iwl_nic_lock()) {
        iwl_write_prph(IWL_RELEASE_CPU_RESET, IWL_RELEASE_CPU_RESET_BIT);
        iwl_nic_unlock();
    } else {
        serial_puts("[IWL] Failed to acquire NIC lock for CPU reset release\n");
        return false;
    }

    /* Stream CPU1 and CPU2 non-paged sections, and copy CPU2 paging sections to DRAM */
    const uint8_t *ptr = (const uint8_t *)fw_data + sizeof(iwl_ucode_header_t);
    size_t rem = fw_len - sizeof(iwl_ucode_header_t);

    int cpu_phase = 1;
    uint32_t sec_num = 1;
    uint32_t loaded_sections = 0;
    int paging_phase = 0; /* 0 = normal, 1 = CSS, 2 = Paging Data */

    while (rem >= sizeof(iwl_ucode_tlv_t)) {
        const iwl_ucode_tlv_t *tlv = (const iwl_ucode_tlv_t *)ptr;
        size_t step = sizeof(iwl_ucode_tlv_t) + ((tlv->length + 3u) & ~3u);
        if (step > rem) break;

        if (tlv->type == target_sec_tlv && tlv->length >= 4) {
            const uint8_t *payload = ptr + sizeof(iwl_ucode_tlv_t);
            uint32_t dst_addr = *(const uint32_t *)payload;
            const uint8_t *data = payload + 4;
            uint32_t dlen = tlv->length - 4;

            if (paging_phase == 0) {
                if (dst_addr == IWL_CPU1_CPU2_SEPARATOR_SECTION) {
                    if (iwl_nic_lock()) {
                        iwl_write32(FH_UCODE_LOAD_STATUS, 0x0000FFFFu);
                        iwl_nic_unlock();
                    }
                    cpu_phase = 2;
                    sec_num = 1;
                } else if (dst_addr == IWL_PAGING_SEPARATOR_SECTION) {
                    paging_phase = 1;
                } else if (dlen > 0) {
                    if (!iwl_load_section(dst_addr, data, dlen)) {
                        serial_puts("[IWL] Failed to stream firmware chunk at dst=");
                        serial_print_hex(dst_addr);
                        serial_puts("\n");
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
                    loaded_sections++;
                }
            } else if (paging_phase == 1) {
                /* Copy CPU2 Paging CSS block (4 KiB) */
                memset(s_paging_css_virt, 0, 4096);
                uint32_t clen = (dlen > 4096u) ? 4096u : dlen;
                if (clen > 0) memcpy(s_paging_css_virt, data, clen);
                paging_phase = 2;
            } else if (paging_phase == 2) {
                /* Copy CPU2 Paging Data into 32 KiB blocks */
                uint32_t psize = (paging_mem_size > 0) ? paging_mem_size : dlen;
                uint32_t num_pages = (psize + 4095u) / 4096u;
                if (num_pages > IWL_MAX_PAGING_DATA_BLOCKS * 8u) {
                    num_pages = IWL_MAX_PAGING_DATA_BLOCKS * 8u;
                }
                s_num_paging_blk = (num_pages > 0) ? (((num_pages - 1u) / 8u) + 1u) : 0;
                s_last_blk_pages = (s_num_paging_blk > 0) ? (num_pages - 8u * (s_num_paging_blk - 1u)) : 0;

                uint32_t off = 0;
                for (uint32_t b = 0; b < s_num_paging_blk; b++) {
                    memset(s_paging_blk_virt[b], 0, 32768);
                    if (off < dlen) {
                        uint32_t b_len = (b + 1 < s_num_paging_blk) ? 32768u : (s_last_blk_pages * 4096u);
                        if (off + b_len > dlen) b_len = dlen - off;
                        memcpy(s_paging_blk_virt[b], data + off, b_len);
                        off += 32768u;
                    }
                }
                paging_phase = 3;
            }
        }

        ptr += step;
        rem -= step;
    }

    /* Finalize CPU2 */
    if (iwl_nic_lock()) {
        iwl_write32(FH_UCODE_LOAD_STATUS, 0xFFFFFFFFu);
        iwl_nic_unlock();
    }

    iwl_write32(CSR_INT_MASK, CSR_INI_SET_MASK);
    (void)loaded_sections;

    for (int ms = 0; ms < 1500; ms++) {
        iwl_drain_rx_ring();
        if (s_alive_received) break;
        iwl_delay_ms(1);
    }

    if (!s_alive_received || s_alive_status != 0xCAFEu) {
        serial_puts("[IWL] Firmware ALIVE timeout/bad status; status=");
        serial_print_hex(s_alive_status);
        serial_puts(" CSR_INT=");
        serial_print_hex(iwl_read32(CSR_INT));
        serial_puts("\n");
        return false;
    }

    if (!iwl_post_alive()) {
        serial_puts("[IWL] iwl_post_alive failed\n");
        return false;
    }

    if (!iwl_send_paging_cmd()) {
        serial_puts("[IWL] iwl_send_paging_cmd failed\n");
        return false;
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

bool iwlwifi_is_scan_complete(void);

static const wlan_hw_ops_t s_iwlwifi_hw_ops = {
    .name = "iwlwifi",
    .get_mac = iwlwifi_get_mac,
    .set_channel = iwlwifi_set_channel,
    .start_scan = iwlwifi_start_scan,
    .prepare_auth = iwlwifi_prepare_auth,
    .set_associated = iwlwifi_set_associated,
    .deauth = iwlwifi_deauth,
    .send_raw_80211 = iwlwifi_send_raw,
    .poll_raw_80211 = iwlwifi_poll_raw,
    .recycle_raw_80211 = iwlwifi_recycle_raw,
    .is_scan_complete = iwlwifi_is_scan_complete,
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

    uint32_t tlv_count        = 0;
    uint32_t fw_phy_config    = 0;
    uint32_t tlv_flags        = IWL_UCODE_TLV_FLAGS_DW_BC_TABLE;
    uint32_t n_scan_chans     = 40;
    uint32_t paging_mem_size  = 0;
    uint32_t calib_init_flow  = 0;
    uint32_t calib_init_event = 0;
    uint32_t calib_rt_flow    = 0;
    uint32_t calib_rt_event   = 0;

    while (rem >= sizeof(iwl_ucode_tlv_t)) {
        const iwl_ucode_tlv_t *tlv = (const iwl_ucode_tlv_t *)ptr;
        if (tlv->length > rem - sizeof(iwl_ucode_tlv_t)) break;
        const uint8_t *val_ptr = ptr + sizeof(iwl_ucode_tlv_t);

        if (tlv->type == IWL_UCODE_TLV_FLAGS && tlv->length >= 4) {
            tlv_flags = *(const uint32_t *)val_ptr;
        } else if (tlv->type == IWL_UCODE_TLV_PHY_SKU && tlv->length == 4) {
            fw_phy_config = *(const uint32_t *)val_ptr;
        } else if (tlv->type == IWL_UCODE_TLV_N_SCAN_CHANNELS && tlv->length == 4) {
            n_scan_chans = *(const uint32_t *)val_ptr;
        } else if (tlv->type == IWL_UCODE_TLV_PAGING && tlv->length == 4) {
            paging_mem_size = *(const uint32_t *)val_ptr;
        } else if (tlv->type == IWL_UCODE_TLV_DEF_CALIB && tlv->length >= sizeof(iwl_tlv_calib_ctrl_t)) {
            const iwl_tlv_calib_ctrl_t *cal = (const iwl_tlv_calib_ctrl_t *)val_ptr;
            if (cal->ucode_type == 1) { /* IWM_UCODE_TYPE_INIT */
                calib_init_flow  = cal->flow_trigger;
                calib_init_event = cal->event_trigger;
            } else if (cal->ucode_type == 0) { /* IWM_UCODE_TYPE_REGULAR */
                calib_rt_flow    = cal->flow_trigger;
                calib_rt_event   = cal->event_trigger;
            }
        }

        tlv_count++;
        size_t step = sizeof(iwl_ucode_tlv_t) + ((tlv->length + 3u) & ~3u);
        if (step > rem) break;
        ptr += step;
        rem -= step;
    }
    if (tlv_count == 0) return false;

#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.detected || !s_iwl_dev.mmio_base) return true;

    /* If runtime MVM firmware is already initialized and alive, do not reset or re-upload */
    if (s_iwl_dev.mvm_initialized && s_iwl_dev.firmware_alive) {
        return true;
    }

    iwl_enable_pci_and_bridges();
    if (!iwl_alloc_dma_buffers()) {
        serial_puts("[IWL] PMM allocation for FW DMA failed\n");
        return false;
    }

    s_tlv_flags = tlv_flags;
    s_n_scan_channels = (n_scan_chans > 0 && n_scan_chans <= 52) ? n_scan_chans : 40;
    s_valid_tx_ant = (uint8_t)((fw_phy_config >> 16) & 0xFu);
    s_valid_rx_ant = (uint8_t)((fw_phy_config >> 20) & 0xFu);
    if (s_valid_tx_ant == 0) s_valid_tx_ant = 3;
    if (s_valid_rx_ant == 0) s_valid_rx_ant = 3;

    /* ========================================================================
     * Stage 1: Boot SEC_INIT (TLV 20), read HW MAC, and collect PHY_DB calib
     * ======================================================================== */
    s_phy_db_count = 0;
    s_phy_db_pool_used = 0;
    s_init_complete_received = false;

    if (!iwl_boot_ucode_image(fw_data, fw_len, IWL_UCODE_TLV_SEC_INIT, fw_phy_config, paging_mem_size)) {
        s_iwl_dev.firmware_alive = false;
        return false;
    }

    iwl_read_hw_mac();

    (void)iwl_send_sf_config(IWL_SF_INIT_OFF);
    (void)iwl_send_tx_ant_cfg(s_valid_tx_ant);
    if (!iwl_send_phy_cfg(fw_phy_config, calib_init_flow, calib_init_event)) {
        serial_puts("[IWL] SEC_INIT PHY_CFG failed\n");
        return false;
    }

    /* Wait up to 2500ms for PHY_DB calibration chunks (0x6B) and INIT_COMPLETE (0x04) */
    for (int ms = 0; ms < 2500; ms++) {
        iwl_drain_rx_ring();
        if (s_init_complete_received && s_phy_db_count >= 2) break;
        iwl_delay_ms(1);
    }

    /* ========================================================================
     * Stage 2: Reset and boot SEC_RT (TLV 19) Runtime Microcode
     * ======================================================================== */
    if (!iwl_boot_ucode_image(fw_data, fw_len, IWL_UCODE_TLV_SEC_RT, fw_phy_config, paging_mem_size)) {
        s_iwl_dev.firmware_alive = false;
        return false;
    }

    /* Configure runtime radio, PHY DB, BT coex, DQA, AUX STA, PHY contexts, and UMAC scan */
    (void)iwl_send_tx_ant_cfg(s_valid_tx_ant);
    (void)iwl_send_all_phy_db();
    if (!iwl_send_phy_cfg(fw_phy_config, calib_rt_flow, calib_rt_event)) {
        serial_puts("[IWL] SEC_RT PHY_CFG failed\n");
        return false;
    }

    iwl_bt_coex_cmd_t bt_cmd = { .mode = 3, .enabled_modules = (1u << 4) };
    (void)iwl_send_cmd_pdu(IWL_HCMD_BT_CONFIG, &bt_cmd, sizeof(bt_cmd), true);

    iwl_dqa_enable_cmd_t dqa_cmd = { .cmd_queue = 0 };
    (void)iwl_send_cmd_pdu(IWL_CMD_ID(IWL_DQA_ENABLE_CMD, IWL_DATA_PATH_GROUP, 0),
                           &dqa_cmd, sizeof(dqa_cmd), true);

    if (!iwl_add_aux_sta()) {
        serial_puts("[IWL] WARN: iwl_add_aux_sta returned error\n");
    }

    for (uint32_t i = 0; i < 3; i++) {
        (void)iwl_send_phy_context_cmd(i, IWL_FW_CTXT_ACTION_ADD, 1);
    }

    uint8_t temp_ths[20] = {0};
    (void)iwl_send_cmd_pdu(IWL_CMD_ID(IWL_TEMP_REPORT_THS_CMD, IWL_PHY_OPS_GROUP, 0),
                           temp_ths, sizeof(temp_ths), true);

    iwl_device_power_cmd_t pwr_cmd = { .flags = 1, .reserved = 0 };
    (void)iwl_send_cmd_pdu(IWL_HCMD_POWER_TABLE, &pwr_cmd, sizeof(pwr_cmd), true);

    iwl_mcc_update_cmd_t mcc_cmd;
    memset(&mcc_cmd, 0, sizeof(mcc_cmd));
    mcc_cmd.mcc = (uint16_t)(('Z' << 8) | 'Z');
    mcc_cmd.source_id = 0;
    (void)iwl_send_cmd_pdu(IWL_HCMD_MCC_UPDATE, &mcc_cmd, sizeof(mcc_cmd), true);

    if (!iwl_config_umac_scan()) {
        serial_puts("[IWL] WARN: iwl_config_umac_scan failed\n");
    }

    /* Enable default management/data queues 5..8 (IWM_TID_NON_QOS = 0) */
    static const uint8_t ac_to_fifo[4] = { 1, 0, 2, 3 };
    for (uint8_t ac = 0; ac < 4; ac++) {
        (void)iwl_enable_txq(IWL_STATION_ID, (uint8_t)(5u + ac), ac_to_fifo[ac], 0);
    }

    /* Disable hardware beacon filtering so all beacons reach the host */
    uint8_t bf_cmd[44] = {0};
    (void)iwl_send_cmd_pdu(IWL_HCMD_BEACON_FILTERING, bf_cmd, sizeof(bf_cmd), true);

    if (s_last_err_intr != 0) {
        serial_puts("[IWL] Firmware reported error interrupt during SEC_RT init\n");
        s_iwl_dev.firmware_alive  = false;
        s_iwl_dev.mvm_initialized = false;
        return false;
    }

    s_iwl_dev.firmware_alive  = true;
    s_iwl_dev.mvm_initialized = true;
#else
    (void)fw_phy_config;
    (void)tlv_flags;
    (void)n_scan_chans;
    (void)paging_mem_size;
    (void)calib_init_flow;
    (void)calib_init_event;
    (void)calib_rt_flow;
    (void)calib_rt_event;
    s_iwl_dev.firmware_alive  = true;
    s_iwl_dev.mvm_initialized = true;
#endif

    return true;
}

int iwlwifi_deauth(void) {
#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive || !s_iwl_dev.mvm_initialized) {
        return 0;
    }

    if (s_last_err_intr & (CSR_INT_BIT_HW_ERR | CSR_INT_BIT_SW_ERR)) {
        s_is_associated = false;
        s_sta_active = false;
        s_binding_active = false;
        s_mac_active = false;
        return 0;
    }

    if (s_is_associated) {
        (void)iwl_send_sf_config(IWL_SF_INIT_OFF);
        (void)iwl_send_time_quota_cmd(false);
        s_is_associated = false;
    }

    if (s_sta_active) {
        iwl_tx_path_flush_cmd_t flush_cmd = {
            .sta_id   = IWL_STATION_ID,
            .tid_mask = 0xFFFFu,
            .reserved = 0,
        };
        (void)iwl_send_cmd_pdu(IWL_HCMD_TXPATH_FLUSH, &flush_cmd, sizeof(flush_cmd), true);

        iwl_rm_sta_cmd_t rm_cmd = { .sta_id = IWL_STATION_ID, .reserved = {0, 0, 0} };
        (void)iwl_send_cmd_pdu(IWL_HCMD_REMOVE_STA, &rm_cmd, sizeof(rm_cmd), true);
        s_sta_active = false;
    }

    if (s_binding_active) {
        (void)iwl_send_binding_context_cmd(IWL_FW_CTXT_ACTION_REMOVE);
        s_binding_active = false;
    }

    if (s_mac_active) {
        (void)iwl_send_mac_context_cmd(IWL_FW_CTXT_ACTION_REMOVE, false, 0);
        s_mac_active = false;
    }
#endif
    return 0;
}

int iwlwifi_prepare_auth(uint8_t channel, const uint8_t bssid[6], uint16_t beacon_interval) {
#ifndef IWLWIFI_HOST_TEST
    if (!bssid || !s_iwl_dev.initialized || !s_iwl_dev.firmware_alive || !s_iwl_dev.mvm_initialized) {
        return -1;
    }

    /* Tear down any previous station/binding/MAC context before starting new authentication */
    (void)iwlwifi_deauth();

    s_iwl_dev.current_channel = (channel > 0) ? channel : 1;
    memcpy(s_assoc_bssid, bssid, 6);
    s_assoc_bi  = (beacon_interval >= 20) ? beacon_interval : 100;
    s_assoc_aid = 0;

    /* 1. Tune PHY Context 0 to target channel */
    if (!iwl_send_phy_context_cmd(0, IWL_FW_CTXT_ACTION_MODIFY, s_iwl_dev.current_channel)) {
        serial_puts("[IWL] WARN: prepare_auth PHY_CONTEXT modify failed\n");
    }

    /* 2. Add MAC Context 0 (BSS_STA, unassociated) */
    if (iwl_send_mac_context_cmd(IWL_FW_CTXT_ACTION_ADD, false, 0)) {
        s_mac_active = true;
    } else {
        serial_puts("[IWL] WARN: prepare_auth MAC_CONTEXT add failed\n");
    }

    /* 3. Bind MAC Context 0 to PHY Context 0 */
    if (iwl_send_binding_context_cmd(IWL_FW_CTXT_ACTION_ADD)) {
        s_binding_active = true;
    } else {
        serial_puts("[IWL] WARN: prepare_auth BINDING_CONTEXT add failed\n");
    }

    /* 4. Add Station 0 (Target AP BSSID) on DQA queues 5..8 */
    if (iwl_send_add_sta_cmd(false, 0)) {
        s_sta_active = true;
    } else {
        serial_puts("[IWL] WARN: prepare_auth ADD_STA failed\n");
    }

    /* Bind DQA queues 5..8 to Station 0 now that Station 0 exists in firmware */
    static const uint8_t ac_to_fifo[4] = { 1, 0, 2, 3 };
    for (uint8_t ac = 0; ac < 4; ac++) {
        (void)iwl_enable_txq(IWL_STATION_ID, (uint8_t)(5u + ac), ac_to_fifo[ac], 0);
    }

    /* 5. Configure Link Quality rate table for Station 0 */
    (void)iwl_send_lq_cmd(s_iwl_dev.current_channel);

    /* 6. Schedule Session Protection Time Event (1500 TU) so radio stays on-channel for Auth/Assoc/EAPOL */
    (void)iwl_protect_session(1500, 100);
    return 0;
#else
    (void)channel;
    (void)bssid;
    (void)beacon_interval;
    return 0;
#endif
}

int iwlwifi_set_associated(uint16_t assoc_id, uint16_t beacon_interval) {
#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive || !s_iwl_dev.mvm_initialized) {
        return -1;
    }

    s_assoc_aid = (assoc_id > 0) ? (assoc_id & 0x3FFFu) : 1;
    if (beacon_interval >= 20) {
        s_assoc_bi = beacon_interval;
    }

    /* 1. Update Station 0 with assigned AID */
    (void)iwl_send_add_sta_cmd(true, s_assoc_aid);

    /* 2. Modify MAC Context 0 with is_assoc = 1 and assoc_id */
    (void)iwl_send_mac_context_cmd(IWL_FW_CTXT_ACTION_MODIFY, true, s_assoc_aid);

    /* 3. Enable full Smart FIFO, Multicast reception, MAC power, Time Quota, and LQ rates */
    (void)iwl_send_sf_config(IWL_SF_FULL_ON);
    (void)iwl_send_mcast_filter_cmd();
    (void)iwl_send_mac_power_cmd();
    (void)iwl_send_time_quota_cmd(true);
    (void)iwl_send_lq_cmd(s_iwl_dev.current_channel);

    s_is_associated = true;
    return 0;
#else
    (void)assoc_id;
    (void)beacon_interval;
    return 0;
#endif
}

int iwlwifi_start_scan(const char *ssid) {
#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive || !s_iwl_dev.mvm_initialized) {
        return -1;
    }

    /* If a previous unassociated auth context is lingering, release it before UMAC scan */
    if (!s_is_associated && (s_mac_active || s_binding_active || s_sta_active)) {
        (void)iwlwifi_deauth();
    }

    memset(s_scan_cmd_buf, 0, sizeof(s_scan_cmd_buf));

    iwl_scan_req_umac_v7_t *req = (iwl_scan_req_umac_v7_t *)s_scan_cmd_buf;
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len > 32) ssid_len = 32;

    req->flags                       = 0;
    req->uid                         = 0;
    req->ooc_priority                = 2; /* IWM_SCAN_PRIORITY_HIGH */
    req->general_flags               = (1u << 2)  | /* PASS_ALL */
                                       (1u << 5)  | /* ITER_COMPLETE */
                                       (1u << 8)  | /* RRM_ENABLED (DS_PARAM + WFA_TPC) */
                                       (1u << 13) | /* ADAPTIVE_DWELL */
                                       ((ssid_len > 0) ? (1u << 4) /* PRE_CONNECT */ : (1u << 3) /* PASSIVE */);
    req->scan_start_mac_id           = 0;
    req->active_dwell                = 10;
    req->passive_dwell               = 110;
    req->fragmented_dwell            = 44;
    req->adwell_default_n_aps        = 2;
    req->adwell_default_n_aps_social = 10;
    req->adwell_max_budget           = (ssid_len > 0) ? 100 : 300;
    req->max_out_time[0]             = 0;
    req->suspend_time[0]             = 0;
    req->scan_priority               = 2; /* HIGH */

    uint32_t nchan = sizeof(s_scan_channels);
    if (nchan > s_n_scan_channels) nchan = s_n_scan_channels;
    req->channel.flags = 0;
    req->channel.count = (uint8_t)nchan;

    iwl_scan_channel_cfg_umac_t *chans = (iwl_scan_channel_cfg_umac_t *)(s_scan_cmd_buf + sizeof(iwl_scan_req_umac_v7_t));
    for (uint32_t i = 0; i < nchan; i++) {
        chans[i].flags         = (ssid_len > 0) ? (1u << 0) : 0;
        chans[i].channel_num   = s_scan_channels[i];
        chans[i].iter_count    = 1;
        chans[i].iter_interval = 0;
    }

    /* Tail v1 starts after s_n_scan_channels channel slots */
    iwl_scan_req_umac_tail_v1_t *tail = (iwl_scan_req_umac_tail_v1_t *)
        (s_scan_cmd_buf + sizeof(iwl_scan_req_umac_v7_t) + sizeof(iwl_scan_channel_cfg_umac_t) * s_n_scan_channels);

    tail->schedule[0].interval   = 0;
    tail->schedule[0].iter_count = 1;
    tail->delay                  = 0;

    /* Populate Probe Request template in tail->preq */
    uint8_t *p = tail->preq.buf;
    /* 802.11 Mgmt Probe Request Header (24 bytes) */
    p[0] = 0x40; p[1] = 0x00; /* FC: Probe Request */
    p[2] = 0x00; p[3] = 0x00; /* Duration */
    memset(p + 4, 0xFF, 6);   /* Addr1: Broadcast */
    memcpy(p + 10, s_iwl_dev.mac_addr, 6); /* Addr2: SA */
    memset(p + 16, 0xFF, 6);  /* Addr3: Broadcast BSSID */
    p[22] = 0x00; p[23] = 0x00;
    /* Wildcard SSID IE (2 bytes) */
    p[24] = 0x00; p[25] = 0x00;
    tail->preq.mac_header.offset = 0;
    tail->preq.mac_header.len    = 26;

    /* 2.4 GHz band IEs: Supported Rates (1,2,5.5,11,6,9,12,18) + Extended Rates (24,36,48,54) + DS Param Set */
    uint8_t *b24 = p + 26;
    b24[0] = 1;  b24[1] = 8;
    b24[2] = 0x82; b24[3] = 0x84; b24[4] = 0x8B; b24[5] = 0x96;
    b24[6] = 0x0C; b24[7] = 0x12; b24[8] = 0x18; b24[9] = 0x24;
    b24[10] = 50; b24[11] = 4;
    b24[12] = 0x30; b24[13] = 0x48; b24[14] = 0x60; b24[15] = 0x6C;
    b24[16] = 3;  b24[17] = 1;  b24[18] = 0; /* DS Param Set IE */
    tail->preq.band_data[0].offset = 26;
    tail->preq.band_data[0].len    = 19;

    /* 5 GHz band IEs: Supported Rates (6,9,12,18,24,36,48,54) */
    uint8_t *b5 = b24 + 19;
    b5[0] = 1; b5[1] = 8;
    b5[2] = 0x8C; b5[3] = 0x12; b5[4] = 0x98; b5[5] = 0x24;
    b5[6] = 0xB0; b5[7] = 0x48; b5[8] = 0x60; b5[9] = 0x6C;
    tail->preq.band_data[1].offset = 26 + 19;
    tail->preq.band_data[1].len    = 10;

    tail->preq.common_data.offset  = 26 + 19 + 10;
    tail->preq.common_data.len     = 0;

    if (ssid_len > 0) {
        tail->direct_scan[0].id  = 0;
        tail->direct_scan[0].len = (uint8_t)ssid_len;
        memcpy(tail->direct_scan[0].ssid, ssid, ssid_len);
    }

    uint16_t total_cmd_len = (uint16_t)(sizeof(iwl_scan_req_umac_v7_t) +
                                        sizeof(iwl_scan_channel_cfg_umac_t) * s_n_scan_channels +
                                        sizeof(iwl_scan_req_umac_tail_v1_t));

    s_scan_complete = false;
    if (!iwl_send_cmd_pdu(IWL_CMD_ID(IWL_SCAN_REQ_UMAC, IWL_LONG_GROUP, 0),
                          s_scan_cmd_buf, total_cmd_len, true)) {
        serial_puts("[IWL] WARN: SCAN_REQ_UMAC command failed\n");
        return -1;
    }

    return 0;
}

bool iwlwifi_is_scan_complete(void) {
    return s_scan_complete;
}
#else
    (void)ssid;
    return 0;
}

bool iwlwifi_is_scan_complete(void) {
    return true;
}
#endif

bool iwlwifi_map_mmio(void) {
#ifndef IWLWIFI_HOST_TEST
    if (!s_iwl_dev.detected) return false;
    uint64_t bar0 = s_iwl_dev.pci.bar[0];
    if (bar0 == 0) return false;

    /* Enable PCI bus master and memory space on endpoint and any parent PCIe bridge */
    iwl_enable_pci_and_bridges();

    /* Pre-allocate low-memory DMA buffers at boot time */
    (void)iwl_alloc_dma_buffers();

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
    if (!buf || len < 24 || len > 1900) return -1;
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive) return -1;
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);

#ifndef IWLWIFI_HOST_TEST
    if (s_iwl_dev.mvm_initialized && s_sta_active && s_txq5_tfd_ring_virt && s_txq5_buf_virt && s_sched_virt) {
        const uint8_t *frame = (const uint8_t *)buf;
        uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
        uint8_t type    = (uint8_t)((fc >> 2) & 0x03u);
        uint8_t subtype = (uint8_t)((fc >> 4) & 0x0Fu);
        bool is_qos     = (type == 0x02u /* DATA */ && subtype == 0x08u /* QOS_DATA */);
        uint16_t hdrlen = is_qos ? 26u : 24u;
        if (len < hdrlen) {
            spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
            return -1;
        }
        uint16_t pad = (hdrlen & 3u) ? (uint16_t)(4u - (hdrlen & 3u)) : 0u;
        uint16_t body_len = (uint16_t)(len - hdrlen);

        uint8_t idx = s_txq5_idx;
        uint32_t b_slot = idx & 31u;
        uint8_t *slot_virt = s_txq5_buf_virt + b_slot * 2048u;
        uintptr_t slot_phys = s_txq5_buf_phys + b_slot * 2048u;

        memset(slot_virt, 0, 2048);

        /* 1. Build 4-byte Command Header at offset 0 */
        iwl_cmd_header_t *hdr = (iwl_cmd_header_t *)slot_virt;
        hdr->code  = IWL_HCMD_TX;
        hdr->flags = 0;
        hdr->idx   = idx;
        hdr->qid   = 5; /* IWM_DQA_MIN_MGMT_QUEUE */

        /* 2. Build 56-byte iwl_tx_cmd_t at offset 4 */
        iwl_tx_cmd_t *tx = (iwl_tx_cmd_t *)(slot_virt + sizeof(iwl_cmd_header_t));
        tx->len = (uint16_t)len;
        tx->offload_assist = (pad > 0) ? (1u << 13) /* IWM_TX_CMD_OFFLD_PAD */ : 0u;

        uint32_t tx_flags = (1u << 12); /* IWM_TX_CMD_FLG_BT_DIS */
        if ((frame[4] & 0x01u) == 0) {
            tx_flags |= (1u << 3);      /* IWM_TX_CMD_FLG_ACK for unicast */
        }
        if (!is_qos) {
            tx_flags |= (1u << 13);     /* IWM_TX_CMD_FLG_SEQ_CTL for non-QoS */
        }
        if (pad > 0) {
            tx_flags |= (1u << 20);     /* IWM_TX_CMD_FLG_MH_PAD */
        }
        tx->tx_flags = tx_flags;

        uint32_t ant_msk = ((s_valid_tx_ant & 1u) ? 1u : 2u) << 14;
        if (type != 0x02u && s_iwl_dev.current_channel <= 14) {
            /* 2.4 GHz Management frame: 1 Mbps CCK (PLCP 10) */
            tx->rate_n_flags = ant_msk | (1u << 9) /* CCK */ | 10u;
        } else {
            /* 5 GHz Management or 2.4/5 GHz Data/EAPOL: 6 Mbps OFDM (PLCP 13) */
            tx->rate_n_flags = ant_msk | 13u;
        }

        tx->sta_id             = IWL_STATION_ID;
        tx->sec_ctl            = 0; /* Software AES-CCMP already applied */
        tx->initial_rate_index = 0;
        tx->life_time          = 0xFFFFFFFFu;

        uintptr_t scratch_pa = slot_phys + sizeof(iwl_cmd_header_t) + 8u;
        tx->dram_lsb_ptr     = (uint32_t)(scratch_pa & 0xFFFFFFFFu);
        tx->dram_msb_ptr     = (uint8_t)((scratch_pa >> 32) & 0xFu);
        tx->rts_retry_limit  = 3;
        tx->data_retry_limit = (type == 0x00u /* MGMT */) ? 3 : 15;
        tx->tid_tspec        = (type == 0x02u /* DATA */) ? 0 /* NON_QOS */ : 8 /* MAX_TID */;
        tx->pm_frame_timeout = (type == 0x00u /* MGMT */) ? ((subtype == 0x00u /* ASSOC_REQ */) ? 3 : 2) : 0;

        /* 3. Copy 802.11 MAC header immediately after iwl_tx_cmd_t (offset 60) + pad */
        memcpy(slot_virt + sizeof(iwl_cmd_header_t) + sizeof(iwl_tx_cmd_t), frame, hdrlen);

        /* 4. Copy frame payload to offset 128 */
        if (body_len > 0) {
            memcpy(slot_virt + 128u, frame + hdrlen, body_len);
        }

        /* 5. Populate TFD on Queue 5 with TB0 (16B), TB1 (rest of cmd + hdr + pad), and TB2 (payload) */
        iwl_tfd_t *tfd = &s_txq5_tfd_ring_virt[idx];
        memset(tfd, 0, sizeof(*tfd));
        uint16_t tb1_len = (uint16_t)(sizeof(iwl_cmd_header_t) + sizeof(iwl_tx_cmd_t) + hdrlen + pad - 16u);
        tfd->tbs[0].lo       = (uint32_t)(slot_phys & 0xFFFFFFFFu);
        tfd->tbs[0].hi_n_len = (uint16_t)(((slot_phys >> 32) & 0xFu) | (16u << 4));
        tfd->tbs[1].lo       = (uint32_t)((slot_phys + 16u) & 0xFFFFFFFFu);
        tfd->tbs[1].hi_n_len = (uint16_t)((((slot_phys + 16u) >> 32) & 0xFu) | ((uint32_t)tb1_len << 4));
        if (body_len > 0) {
            uintptr_t body_pa    = slot_phys + 128u;
            tfd->tbs[2].lo       = (uint32_t)(body_pa & 0xFFFFFFFFu);
            tfd->tbs[2].hi_n_len = (uint16_t)(((body_pa >> 32) & 0xFu) | ((uint32_t)body_len << 4));
            tfd->num_tbs         = 3;
        } else {
            tfd->num_tbs         = 2;
        }

        /* 6. Update TX Scheduler Byte-Count Table for Queue 5, Station 0 */
        uint16_t bc_len = (uint16_t)(len + 8u); /* + CRC (4) + Delimiter (4) */
        if (s_tlv_flags & IWL_UCODE_TLV_FLAGS_DW_BC_TABLE) {
            bc_len = (uint16_t)((bc_len + 3u) / 4u);
        }
        uint16_t bc_val = (uint16_t)(((uint16_t)IWL_STATION_ID << 12) | (bc_len & 0x0FFFu));
        s_sched_virt[5].tfd_offset[idx] = bc_val;
        if (idx < IWL_TFD_QUEUE_SIZE_BC_DUP) {
            s_sched_virt[5].tfd_offset[IWL_TFD_QUEUE_SIZE_MAX + idx] = bc_val;
        }

        s_txq5_idx = (uint8_t)((idx + 1u) & 0xFFu);
        __asm__ volatile("mfence" ::: "memory");

        /* 7. Ring Queue 5 Doorbell */
        if (iwl_nic_lock()) {
            iwl_write32(HBUS_TARG_WRPTR, (5u << 8) | (uint32_t)s_txq5_idx);
            iwl_nic_unlock();
        } else {
            iwl_write32(HBUS_TARG_WRPTR, (5u << 8) | (uint32_t)s_txq5_idx);
        }
    }
#endif

    /* Also record in software ring for host unit test inspection */
    uint32_t next = (s_iwl_dev.tx_head + 1) % IWL_TX_RING_SIZE;
    if (next != s_iwl_dev.tx_tail) {
        iwl_tx_desc_t *desc = (iwl_tx_desc_t *)s_tx_ring_mem + s_iwl_dev.tx_head;
        desc->buf_low = (uint32_t)(uintptr_t)buf;
        desc->buf_high = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
        desc->len = (uint16_t)len;
        desc->flags = 1; /* Valid */
        s_iwl_dev.tx_head = next;
    }

    spin_unlock_irqrestore(&s_iwl_dev.lock, irq);
    return 0;
}

pbuf_t *iwlwifi_poll_raw(void) {
    if (!s_iwl_dev.initialized || !s_iwl_dev.firmware_alive) return NULL;
    uint64_t irq = spin_lock_irqsave(&s_iwl_dev.lock);

#ifndef IWLWIFI_HOST_TEST
    iwl_drain_rx_ring();
#endif

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
#ifndef IWLWIFI_HOST_TEST
    if (s_iwl_dev.mvm_initialized && channel > 0) {
        (void)iwl_send_phy_context_cmd(0, IWL_FW_CTXT_ACTION_MODIFY, channel);
    }
#endif
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
