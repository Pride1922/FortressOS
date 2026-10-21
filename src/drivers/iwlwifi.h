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
#define CSR_INT                         0x008
#define CSR_INT_MASK                    0x00C
#define CSR_FH_INT_STATUS               0x010
#define CSR_GPIO_IN                     0x018
#define CSR_RESET                       0x020
#define CSR_GP_CNTRL                    0x044
#define CSR_EEPROM_REG                  0x02C
#define CSR_OTP_GP_REG                  0x034
#define CSR_UCODE_LOAD_STATUS           0x048

/* CSR_RESET Flags */
#define CSR_RESET_REG_FLAG_SW_RESET     0x00000080
#define CSR_RESET_REG_FLAG_MASTER_DIS   0x00000100
#define CSR_RESET_REG_FLAG_STOP_MASTER  0x00000200

/* CSR_GP_CNTRL Flags */
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY 0x00000001
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE       0x00000004
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ  0x00000008
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_ENA  0x00000001

/* Flow Handler (FH) DMA Registers */
#define FH_TCSR_CHNL_TX_CONFIG_REG(c)   (0x1D00 + ((c) * 0x20))
#define FH_TSSR_TX_STATUS_REG           0x1D10
#define FH_TX_BUF_ADDR(c)               (0x1D08 + ((c) * 0x20))
#define FH_TX_BUF_SIZE(c)               (0x1D0C + ((c) * 0x20))
#define FH_RSCSR_CHNL0_WPTR             0x1BC0
#define FH_RSCSR_CHNL0_RBDCB_WPTR       0x1BC8

/* Firmware Microcode TLV format */
#define IWL_UCODE_TLV_MAGIC             0x0a4c5749  /* "IWL\n" */
#define IWL_UCODE_TLV_SEC_RT            4
#define IWL_UCODE_TLV_SEC_INIT          5
#define IWL_UCODE_TLV_PAGING            6
#define IWL_UCODE_TLV_FW_VERSION        17

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
