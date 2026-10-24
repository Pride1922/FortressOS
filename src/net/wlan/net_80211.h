#ifndef FORTRESS_NET_80211_H
#define FORTRESS_NET_80211_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IEEE80211_ADDR_LEN          6
#define IEEE80211_SSID_MAX_LEN      32
#define IEEE80211_MAX_BSS           32

/* 802.11 Frame Types */
#define IEEE80211_TYPE_MGMT         0x00
#define IEEE80211_TYPE_CTRL         0x01
#define IEEE80211_TYPE_DATA         0x02

/* 802.11 Management Subtypes */
#define IEEE80211_SUBTYPE_ASSOC_REQ 0x00
#define IEEE80211_SUBTYPE_ASSOC_RESP 0x01
#define IEEE80211_SUBTYPE_PROBE_REQ 0x04
#define IEEE80211_SUBTYPE_PROBE_RESP 0x05
#define IEEE80211_SUBTYPE_BEACON    0x08
#define IEEE80211_SUBTYPE_DISASSOC  0x0A
#define IEEE80211_SUBTYPE_AUTH      0x0B
#define IEEE80211_SUBTYPE_DEAUTH    0x0C

/* 802.11 Data Subtypes */
#define IEEE80211_SUBTYPE_DATA      0x00
#define IEEE80211_SUBTYPE_QOS_DATA  0x08

/* 802.11 Frame Control Flags */
#define IEEE80211_FCTL_TODS         0x0100
#define IEEE80211_FCTL_FROMDS       0x0200
#define IEEE80211_FCTL_MOREFRAGS    0x0400
#define IEEE80211_FCTL_RETRY        0x0800
#define IEEE80211_FCTL_PWRMGT       0x1000
#define IEEE80211_FCTL_MOREDATA     0x2000
#define IEEE80211_FCTL_PROTECTED    0x4000
#define IEEE80211_FCTL_ORDER        0x8000

/* 802.11 Information Element IDs */
#define IEEE80211_IE_SSID           0
#define IEEE80211_IE_RATES          1
#define IEEE80211_IE_DS_PARAM       3
#define IEEE80211_IE_RSN            48
#define IEEE80211_IE_EXT_RATES      50
#define IEEE80211_IE_VENDOR         221

/* 802.11 Standard MAC Header (24 bytes) */
typedef struct {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t  addr1[IEEE80211_ADDR_LEN]; /* Receiver / DA */
    uint8_t  addr2[IEEE80211_ADDR_LEN]; /* Transmitter / SA */
    uint8_t  addr3[IEEE80211_ADDR_LEN]; /* BSSID / Filter */
    uint16_t seq_ctrl;
} __attribute__((packed)) ieee80211_hdr_t;

/* 802.11 QoS Data Header (26 bytes) */
typedef struct {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t  addr1[IEEE80211_ADDR_LEN];
    uint8_t  addr2[IEEE80211_ADDR_LEN];
    uint8_t  addr3[IEEE80211_ADDR_LEN];
    uint16_t seq_ctrl;
    uint16_t qos_ctrl;
} __attribute__((packed)) ieee80211_qos_hdr_t;

/* LLC/SNAP Encapsulation Header (8 bytes) */
typedef struct {
    uint8_t  dsap;          /* 0xAA */
    uint8_t  ssap;          /* 0xAA */
    uint8_t  control;       /* 0x03 */
    uint8_t  oui[3];        /* 0x00 0x00 0x00 (RFC 1042 encapsulation) */
    uint16_t ethertype;     /* Big-endian EtherType */
} __attribute__((packed)) llc_snap_hdr_t;

/* BSS (Access Point) Description */
typedef struct {
    uint8_t  bssid[IEEE80211_ADDR_LEN];
    char     ssid[IEEE80211_SSID_MAX_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    int8_t   rssi;
    uint16_t beacon_interval;
    uint16_t capabilities;
    bool     has_wpa2_psk;
    uint32_t last_seen_ticks;
} wlan_bss_entry_t;

typedef struct {
    wlan_bss_entry_t entries[IEEE80211_MAX_BSS];
    uint32_t         count;
} wlan_bss_table_t;

/* Station State Machine States */
typedef enum {
    WLAN_STATE_IDLE = 0,
    WLAN_STATE_SCANNING,
    WLAN_STATE_AUTHENTICATING,
    WLAN_STATE_ASSOCIATING,
    WLAN_STATE_4WAY_HANDSHAKE,
    WLAN_STATE_CONNECTED,
    WLAN_STATE_FAILED,
} wlan_state_t;

/* Frame Encoders */
size_t net_80211_build_probe_req(uint8_t *buf, size_t max_len,
                                 const uint8_t src_mac[6],
                                 const char *ssid);

size_t net_80211_build_auth_req(uint8_t *buf, size_t max_len,
                                const uint8_t src_mac[6],
                                const uint8_t bssid[6]);

size_t net_80211_build_assoc_req(uint8_t *buf, size_t max_len,
                                 const uint8_t src_mac[6],
                                 const uint8_t bssid[6],
                                 const char *ssid);

size_t net_80211_build_deauth(uint8_t *buf, size_t max_len,
                             const uint8_t src_mac[6],
                             const uint8_t bssid[6],
                             uint16_t reason);

/* Bridging Helpers: 802.3 Ethernet <--> 802.11 LLC/SNAP */
int net_80211_encapsulate(const uint8_t eth_frame[], size_t eth_len,
                          const uint8_t bssid[6], const uint8_t src_mac[6],
                          uint16_t seq_num,
                          uint8_t wlan_frame[], size_t max_wlan_len);

int net_80211_decapsulate(const uint8_t wlan_frame[], size_t wlan_len,
                          uint8_t eth_frame[], size_t max_eth_len);

/* Beacon & Probe Response Parser */
bool net_80211_parse_beacon_or_probe(const uint8_t *frame, size_t len,
                                     int8_t rssi,
                                     wlan_bss_entry_t *out_bss);

/* BSS Table Helpers */
void wlan_bss_table_init(wlan_bss_table_t *table);
bool wlan_bss_table_update(wlan_bss_table_t *table, const wlan_bss_entry_t *bss);
const wlan_bss_entry_t *wlan_bss_table_find(const wlan_bss_table_t *table, const char *ssid);

#endif /* FORTRESS_NET_80211_H */
