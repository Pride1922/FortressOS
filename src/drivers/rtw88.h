#ifndef FORTRESS_RTW88_H
#define FORTRESS_RTW88_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "pci.h"
#include "net.h"
#include "../net/wlan/wlan_mgr.h"

/* Realtek Semiconductor Corp. PCIe Wireless IDs */
#define RTW_PCI_VENDOR_ID               0x10EC
#define RTW_DEV_ID_8821CE               0xC821  /* RTL8821CE 802.11ac 1x1 */
#define RTW_DEV_ID_8822BE               0xB822  /* RTL8822BE 802.11ac 2x2 */
#define RTW_DEV_ID_8822CE               0xC822  /* RTL8822CE 802.11ac 2x2 */
#define RTW_DEV_ID_8852AE               0x8852  /* RTL8852AE 802.11ax 2x2 */
#define RTW_DEV_ID_8852BE               0xB852  /* RTL8852BE 802.11ax 2x2 */

/* Qualcomm Atheros PCIe Wireless IDs */
#define ATH_PCI_VENDOR_ID               0x168C
#define ATH_DEV_ID_QCA6174              0x003E  /* QCA6174 802.11ac 2x2 */
#define ATH_DEV_ID_QCA9377              0x0042  /* QCA9377 802.11ac 1x1 */

/* Realtek MAC / EFUSE Registers */
#define RTW_REG_SYS_FUNC_EN             0x0002
#define RTW_REG_APS_FSMCO               0x0004
#define RTW_REG_CR                      0x0100
#define RTW_REG_MACID                   0x0610
#define RTW_REG_RCR                     0x0608
#define RTW_REG_TCR                     0x0604

#define RTW_TX_RING_SIZE                32
#define RTW_RX_RING_SIZE                32

/* Hardware TX Packet Descriptor (TXPKTBUF - 48 bytes) */
typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4;
    uint32_t dw5;
    uint32_t dw6;
    uint32_t dw7;
    uint32_t buf_addr_low;
    uint32_t buf_addr_high;
    uint32_t next_desc_low;
    uint32_t next_desc_high;
} __attribute__((packed)) rtw_tx_desc_t;

_Static_assert(sizeof(rtw_tx_desc_t) == 48, "rtw_tx_desc_t must be 48 bytes");

typedef struct {
    bool         detected;
    bool         initialized;
    bool         firmware_loaded;
    uint16_t     vendor_id;
    uint16_t     device_id;
    pci_device_t pci;
    uintptr_t    mmio_base;
    uint8_t      mac_addr[6];
    uint8_t      current_channel;
    bool         ht_enabled;
    bool         vht_enabled;
    uint8_t      chan_width;
    uint32_t     tx_head;
    uint32_t     tx_tail;
    uint32_t     rx_head;
    uint32_t     rx_tail;
    spinlock_t   lock;
} rtw88_state_t;

bool rtw88_probe(void);
bool rtw88_is_supported_device(uint16_t vendor, uint16_t device);
bool rtw88_is_detected(void);
bool rtw88_init_hw(void);
bool rtw88_load_firmware(const void *fw_data, size_t fw_len);
int  rtw88_get_mac(uint8_t mac[6]);
int  rtw88_set_channel(uint8_t channel);
int  rtw88_start_scan(const char *ssid);
int  rtw88_prepare_auth(uint8_t channel, const uint8_t bssid[6], uint16_t beacon_interval);
int  rtw88_set_associated(uint16_t assoc_id, uint16_t beacon_interval);
int  rtw88_deauth(void);
int  rtw88_configure_ht_vht(bool ht_en, bool vht_en, uint8_t chan_width, uint8_t rx_nss);
int  rtw88_add_ba_session(uint8_t tid, uint16_t ssn, uint16_t buf_size);
int  rtw88_send_raw(const void *buf, size_t len);
pbuf_t *rtw88_poll_raw(void);
void rtw88_recycle_raw(pbuf_t *p);
bool rtw88_is_scan_complete(void);
const wlan_mac_ops_t *rtw88_get_mac_ops(void);

#endif /* FORTRESS_RTW88_H */
