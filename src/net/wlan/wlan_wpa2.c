#include "wlan_wpa2.h"
#include <string.h>

#ifndef WLAN_HOST_TEST
#include "syscall_abi.h"
#endif

/* RSN Information Element template used in Message 2 (22 bytes) */
static const uint8_t s_sta_rsn_ie[22] = {
    0x30, 0x14,                         /* ID 48 (RSN), Length 20 */
    0x01, 0x00,                         /* Version 1 */
    0x00, 0x0F, 0xAC, 0x04,             /* Group: AES-CCMP */
    0x01, 0x00,                         /* Pairwise Count 1 */
    0x00, 0x0F, 0xAC, 0x04,             /* Pairwise: AES-CCMP */
    0x01, 0x00,                         /* AKM Count 1 */
    0x00, 0x0F, 0xAC, 0x02,             /* AKM: PSK */
    0x00, 0x00                          /* Capabilities */
};

/* Endianness conversion helpers */
static inline uint16_t wlan_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t wlan_ntohs(uint16_t v) {
    return wlan_htons(v);
}

static inline uint64_t wlan_ntoh64(uint64_t v) {
    return __builtin_bswap64(v);
}

static inline uint64_t wlan_hton64(uint64_t v) {
    return __builtin_bswap64(v);
}

void wpa2_sm_init(wpa2_sm_t *sm,
                  const uint8_t sta_mac[6],
                  const uint8_t bssid[6],
                  const uint8_t pmk[WLAN_PMK_LEN]) {
    if (!sm) return;
    memset(sm, 0, sizeof(wpa2_sm_t));
    if (sta_mac) memcpy(sm->sta_mac, sta_mac, 6);
    if (bssid) memcpy(sm->bssid, bssid, 6);
    if (pmk) memcpy(sm->pmk, pmk, WLAN_PMK_LEN);
    sm->active = true;
    sm->port_authorized = false;
    sm->handshake_step = 0;
    sm->tx_pn = 0;
    sm->rx_pn = 0;
}

bool wpa2_sm_is_authorized(const wpa2_sm_t *sm) {
    return sm && sm->active && sm->port_authorized && sm->handshake_step == 4;
}

/* Pseudo-random generation for SNonce */
static void wlan_generate_snonce(uint8_t snonce[32]) {
    static uint32_t seed = 0x12345678;
    for (int i = 0; i < 32; i++) {
        seed = seed * 1664525u + 1013904223u;
        snonce[i] = (uint8_t)(seed >> 16);
    }
}

int wpa2_sm_rx_eapol(wpa2_sm_t *sm,
                     const uint8_t *eapol_frame, size_t len,
                     uint8_t *out_resp, size_t max_resp_len) {
    if (!sm || !sm->active || !eapol_frame || !out_resp) return -1;

    size_t min_len = sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
    if (len < min_len) return -1;

    const eapol_hdr_t *hdr = (const eapol_hdr_t *)eapol_frame;
    if (hdr->type != EAPOL_TYPE_KEY) return -1;

    const eapol_key_desc_t *kd = (const eapol_key_desc_t *)(eapol_frame + sizeof(eapol_hdr_t));
    if (kd->desc_type != EAPOL_KEY_DESC_WPA2) return -1;

    uint16_t key_info = wlan_ntohs(kd->key_info);
    if ((key_info & EAPOL_KEY_INFO_TYPE_MASK) != EAPOL_KEY_INFO_HMAC_SHA1) {
        return -1;
    }
    if (!(key_info & EAPOL_KEY_INFO_KEY_TYPE)) {
        return -1; /* Pairwise keys only */
    }

    uint64_t replay = wlan_ntoh64(kd->replay_counter);

    /* -------------------------------------------------------------------------
     * Message 1: KeyACK = 1, KeyMIC = 0
     * ------------------------------------------------------------------------- */
    if (!(key_info & EAPOL_KEY_INFO_KEY_MIC) && (key_info & EAPOL_KEY_INFO_KEY_ACK)) {
        /* Record ANonce and Replay Counter */
        memcpy(sm->anonce, kd->key_nonce, 32);
        sm->last_replay_counter = replay;

        /* Generate SNonce */
        wlan_generate_snonce(sm->snonce);

        /* Derive PTK (KCK | KEK | TK) */
        wlan_derive_ptk(sm->pmk, sm->bssid, sm->sta_mac, sm->anonce, sm->snonce, sm->ptk);
        memcpy(sm->kck, sm->ptk + 0, WLAN_KCK_LEN);
        memcpy(sm->kek, sm->ptk + 16, WLAN_KEK_LEN);
        memcpy(sm->tk,  sm->ptk + 32, WLAN_TK_LEN);

        /* Construct Message 2 */
        size_t rsn_len = sizeof(s_sta_rsn_ie);
        size_t resp_len = sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t) + rsn_len;
        if (max_resp_len < resp_len) return -1;

        memset(out_resp, 0, resp_len);
        eapol_hdr_t *resp_hdr = (eapol_hdr_t *)out_resp;
        resp_hdr->version = EAPOL_VERSION_1;
        resp_hdr->type = EAPOL_TYPE_KEY;
        resp_hdr->length = wlan_htons((uint16_t)(sizeof(eapol_key_desc_t) + rsn_len));

        eapol_key_desc_t *resp_kd = (eapol_key_desc_t *)(out_resp + sizeof(eapol_hdr_t));
        resp_kd->desc_type = EAPOL_KEY_DESC_WPA2;
        /* Key Info: HMAC-SHA1 | Pairwise | KeyMIC (0x010A) */
        resp_kd->key_info = wlan_htons(EAPOL_KEY_INFO_HMAC_SHA1 |
                                       EAPOL_KEY_INFO_KEY_TYPE |
                                       EAPOL_KEY_INFO_KEY_MIC);
        resp_kd->key_length = 0;
        resp_kd->replay_counter = kd->replay_counter; /* Echo replay counter */
        memcpy(resp_kd->key_nonce, sm->snonce, 32);
        resp_kd->key_data_len = wlan_htons((uint16_t)rsn_len);

        /* Append Station RSN IE into Key Data */
        memcpy(out_resp + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t), s_sta_rsn_ie, rsn_len);

        /* Compute HMAC-SHA1 MIC over entire EAPOL frame (with key_mic zeroed) */
        uint8_t full_mic[WLAN_SHA1_DIGEST_SIZE];
        wlan_hmac_sha1(sm->kck, WLAN_KCK_LEN, out_resp, resp_len, full_mic);
        memcpy(resp_kd->key_mic, full_mic, 16);

        sm->handshake_step = 2;
        return (int)resp_len;
    }

    /* -------------------------------------------------------------------------
     * Message 3: KeyACK = 1, KeyMIC = 1, Install = 1
     * ------------------------------------------------------------------------- */
    if ((key_info & EAPOL_KEY_INFO_KEY_MIC) && (key_info & EAPOL_KEY_INFO_KEY_ACK)) {
        if (sm->handshake_step < 2) return -1;
        if (replay < sm->last_replay_counter) return -1; /* Replay attack */

        /* Verify Message 3 MIC */
        size_t eapol_total_len = sizeof(eapol_hdr_t) + wlan_ntohs(hdr->length);
        if (eapol_total_len < min_len || eapol_total_len > len) {
            eapol_total_len = len;
        }
        uint8_t frame_copy[512];
        if (eapol_total_len > sizeof(frame_copy)) return -1;
        memcpy(frame_copy, eapol_frame, eapol_total_len);

        eapol_key_desc_t *copy_kd = (eapol_key_desc_t *)(frame_copy + sizeof(eapol_hdr_t));
        uint8_t rx_mic[16];
        memcpy(rx_mic, copy_kd->key_mic, 16);
        memset(copy_kd->key_mic, 0, 16);

        uint8_t computed_mic[WLAN_SHA1_DIGEST_SIZE];
        wlan_hmac_sha1(sm->kck, WLAN_KCK_LEN, frame_copy, eapol_total_len, computed_mic);

        /* Constant-time comparison */
        uint8_t diff = 0;
        for (int i = 0; i < 16; i++) {
            diff |= (rx_mic[i] ^ computed_mic[i]);
        }
        if (diff != 0) {
            return -1; /* MIC verification failure */
        }

        sm->last_replay_counter = replay;

        /* Unpack GTK if present */
        uint16_t key_data_len = wlan_ntohs(kd->key_data_len);
        if (key_data_len > 0 && len >= min_len + key_data_len) {
            const uint8_t *key_data = eapol_frame + min_len;
            uint8_t plain_data[256];

            if (key_info & EAPOL_KEY_INFO_ENCR_KEY_DATA) {
                if (key_data_len >= 16 && (key_data_len % 8 == 0)) {
                    if (wlan_aes_unwrap(sm->kek, key_data, key_data_len, plain_data) == 0) {
                        /* Search for GTK KDE (OUI 00-0F-AC, DataType 1) */
                        size_t off = 0;
                        while (off + 6 <= (size_t)(key_data_len - 8)) {
                            if (plain_data[off + 0] == 0xDD && /* Vendor Specific */
                                plain_data[off + 2] == 0x00 &&
                                plain_data[off + 3] == 0x0F &&
                                plain_data[off + 4] == 0xAC &&
                                plain_data[off + 5] == 0x01) { /* GTK KDE */
                                uint8_t kde_len = plain_data[off + 1];
                                if (kde_len >= 6 + 16) {
                                    sm->gtk_idx = plain_data[off + 6] & 0x03;
                                    memcpy(sm->gtk, &plain_data[off + 8], 16);
                                }
                                break;
                            }
                            off += 2 + plain_data[off + 1];
                        }
                    }
                }
            }
        }

        /* Construct Message 4 */
        size_t resp_len = sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
        if (max_resp_len < resp_len) return -1;

        memset(out_resp, 0, resp_len);
        eapol_hdr_t *resp_hdr = (eapol_hdr_t *)out_resp;
        resp_hdr->version = EAPOL_VERSION_1;
        resp_hdr->type = EAPOL_TYPE_KEY;
        resp_hdr->length = wlan_htons((uint16_t)sizeof(eapol_key_desc_t));

        eapol_key_desc_t *resp_kd = (eapol_key_desc_t *)(out_resp + sizeof(eapol_hdr_t));
        resp_kd->desc_type = EAPOL_KEY_DESC_WPA2;
        resp_kd->key_info = wlan_htons(EAPOL_KEY_INFO_HMAC_SHA1 |
                                       EAPOL_KEY_INFO_KEY_TYPE |
                                       EAPOL_KEY_INFO_KEY_MIC |
                                       (key_info & EAPOL_KEY_INFO_SECURE));
        resp_kd->key_length = 0;
        resp_kd->replay_counter = kd->replay_counter;
        resp_kd->key_data_len = 0;

        /* Compute MIC over Message 4 */
        uint8_t full_mic[WLAN_SHA1_DIGEST_SIZE];
        wlan_hmac_sha1(sm->kck, WLAN_KCK_LEN, out_resp, resp_len, full_mic);
        memcpy(resp_kd->key_mic, full_mic, 16);

        sm->port_authorized = true;
        sm->handshake_step = 4;
        return (int)resp_len;
    }

    return -1;
}
