#ifndef FORTRESS_IWLWIFI_H
#define FORTRESS_IWLWIFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "pci.h"
#include "net.h"

#define IWL_PCI_VENDOR_ID               0x8086

/* Supported Intel Wireless Device IDs */
#define IWL_DEV_ID_AC_8265              0x24FD  /* Dell Latitude 5590 */
#define IWL_DEV_ID_AC_8260              0x24F3
#define IWL_DEV_ID_AC_7265              0x095A
#define IWL_DEV_ID_AC_3165              0x3165
#define IWL_DEV_ID_AC_9260              0x2526
#define IWL_DEV_ID_AX_200               0x2723
#define IWL_DEV_ID_AX_201               0x43F0
#define IWL_DEV_ID_AX_210               0x2725
#define IWL_DEV_ID_AX_211               0x51F0  /* Dell Latitude 5530 */

/* Core CSR Registers */
#define CSR_HW_IF_CONFIG_REG            0x000
#define CSR_INT_COALESCING              0x004
#define CSR_INT                         0x008
#define CSR_INT_MASK                    0x00C
#define CSR_FH_INT_STATUS               0x010
#define CSR_GPIO_IN                     0x018
#define CSR_RESET                       0x020
#define CSR_GP_CNTRL                    0x024
#define CSR_HW_REV                      0x028
#define CSR_UCODE_DRV_GP1_CLR           0x05C
#define CSR_EEPROM_REG                  0x02C
#define CSR_OTP_GP_REG                  0x034

/* CSR_RESET Flags */
#define CSR_RESET_REG_FLAG_SW_RESET     0x00000080
#define CSR_RESET_REG_FLAG_MASTER_DIS   0x00000100
#define CSR_RESET_REG_FLAG_STOP_MASTER  0x00000200

/* CSR_GP_CNTRL Flags */
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY 0x00000001
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE       0x00000004
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ  0x00000008
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP  0x00000010

/* Interrupt bits */
#define CSR_INT_BIT_ALIVE               (1u << 0)
#define CSR_INT_BIT_SW_ERR              (1u << 25)
#define CSR_INT_BIT_FH_TX               (1u << 27)
#define CSR_INT_BIT_HW_ERR              (1u << 29)
#define CSR_INT_BIT_FH_RX               (1u << 31)

/* Peripheral register access (HBUS PRPH) */
#define HBUS_TARG_PRPH_WADDR            0x444
#define HBUS_TARG_PRPH_RADDR            0x448
#define HBUS_TARG_PRPH_WDAT             0x44C
#define HBUS_TARG_PRPH_RDAT             0x450

/* Microcode Control & Flow Handler (Family 8000) */
#define IWL_RELEASE_CPU_RESET           0x300C
#define IWL_RELEASE_CPU_RESET_BIT       0x01000000
#define IWL_WFPM_CTRL_REG               0xA03030u
#define IWL_ENABLE_WFPM                 0x80000000u
#define IWL_WFPM_GP2                    0xA030B4u
#define IWL_AUX_MISC_REG                0xA200B0u
#define IWL_SB_CPU_1_STATUS             0xA01E30u
#define IWL_SB_CPU_2_STATUS             0xA01E34u

#define IWL_FH_SRVC_CHNL                9
#define FH_SRVC_CHNL_SRAM_ADDR_REG(c)   (0x19C8 + (((c) - 9) * 0x4))
#define FH_TFDIB_CTRL0_REG(c)           (0x1900 + (8 * (c)))
#define FH_TFDIB_CTRL1_REG(c)           (0x1900 + (8 * (c)) + 4)
#define FH_TCSR_CHNL_TX_CONFIG_REG(c)   (0x1D00 + ((c) * 0x20))
#define FH_TCSR_CHNL_TX_BUF_STS_REG(c)  (0x1D00 + ((c) * 0x20) + 8)
#define FH_TSSR_TX_STATUS_REG           0x1EB0
#define FH_TSSR_TX_ERROR_REG            0x1EB8
#define FH_UCODE_LOAD_STATUS            0x1AF0

#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE    0x80000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE 0x00000000u
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD   0x00100000u

#define CSR_GIO_REG                     0x03C
#define CSR_GIO_REG_VAL_L0S_DISABLED    0x00000002u
#define CSR_GIO_CHICKEN_BITS            0x100
#define CSR_GIO_CHICKEN_BITS_L1A_NO_L0S_RX 0x00800000u
#define CSR_DBG_HPET_MEM_REG            0x240
#define CSR_DBG_HPET_MEM_REG_VAL        0xFFFF0000u
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A 0x00080000u

#define IWL_FW_MEM_EXTENDED_START       0x40000u
#define IWL_FW_MEM_EXTENDED_END         0x57FFFu
#define IWL_LMPM_CHICK                  0xA01FF8u
#define IWL_LMPM_CHICK_EXTENDED_ADDR_SPACE 0x01u

#define FH_RSCSR_CHNL0_STTS_WPTR_REG    0x1BC0
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG   0x1BC4
#define FH_RSCSR_CHNL0_WPTR             0x1BC8
#define FH_RSCSR_CHNL0_RDPTR            0x1BCC
#define FH_MEM_RCSR_CHNL0_CONFIG_REG    0x1C00
#define FH_MEM_RCSR_CHNL0_RBDCB_WPTR    0x1C08
#define FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ  0x1C10
#define FH_KW_MEM_ADDR_REG              0x197C

#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY   0x00400000
#define CSR_HW_IF_CONFIG_REG_PREPARE         0x08000000
#define CSR_MBOX_SET_REG                     0x088
#define CSR_MBOX_SET_REG_OS_ALIVE            0x00000020
#define CSR_MAC_SHADOW_REG_CTRL              0x0A8

/* Firmware Microcode TLV format */
#define IWL_UCODE_TLV_MAGIC             0x0a4c5749  /* "IWL\n" */
#define IWL_UCODE_TLV_SEC_RT            19
#define IWL_UCODE_TLV_SEC_INIT          20
#define IWL_UCODE_TLV_SEC_WOWLAN        21
#define IWL_UCODE_TLV_PHY_SKU           23

#define IWL_CPU1_CPU2_SEPARATOR_SECTION 0xFFFFCCCC
#define IWL_PAGING_SEPARATOR_SECTION    0xAAAABBBB

typedef struct {
    uint32_t zero;
    uint32_t magic;
    char     build[80];
} __attribute__((packed)) iwl_ucode_header_t;

_Static_assert(sizeof(iwl_ucode_header_t) == 88, "iwl_ucode_header_t must be 88 bytes");

typedef struct {
    uint32_t type;
    uint32_t length;
} __attribute__((packed)) iwl_ucode_tlv_t;

/* Descriptor Ring Sizes */
#define IWL_TX_RING_SIZE                64
#define IWL_RX_RING_SIZE                64

/* Tx / Command Descriptor */
typedef struct {
    uint32_t buf_low;
    uint32_t buf_high;
    uint16_t len;
    uint16_t flags;
} __attribute__((packed)) iwl_tx_desc_t;

/* Rx Descriptor */
typedef struct {
    uint32_t buf_low;
    uint32_t buf_high;
} __attribute__((packed)) iwl_rx_desc_t;

/* Host Commands */
#define IWL_HCMD_ALIVE                  0x01
#define IWL_HCMD_PHY_CONTEXT            0x08
#define IWL_HCMD_BINDING_CONTEXT        0x09
#define IWL_HCMD_MAC_CONTEXT            0x28
#define IWL_HCMD_SCAN_REQUEST           0x80
#define IWL_HCMD_TX                     0x1C
#define IWL_HCMD_RX                     0xC0

typedef struct {
    uint8_t  cmd_id;
    uint8_t  subcmd_id;
    uint16_t sequence;
    uint8_t  flags;
    uint8_t  reserved[3];
} __attribute__((packed)) iwl_cmd_hdr_t;

/* Device Driver State */
typedef struct {
    bool         detected;
    bool         initialized;
    bool         firmware_alive;
    pci_device_t pci;
    uintptr_t    mmio_base;
    uint64_t     mmio_size;
    uint8_t      mac_addr[6];
    uint8_t      current_channel;

    /* DMA Rings */
    uint32_t     tx_head;
    uint32_t     tx_tail;
    uint32_t     rx_head;
    uint32_t     rx_tail;

    spinlock_t   lock;
} iwlwifi_state_t;

/* Virtual MMIO mapping base */
#define IWLWIFI_MMIO_VIRT               0xFFFFFFFFE3000000ULL

/* Public API */
bool iwlwifi_probe(void);
bool iwlwifi_map_mmio(void);
bool iwlwifi_is_supported_device(uint16_t vendor, uint16_t device);
bool iwlwifi_init_hw(void);
bool iwlwifi_load_firmware(const void *fw_data, size_t fw_len);
int  iwlwifi_send_raw(const void *buf, size_t len);
pbuf_t *iwlwifi_poll_raw(void);
void iwlwifi_recycle_raw(pbuf_t *p);
int  iwlwifi_set_channel(uint8_t channel);
int  iwlwifi_get_mac(uint8_t mac[6]);
bool iwlwifi_is_detected(void);

#endif /* FORTRESS_IWLWIFI_H */
