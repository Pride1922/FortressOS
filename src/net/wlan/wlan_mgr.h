#ifndef FORTRESS_WLAN_MGR_H
#define FORTRESS_WLAN_MGR_H

#include "net_80211.h"
#include "wlan_crypto.h"
#include "wlan_wpa2.h"
#include "wifi_abi.h"
#include "netctl_abi.h"
#include "net.h"

/* Hardware Capability Flags */
#define WLAN_HW_CAP_2GHZ        (1u << 0)
#define WLAN_HW_CAP_5GHZ        (1u << 1)
#define WLAN_HW_CAP_HT          (1u << 2) /* 802.11n HT20/HT40 */
#define WLAN_HW_CAP_VHT         (1u << 3) /* 802.11ac VHT80 */
#define WLAN_HW_CAP_SAE         (1u << 4) /* WPA3-Personal SAE */
#define WLAN_HW_CAP_TKIP_GROUP  (1u << 5) /* TKIP Group Cipher */

#define WLAN_MAX_DRIVERS        4u

/* Low-level MAC/PHY operations implemented by NIC driver (iwlwifi, rtw88, ath10k, etc.) */
typedef struct wlan_hw_ops {
    const char *name;
    int    (*get_mac)(uint8_t mac[6]);
    int    (*set_channel)(uint8_t channel);
    int    (*start_scan)(const char *ssid);
    int    (*prepare_auth)(uint8_t channel, const uint8_t bssid[6], uint16_t beacon_interval);
    int    (*set_associated)(uint16_t assoc_id, uint16_t beacon_interval);
    int    (*deauth)(void);
    int    (*send_raw_80211)(const void *buf, size_t len);
    pbuf_t *(*poll_raw_80211)(void);
    void   (*recycle_raw_80211)(pbuf_t *packet);
    bool   (*is_scan_complete)(void);
    /* Extended multi-vendor & HT/VHT hooks (optional; NULL-checked) */
    uint32_t caps;
    bool   (*is_detected)(void);
    bool   (*init_hw)(void);
    bool   (*load_firmware)(const void *fw_data, size_t fw_len);
    int    (*configure_ht_vht)(bool ht_en, bool vht_en, uint8_t chan_width, uint8_t rx_nss);
    int    (*add_ba_session)(uint8_t tid, uint16_t ssn, uint16_t buf_size);
} wlan_hw_ops_t;

typedef wlan_hw_ops_t wlan_mac_ops_t;

/* Multi-Vendor Driver Registration & Firmware Dispatch */
int wlan_register_driver(const wlan_mac_ops_t *ops);
int wlan_mgr_load_firmware(const void *fw_data, size_t fw_len);

/* Wi-Fi Manager Interface */
void wlan_mgr_init(const wlan_hw_ops_t *ops);
net_dev_t *wlan_get_net_device(void);

/* Management: registered CPU-0 owner only (boot init precedes registration).
 * SYS_WIFI callers publish copied requests through net_wifi_owner. Status is
 * a lock-protected snapshot: no lazy initialization, RX drain or driver calls. */
int wlan_mgr_scan(wifi_scan_result_t *out_result);
int wlan_mgr_connect(const char *ssid, const char *passphrase, const uint8_t bssid[6]);
int wlan_mgr_disconnect(void);
int wlan_mgr_status(wifi_status_t *out_status);
/* Owner thread only, unlocked context; prompt bounded RX turns during setup. */
bool wlan_mgr_work_pending(void);

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

/* Atomic snapshot only; callable on any CPU, no locks or radio access. */
size_t wlan_rx_profile_format(char *out, size_t cap);

#endif /* FORTRESS_WLAN_MGR_H */
