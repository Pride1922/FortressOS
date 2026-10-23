#ifndef FORTRESS_WLAN_MGR_H
#define FORTRESS_WLAN_MGR_H

#include "net_80211.h"
#include "wlan_crypto.h"
#include "wlan_wpa2.h"
#include "wifi_abi.h"
#include "netctl_abi.h"
#include "net.h"

/* Low-level hardware operations implemented by NIC driver (iwlwifi, USB dongle, etc.) */
typedef struct wlan_hw_ops {
    const char *name;
    int    (*get_mac)(uint8_t mac[6]);
    int    (*set_channel)(uint8_t channel);
    int    (*start_scan)(const char *ssid);
    int    (*send_raw_80211)(const void *buf, size_t len);
    pbuf_t *(*poll_raw_80211)(void);
    void   (*recycle_raw_80211)(pbuf_t *packet);
} wlan_hw_ops_t;

/* Wi-Fi Manager Interface */
void wlan_mgr_init(const wlan_hw_ops_t *ops);
net_dev_t *wlan_get_net_device(void);

/* Management & Syscall Handlers (SYS_WIFI) */
int wlan_mgr_scan(wifi_scan_result_t *out_result);
int wlan_mgr_connect(const char *ssid, const char *passphrase, const uint8_t bssid[6]);
int wlan_mgr_disconnect(void);
int wlan_mgr_status(wifi_status_t *out_status);

/* Core Rx Packet Pipeline (Processes raw 802.11 frame from radio) */
int wlan_mgr_rx_raw_80211(const uint8_t *frame, size_t len, int8_t rssi);

/* net_dev_t Callbacks */
int      wlan_send_packet(net_dev_t *dev, const void *buf, size_t len);
pbuf_t  *wlan_poll_rx(net_dev_t *dev);
void     wlan_recycle_rx(net_dev_t *dev, pbuf_t *packet);
bool     wlan_service_link(net_dev_t *dev, uint64_t now, uint64_t tick_hz);
bool     wlan_network_online(net_dev_t *dev);
uint32_t wlan_link_state_abi(const net_dev_t *dev);
void     wlan_get_stats(const net_dev_t *dev, uint64_t *rx_packets, uint64_t *tx_packets);

#endif /* FORTRESS_WLAN_MGR_H */
