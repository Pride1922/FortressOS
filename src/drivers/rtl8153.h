#ifndef FORTRESS_RTL8153_H
#define FORTRESS_RTL8153_H

#include "types.h"
#include "net.h"
#include "xhci_rings.h"
#include "xhci_dev.h"

/* Vendor and Product IDs */
#define REALTEK_VID             0x0BDA
#define DELL_OEM_VID            0x413C

#define RTL8153_PID             0x8153
#define RTL8152_PID             0x8152
#define RTL8151_PID             0x8151
#define RTL8156_PID             0x8156
#define DELL_WD19S_PID1         0xB06E
#define DELL_WD19S_PID2         0xB06F

/* Control Request Constants */
#define RTL8153_REQ_GET_REGS    0x05
#define RTL8153_REQ_SET_REGS    0x05

#define MCU_TYPE_PLA            0x0100
#define MCU_TYPE_USB            0x0000

#define BYTE_EN_DWORD           0xFF
#define BYTE_EN_WORD            0x33
#define BYTE_EN_BYTE            0x11
#define BYTE_EN_SIX_BYTES       0x3F

/* PLA (Physical Layer Accelerator) Registers */
#define PLA_IDR                 0xC000  /* 6 bytes: MAC Address */
#define PLA_RCR                 0xC010  /* 4 bytes: Receive Configuration Register */
#define PLA_RMS                 0xC016  /* 2 bytes: Receive Maximum Packet Size */
#define PLA_BACKUP              0xD000  /* 6 bytes: Backup MAC Address */
#define PLA_OOB_CTRL            0xD390  /* 1 byte: Out-Of-Band Control */
#define PLA_CR                  0xE813  /* 1 byte: Chip Command Register */
#define PLA_CRWECR              0xE81C  /* 1 byte: Config Write Enable */
#define PLA_PHYSTATUS           0xE908  /* 2 bytes: PHY Status & Link Speed */

/* USB Registers */
#define USB_USB_CTRL            0xD406  /* 2 bytes: USB Control */

/* PLA_CR Register Bits */
#define RTL8153_CR_RST          0x10    /* Soft reset (self-clearing) */
#define RTL8153_CR_RE           0x08    /* Receiver Enable */
#define RTL8153_CR_TE           0x04    /* Transmitter Enable */

/* PLA_RCR Register Bits */
#define RTL8153_RCR_AAP         0x00000001u /* Accept All Packets (promiscuous) */
#define RTL8153_RCR_APM         0x00000002u /* Accept Physical Match */
#define RTL8153_RCR_AM          0x00000004u /* Accept Multicast */
#define RTL8153_RCR_AB          0x00000008u /* Accept Broadcast */

/* PLA_PHYSTATUS Register Bits */
#define RTL8153_PHY_FULL_DUP    0x0001u /* Full Duplex */
#define RTL8153_PHY_LINK_STATUS 0x0002u /* Link Up */
#define RTL8153_PHY_10M         0x0004u /* 10 Mbps */
#define RTL8153_PHY_100M        0x0008u /* 100 Mbps */
#define RTL8153_PHY_1000M       0x0010u /* 1000 Mbps Gigabit */

/* USB_USB_CTRL Bits */
#define RTL8153_RX_AGG_DISABLE  0x0010u /* Disable HW aggregation for low latency */
#define RTL8153_RX_ZERO_EN      0x0080u /* Zero length packet enable */

/* Transmit Descriptor (8 bytes prepended to Bulk OUT payload) */
typedef struct {
    uint32_t opts1;
    uint32_t opts2;
} __attribute__((packed)) rtl8153_tx_desc_t;

#define RTL8153_TX_FS           (1u << 31)  /* First segment of packet */
#define RTL8153_TX_LS           (1u << 30)  /* Final segment of packet */
#define RTL8153_TX_LEN_MASK     0x0003FFFFu

/* Receive Descriptor (24 bytes prepended to Bulk IN packets) */
typedef struct {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t opts3;
    uint32_t opts4;
    uint32_t opts5;
    uint32_t opts6;
} __attribute__((packed)) rtl8153_rx_desc_t;

#define RTL8153_RX_LEN_MASK     0x00007FFFu

/* Maximum packet buffer capacity */
#define RTL8153_MAX_FRAME_SIZE  1514u
#define RTL8153_RX_POOL_SIZE    8u
#define RTL8153_RX_BUF_SIZE     4096u

/* Driver API */
bool rtl8153_configure_device(const xhci_rings_io_t *io,
                              xhci_dma_buffers_t *ring_dma,
                              xhci_dev_dma_t *dev_dma,
                              xhci_bot_device_t *device);

net_dev_t *rtl8153_get_net_device(void);

int rtl8153_send_packet(net_dev_t *dev, const void *buf, size_t len);
pbuf_t *rtl8153_poll_rx(net_dev_t *dev);
void rtl8153_recycle_rx(net_dev_t *dev, pbuf_t *packet);
bool rtl8153_service_link(net_dev_t *dev, uint64_t now, uint64_t tick_hz);
bool rtl8153_network_online(net_dev_t *dev);
uint32_t rtl8153_link_state_abi(const net_dev_t *dev);
void rtl8153_get_stats(const net_dev_t *dev, uint64_t *rx_packets, uint64_t *tx_packets);

/* Helper for host testing and OCP register manipulation */
bool rtl8153_is_supported_device(uint16_t vid, uint16_t pid);
#ifdef RTL8153_HOST_TEST
uint8_t *rtl8153_test_get_rx_buf(void);
#endif

#endif /* FORTRESS_RTL8153_H */
