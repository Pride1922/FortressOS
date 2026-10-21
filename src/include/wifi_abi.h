#ifndef FORTRESS_WIFI_ABI_H
#define FORTRESS_WIFI_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SYS_WIFI_CMD_SCAN          1u
#define WIFI_CMD_SCAN              SYS_WIFI_CMD_SCAN

#define SYS_WIFI_CMD_CONNECT       2u
#define WIFI_CMD_CONNECT           SYS_WIFI_CMD_CONNECT

#define SYS_WIFI_CMD_DISCONNECT    3u
#define WIFI_CMD_DISCONNECT        SYS_WIFI_CMD_DISCONNECT

#define SYS_WIFI_CMD_STATUS        4u
#define WIFI_CMD_STATUS            SYS_WIFI_CMD_STATUS

#define SYS_WIFI_CMD_LOAD_FW       5u
#define WIFI_CMD_LOAD_FW           SYS_WIFI_CMD_LOAD_FW

#define WIFI_MAX_SCAN_ENTRIES      16u
#define WIFI_SSID_MAX_LEN          32u
#define WIFI_PASSPHRASE_MAX_LEN    64u

/* Wireless Link / Connection State */
#define WIFI_STATUS_IDLE           0u
#define WIFI_STATUS_SCANNING       1u
#define WIFI_STATUS_AUTHENTICATING 2u
#define WIFI_STATUS_ASSOCIATING    3u
#define WIFI_STATUS_HANDSHAKE      4u
#define WIFI_STATUS_CONNECTED      5u
#define WIFI_STATUS_FAILED         6u

/* Security Type */
#define WIFI_SEC_OPEN              0u
#define WIFI_SEC_WPA2_PSK          1u

/* Scan entry returned to userland (48 bytes) */
typedef struct {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  ssid_len;
    uint8_t  security;      /* WIFI_SEC_* */
    uint16_t reserved;
    char     ssid[WIFI_SSID_MAX_LEN + 1]; /* 33 bytes null-terminated */
    uint8_t  pad[3];        /* align to 48 bytes */
} wifi_scan_entry_t;

_Static_assert(sizeof(wifi_scan_entry_t) == 48, "wifi_scan_entry_t must be 48 bytes");

/* Scan results buffer */
typedef struct {
    uint32_t count;         /* Count of populated entries */
    uint32_t max_entries;   /* Capacity of entries array */
    wifi_scan_entry_t entries[WIFI_MAX_SCAN_ENTRIES];
} wifi_scan_result_t;

/* Connect request */
typedef struct {
    char     ssid[WIFI_SSID_MAX_LEN + 1];
    char     passphrase[WIFI_PASSPHRASE_MAX_LEN + 1];
    uint8_t  bssid[6];      /* All 0s for any matching SSID */
    uint8_t  reserved[6];
} wifi_connect_req_t;

/* Status query response */
typedef struct {
    uint32_t state;         /* WIFI_STATUS_* */
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  bssid[6];
    char     ssid[WIFI_SSID_MAX_LEN + 1];
    uint8_t  mac[6];        /* Local wlan0 MAC address */
    uint16_t link_online;   /* 1 if connected and authorized, 0 otherwise */
    uint32_t reserved;
} wifi_status_t;

#endif /* FORTRESS_WIFI_ABI_H */
