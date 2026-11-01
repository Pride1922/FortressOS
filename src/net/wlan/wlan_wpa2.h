#ifndef FORTRESS_WLAN_WPA2_H
#define FORTRESS_WLAN_WPA2_H

#include "wlan_crypto.h"
#include "net_80211.h"

#define EAPOL_ETHER_TYPE            0x888Eu
#define EAPOL_VERSION_1             1u
#define EAPOL_VERSION_2             2u
#define EAPOL_TYPE_KEY              3u

#define EAPOL_KEY_DESC_WPA2         2u
#define WLAN_GTK_MAX_LEN            32u

/* Key Information Bit Flags */
#define EAPOL_KEY_INFO_TYPE_MASK    0x0007u
#define EAPOL_KEY_INFO_TYPE_AKM     0x0000u /* IEEE 802.11-2020 AKM-defined (WPA3-SAE) */
#define EAPOL_KEY_INFO_HMAC_SHA1    0x0002u /* WPA2 (HMAC-SHA1-128 + AES-128) */
#define EAPOL_KEY_INFO_AES_128_CMAC 0x0003u /* 802.11w / SHA-256 AKM */
#define EAPOL_KEY_INFO_KEY_TYPE     0x0008u /* 1 = Pairwise, 0 = Group */
#define EAPOL_KEY_INFO_INSTALL      0x0040u /* Install PTK into hardware/engine */
#define EAPOL_KEY_INFO_KEY_ACK      0x0080u /* Set by AP to request ACK */
#define EAPOL_KEY_INFO_KEY_MIC      0x0100u /* MIC is present */
#define EAPOL_KEY_INFO_SECURE       0x0200u /* Secure flag */
#define EAPOL_KEY_INFO_ERROR        0x0400u
#define EAPOL_KEY_INFO_REQUEST      0x0800u
#define EAPOL_KEY_INFO_ENCR_KEY_DATA 0x1000u /* Key Data is encrypted with KEK */

/* EAPOL Header (4 bytes) */
typedef struct {
    uint8_t  version;       /* 1 or 2 */
    uint8_t  type;          /* 3 = Key */
    uint16_t length;        /* Big-endian payload length */
} __attribute__((packed)) eapol_hdr_t;

/* EAPOL-Key Descriptor (95 bytes fixed header) */
typedef struct {
    uint8_t  desc_type;     /* 2 = RSN / WPA2 */
    uint16_t key_info;      /* Flags (big-endian) */
    uint16_t key_length;    /* 16 for TK (big-endian) */
    uint64_t replay_counter;/* Monotonic replay counter (big-endian) */
    uint8_t  key_nonce[32]; /* ANonce / SNonce */
    uint8_t  key_iv[16];
    uint64_t key_rsc;
    uint8_t  reserved[8];
    uint8_t  key_mic[16];   /* HMAC-SHA1-128 MIC */
    uint16_t key_data_len;  /* Big-endian */
} __attribute__((packed)) eapol_key_desc_t;

/* WPA2 Handshake Context */
typedef struct {
    bool     active;
    bool     port_authorized;
    uint8_t  sta_mac[6];
    uint8_t  bssid[6];
    uint8_t  pmk[WLAN_PMK_LEN];

    /* Generated Nonces */
    uint8_t  anonce[32];
    uint8_t  snonce[32];

    /* Derived Temporal Keys */
    uint8_t  ptk[WLAN_PTK_LEN];
    uint8_t  kck[WLAN_KCK_LEN]; /* bytes 0..15 */
    uint8_t  kek[WLAN_KEK_LEN]; /* bytes 16..31 */
    uint8_t  tk[WLAN_TK_LEN];   /* bytes 32..47 */
    uint8_t  gtk[WLAN_GTK_MAX_LEN]; /* Group Temporal Key (16B CCMP or 32B TKIP) */
    uint8_t  gtk_len;
    uint8_t  gtk_idx;
    uint8_t  group_cipher;      /* Negotiated Group Cipher: 0x02 (TKIP) or 0x04 (CCMP) */
    uint16_t rsn_caps;          /* Negotiated RSN capabilities (mirrored in M2 Key Data) */
    bool     use_sae;           /* True when AKM is WPA3-SAE (00-0F-AC:8) */
    uint8_t  pmkid[WLAN_PMKID_LEN];

    uint64_t last_replay_counter;
    uint32_t handshake_step;    /* 0=Idle, 1=Got M1, 2=Sent M2, 3=Got M3, 4=Authorized */

    /* Packet Number counters for CCMP (per-TID 0..15 replay protection) */
    uint64_t tx_pn;
    uint64_t rx_pn[16];
    uint64_t rx_bitmap[16];
} wpa2_sm_t;

/* Initialize WPA2 Station State Machine */
void wpa2_sm_init(wpa2_sm_t *sm,
                  const uint8_t sta_mac[6],
                  const uint8_t bssid[6],
                  const uint8_t pmk[WLAN_PMK_LEN]);

/* Set negotiated group cipher suite (0x02 for TKIP, 0x04 for CCMP) */
void wpa2_sm_set_group_cipher(wpa2_sm_t *sm, uint8_t group_cipher);

/* Set negotiated group cipher suite and RSN capabilities (mirrored in Message 2) */
void wpa2_sm_set_rsn(wpa2_sm_t *sm, uint8_t group_cipher, uint16_t rsn_caps);

/* Enable WPA3-SAE AKM mode (KDF-SHA-256 + HMAC-SHA-256-128 MIC + PMKID in M2 RSN IE) */
void wpa2_sm_set_sae(wpa2_sm_t *sm, bool use_sae, const uint8_t pmkid[WLAN_PMKID_LEN]);

/* Process incoming EAPOL frame (Message 1 or Message 3) */
int wpa2_sm_rx_eapol(wpa2_sm_t *sm,
                     const uint8_t *eapol_frame, size_t len,
                     uint8_t *out_resp, size_t max_resp_len);

/* Check if WPA2 link is secure and authorized for data traffic */
bool wpa2_sm_is_authorized(const wpa2_sm_t *sm);

#endif /* FORTRESS_WLAN_WPA2_H */
