#include "net_80211.h"
#include <string.h>

/* ============================================================================
 * 1. Frame Builders
 * ============================================================================ */

size_t net_80211_build_probe_req(uint8_t *buf, size_t max_len,
                                 const uint8_t src_mac[6],
                                 const char *ssid) {
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len > IEEE80211_SSID_MAX_LEN) ssid_len = IEEE80211_SSID_MAX_LEN;

    size_t total_len = sizeof(ieee80211_hdr_t) + (2 + ssid_len) + (2 + 4);
    if (!buf || max_len < total_len) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    /* Frame Control: Type = MGMT (0), Subtype = PROBE_REQ (4) -> 0x0040 */
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_PROBE_REQ << 4);
    memset(hdr->addr1, 0xFF, 6); /* Broadcast */
    memcpy(hdr->addr2, src_mac, 6);
    memset(hdr->addr3, 0xFF, 6);

    uint8_t *ie = buf + sizeof(ieee80211_hdr_t);

    /* 1. SSID IE */
    *ie++ = IEEE80211_IE_SSID;
    *ie++ = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(ie, ssid, ssid_len);
        ie += ssid_len;
    }

    /* 2. Supported Rates IE: 1, 2, 5.5, 11 Mbps (basic rates) */
    *ie++ = IEEE80211_IE_RATES;
    *ie++ = 4;
    *ie++ = 0x82; /* 1 Mbps (basic) */
    *ie++ = 0x84; /* 2 Mbps (basic) */
    *ie++ = 0x8B; /* 5.5 Mbps (basic) */
    *ie++ = 0x96; /* 11 Mbps (basic) */

    return (size_t)(ie - buf);
}

size_t net_80211_build_auth_req(uint8_t *buf, size_t max_len,
                                const uint8_t src_mac[6],
                                const uint8_t bssid[6]) {
    size_t total_len = sizeof(ieee80211_hdr_t) + 6; /* 6 bytes fixed body */
    if (!buf || max_len < total_len) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    /* Frame Control: Type = MGMT, Subtype = AUTH (0x0B) -> 0x00B0 */
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_AUTH << 4);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, src_mac, 6);
    memcpy(hdr->addr3, bssid, 6);

    uint8_t *body = buf + sizeof(ieee80211_hdr_t);
    /* Auth Algorithm = Open System (0) */
    body[0] = 0x00; body[1] = 0x00;
    /* Auth Sequence = 1 */
    body[2] = 0x01; body[3] = 0x00;
    /* Status Code = 0 */
    body[4] = 0x00; body[5] = 0x00;

    return total_len;
}

size_t net_80211_build_sae_auth(uint8_t *buf, size_t max_len,
                                const uint8_t src_mac[6],
                                const uint8_t bssid[6],
                                uint16_t seq_num,
                                uint16_t status_code,
                                const uint8_t *sae_body, size_t sae_len) {
    size_t total_len = sizeof(ieee80211_hdr_t) + 6 + sae_len;
    if (!buf || !src_mac || !bssid || max_len < total_len) return 0;
    if (sae_len > 0 && !sae_body) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_AUTH << 4);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, src_mac, 6);
    memcpy(hdr->addr3, bssid, 6);

    uint8_t *body = buf + sizeof(ieee80211_hdr_t);
    /* Auth Algorithm = SAE (3) */
    body[0] = (uint8_t)(IEEE80211_AUTH_ALG_SAE & 0xFFu);
    body[1] = (uint8_t)(IEEE80211_AUTH_ALG_SAE >> 8);
    /* Auth Sequence = 1 (Commit) or 2 (Confirm) */
    body[2] = (uint8_t)(seq_num & 0xFFu);
    body[3] = (uint8_t)(seq_num >> 8);
    /* Status Code */
    body[4] = (uint8_t)(status_code & 0xFFu);
    body[5] = (uint8_t)(status_code >> 8);
    if (sae_len > 0) {
        memcpy(body + 6, sae_body, sae_len);
    }
    return total_len;
}

size_t net_80211_build_assoc_req_full(uint8_t *buf, size_t max_len,
                                      const uint8_t src_mac[6],
                                      const uint8_t bssid[6],
                                      const char *ssid,
                                      bool is_open,
                                      bool use_sae,
                                      uint8_t group_cipher,
                                      uint16_t rsn_caps,
                                      const uint8_t *pmkid,
                                      bool include_ht,
                                      bool include_vht) {
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    if (ssid_len > IEEE80211_SSID_MAX_LEN) ssid_len = IEEE80211_SSID_MAX_LEN;

    size_t rsn_ie_total = 0;
    if (!is_open) {
        rsn_ie_total = 22 + ((use_sae && pmkid) ? 18 : 0);
    }
    size_t ht_ie_total = include_ht ? 28 : 0;
    size_t vht_ie_total = include_vht ? 14 : 0;

    size_t total_len = sizeof(ieee80211_hdr_t) + 4 + (2 + ssid_len) + 10 + 6 +
                       rsn_ie_total + ht_ie_total + vht_ie_total + 9;
    if (!buf || max_len < total_len) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    /* Frame Control: Type = MGMT, Subtype = ASSOC_REQ (0) -> 0x0000 */
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_ASSOC_REQ << 4);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, src_mac, 6);
    memcpy(hdr->addr3, bssid, 6);

    uint8_t *body = buf + sizeof(ieee80211_hdr_t);

    /* Capabilities: ESS (0x0001) | Short Preamble (0x0020) | Short Slot (0x0400) [+ Privacy (0x0010) if secured] */
    uint16_t caps = is_open ? 0x0421u : 0x0431u;
    body[0] = (uint8_t)(caps & 0xFFu);
    body[1] = (uint8_t)(caps >> 8);
    /* Listen Interval: 10 */
    body[2] = 0x0A; body[3] = 0x00;

    uint8_t *ie = body + 4;

    /* 1. SSID IE */
    *ie++ = IEEE80211_IE_SSID;
    *ie++ = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(ie, ssid, ssid_len);
        ie += ssid_len;
    }

    /* 2. Supported Rates IE (1, 2, 5.5, 11, 6, 9, 12, 18 Mbps) */
    *ie++ = IEEE80211_IE_RATES;
    *ie++ = 8;
    *ie++ = 0x82; *ie++ = 0x84; *ie++ = 0x8B; *ie++ = 0x96;
    *ie++ = 0x8C; *ie++ = 0x12; *ie++ = 0x98; *ie++ = 0x24;

    /* 3. Extended Supported Rates IE (24, 36, 48, 54 Mbps) */
    *ie++ = IEEE80211_IE_EXT_RATES;
    *ie++ = 4;
    *ie++ = 0xB0; *ie++ = 0x48; *ie++ = 0x60; *ie++ = 0x6C;

    /* 4. RSN Information Element (omitted for open networks) */
    if (!is_open) {
        bool has_pmkid = (use_sae && pmkid != NULL);
        *ie++ = IEEE80211_IE_RSN;
        *ie++ = has_pmkid ? 38 : 20;
        *ie++ = 0x01; *ie++ = 0x00; /* Version 1 */
        /* Group Cipher: 00-0F-AC:<group_cipher> (0x02 for TKIP, 0x04 for CCMP) */
        *ie++ = 0x00; *ie++ = 0x0F; *ie++ = 0xAC;
        *ie++ = (group_cipher == 0x02 || group_cipher == 0x04) ? group_cipher : 0x04;
        /* Pairwise Cipher Count = 1 */
        *ie++ = 0x01; *ie++ = 0x00;
        /* Pairwise Cipher: 00-0F-AC:4 (AES-CCMP) */
        *ie++ = 0x00; *ie++ = 0x0F; *ie++ = 0xAC; *ie++ = 0x04;
        /* AKM Suite Count = 1 */
        *ie++ = 0x01; *ie++ = 0x00;
        /* AKM Suite: 00-0F-AC:8 (SAE) or 00-0F-AC:2 (PSK) */
        *ie++ = 0x00; *ie++ = 0x0F; *ie++ = 0xAC;
        *ie++ = use_sae ? 0x08 : 0x02;
        /* RSN Capabilities: WPA3 requires MFPC (bit 7, 0x0080) | MFPR (bit 6, 0x0040) */
        uint16_t client_caps = use_sae ? ((rsn_caps & 0x00C0u) | 0x00C0u) : (rsn_caps & 0x0080u);
        *ie++ = (uint8_t)(client_caps & 0xFFu);
        *ie++ = (uint8_t)((client_caps >> 8) & 0xFFu);
        if (has_pmkid) {
            /* PMKID Count = 1 + 16-byte PMKID */
            *ie++ = 0x01; *ie++ = 0x00;
            memcpy(ie, pmkid, 16);
            ie += 16;
        }
    }

    /* 5. Optional 802.11n HT Capabilities IE (ID 45, Length 26) */
    if (include_ht) {
        *ie++ = IEEE80211_IE_HT_CAP;
        *ie++ = 26;
        /* HT Capability Info: LDPC (0x0001) | 40MHz (0x0002) | SMPS Disabled (0x000C) |
         * SGI20 (0x0020) | SGI40 (0x0040) | TX STBC (0x0080) | RX STBC 1SS (0x0100) = 0x01EF */
        *ie++ = 0xEF; *ie++ = 0x01;
        /* A-MPDU Parameters: Max RX A-MPDU 64KB (3) | Min MPDU Start Spacing 4us (5 << 2) = 0x17 */
        *ie++ = 0x17;
        /* Supported MCS Set (16 bytes): RX MCS 0..15 (2x2 MIMO) */
        *ie++ = 0xFF; *ie++ = 0xFF;
        memset(ie, 0, 14);
        ie += 14;
        /* HT Extended Capabilities (2B) + Transmit Beamforming (4B) + ASEL (1B) */
        memset(ie, 0, 7);
        ie += 7;
    }

    /* 6. Optional 802.11ac VHT Capabilities IE (ID 191, Length 12) */
    if (include_vht) {
        *ie++ = IEEE80211_IE_VHT_CAP;
        *ie++ = 12;
        /* VHT Capabilities Info: Max MPDU 11454 (2) | RX LDPC (1<<4) | SGI80 (1<<5) |
         * TX STBC (1<<7) | RX STBC 1SS (1<<8) | Max A-MPDU 2^(13+7)-1 (7<<23) = 0x038001B2 */
        *ie++ = 0xB2; *ie++ = 0x01; *ie++ = 0x80; *ie++ = 0x03;
        /* Supported VHT-MCS and NSS Set (8 bytes): 2x2 MCS 0..9 (0xFFFA) for RX and TX */
        *ie++ = 0xFA; *ie++ = 0xFF;
        *ie++ = 0x00; *ie++ = 0x00;
        *ie++ = 0xFA; *ie++ = 0xFF;
        *ie++ = 0x00; *ie++ = 0x00;
    }

    /* 7. WMM / QoS Information Element (9 bytes) */
    *ie++ = IEEE80211_IE_VENDOR;
    *ie++ = 7;
    *ie++ = 0x00; *ie++ = 0x50; *ie++ = 0xF2; /* Microsoft OUI */
    *ie++ = 0x02; /* WMM Type */
    *ie++ = 0x00; /* WMM Information Subtype */
    *ie++ = 0x01; /* Version 1 */
    *ie++ = 0x00; /* QoS Info */

    return (size_t)(ie - buf);
}

size_t net_80211_build_assoc_req_open(uint8_t *buf, size_t max_len,
                                      const uint8_t src_mac[6],
                                      const uint8_t bssid[6],
                                      const char *ssid) {
    return net_80211_build_assoc_req_full(buf, max_len, src_mac, bssid, ssid,
                                          true, false, 0, 0, NULL, false, false);
}

size_t net_80211_build_assoc_req_ext(uint8_t *buf, size_t max_len,
                                     const uint8_t src_mac[6],
                                     const uint8_t bssid[6],
                                     const char *ssid,
                                     uint8_t group_cipher,
                                     uint16_t rsn_caps) {
    return net_80211_build_assoc_req_full(buf, max_len, src_mac, bssid, ssid,
                                          false, false, group_cipher, rsn_caps, NULL, false, false);
}

size_t net_80211_build_assoc_req(uint8_t *buf, size_t max_len,
                                 const uint8_t src_mac[6],
                                 const uint8_t bssid[6],
                                 const char *ssid) {
    return net_80211_build_assoc_req_ext(buf, max_len, src_mac, bssid, ssid, 0x04, 0);
}

size_t net_80211_build_addba_resp(uint8_t *buf, size_t max_len,
                                  const uint8_t src_mac[6],
                                  const uint8_t bssid[6],
                                  uint8_t dialog_token,
                                  uint8_t tid,
                                  uint16_t buf_size) {
    size_t total_len = sizeof(ieee80211_hdr_t) + 9;
    if (!buf || !src_mac || !bssid || max_len < total_len) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_ACTION << 4);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, src_mac, 6);
    memcpy(hdr->addr3, bssid, 6);

    uint8_t *body = buf + sizeof(ieee80211_hdr_t);
    body[0] = IEEE80211_CATEGORY_BACK;     /* Category 3 = Block Ack */
    body[1] = IEEE80211_ACTION_ADDBA_RESP; /* Action 1 = ADDBA Response */
    body[2] = dialog_token;
    body[3] = 0x00; body[4] = 0x00;        /* Status Code = 0 (SUCCESS) */

    /* Block Ack Parameter Set: A-MSDU=0 (bit 0), Policy=1 Immediate (bit 1), TID (bits 2..5), BufSize (bits 6..15) */
    uint16_t bsize = (buf_size > 0 && buf_size <= 64) ? buf_size : 64;
    uint16_t ba_param = (uint16_t)((1u << 1) | (((uint16_t)(tid & 0x0Fu)) << 2) | (bsize << 6));
    body[5] = (uint8_t)(ba_param & 0xFFu);
    body[6] = (uint8_t)(ba_param >> 8);
    body[7] = 0x00; body[8] = 0x00;        /* Block Ack Timeout = 0 (disabled) */

    return total_len;
}

size_t net_80211_build_deauth(uint8_t *buf, size_t max_len,
                             const uint8_t src_mac[6],
                             const uint8_t bssid[6],
                             uint16_t reason) {
    size_t total_len = sizeof(ieee80211_hdr_t) + 2;
    if (!buf || max_len < total_len) return 0;

    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    /* Frame Control: Type = MGMT, Subtype = DEAUTH (0x0C) -> 0x00C0 */
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_DEAUTH << 4);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, src_mac, 6);
    memcpy(hdr->addr3, bssid, 6);

    uint8_t *body = buf + sizeof(ieee80211_hdr_t);
    body[0] = (uint8_t)(reason & 0xFF);
    body[1] = (uint8_t)(reason >> 8);

    return total_len;
}

/* ============================================================================
 * 2. 802.3 Ethernet <--> 802.11 LLC/SNAP Bridging
 * ============================================================================ */

int net_80211_encapsulate(const uint8_t eth_frame[], size_t eth_len,
                          const uint8_t bssid[6], const uint8_t src_mac[6],
                          uint16_t seq_num,
                          uint8_t wlan_frame[], size_t max_wlan_len) {
    if (!eth_frame || eth_len < 14 || !bssid || !src_mac || !wlan_frame) return -1;
    size_t payload_len = eth_len - 14;
    size_t total_wlan_len = sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + payload_len;
    if (max_wlan_len < total_wlan_len) return -1;

    /* Extract 802.3 Header */
    const uint8_t *dst_mac = eth_frame;
    uint16_t ethertype = ((uint16_t)eth_frame[12] << 8) | eth_frame[13];
    const uint8_t *payload = eth_frame + 14;

    /* 802.11 QoS Data Header: ToDS=1, FromDS=0 */
    ieee80211_qos_hdr_t *hdr = (ieee80211_qos_hdr_t *)wlan_frame;
    hdr->frame_control = (IEEE80211_TYPE_DATA << 2) |
                         (IEEE80211_SUBTYPE_QOS_DATA << 4) |
                         IEEE80211_FCTL_TODS;
    hdr->duration_id = 0;
    memcpy(hdr->addr1, bssid, 6);    /* RA = AP BSSID */
    memcpy(hdr->addr2, src_mac, 6);  /* TA = Station MAC */
    memcpy(hdr->addr3, dst_mac, 6);  /* DA = Target Destination */
    hdr->seq_ctrl = (uint16_t)((seq_num & 0x0FFF) << 4);
    hdr->qos_ctrl = 0x0000;          /* Best effort QoS */

    /* LLC/SNAP Header */
    llc_snap_hdr_t *snap = (llc_snap_hdr_t *)(wlan_frame + sizeof(ieee80211_qos_hdr_t));
    snap->dsap = 0xAA;
    snap->ssap = 0xAA;
    snap->control = 0x03;
    snap->oui[0] = 0x00; snap->oui[1] = 0x00; snap->oui[2] = 0x00;
    snap->ethertype = ((ethertype >> 8) & 0xFF) | ((ethertype << 8) & 0xFF00); /* Big-endian */

    /* Copy IP/ARP Payload */
    memcpy(wlan_frame + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t),
           payload, payload_len);

    return (int)total_wlan_len;
}

int net_80211_decapsulate(const uint8_t wlan_frame[], size_t wlan_len,
                          uint8_t eth_frame[], size_t max_eth_len) {
    if (!wlan_frame || wlan_len < sizeof(ieee80211_hdr_t) || !eth_frame) return -1;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)wlan_frame;
    uint8_t type = (uint8_t)((hdr->frame_control >> 2) & 0x03);
    uint8_t subtype = (uint8_t)((hdr->frame_control >> 4) & 0x0F);

    if (type != IEEE80211_TYPE_DATA) return -1;

    size_t hdr_len = sizeof(ieee80211_hdr_t);
    if (subtype == IEEE80211_SUBTYPE_QOS_DATA || (subtype & 0x08) != 0) {
        hdr_len = sizeof(ieee80211_qos_hdr_t);
    }
    if (wlan_len < hdr_len + sizeof(llc_snap_hdr_t)) return -1;

    /* Verify LLC/SNAP */
    const llc_snap_hdr_t *snap = (const llc_snap_hdr_t *)(wlan_frame + hdr_len);
    if (snap->dsap != 0xAA || snap->ssap != 0xAA || snap->control != 0x03) {
        return -1;
    }
    if (snap->oui[0] != 0x00 || snap->oui[1] != 0x00 || snap->oui[2] != 0x00) {
        return -1;
    }

    size_t payload_len = wlan_len - hdr_len - sizeof(llc_snap_hdr_t);
    size_t total_eth_len = 14 + payload_len;
    if (max_eth_len < total_eth_len) return -1;

    /* In a frame from AP (FromDS=1, ToDS=0):
     * Addr1 = Station MAC (DA)
     * Addr2 = BSSID
     * Addr3 = Source MAC (SA) */
    const uint8_t *dst_mac = hdr->addr1;
    const uint8_t *src_mac = hdr->addr3;
    uint16_t ethertype = ((snap->ethertype >> 8) & 0xFF) | ((snap->ethertype << 8) & 0xFF00);

    /* Construct 802.3 Header */
    memcpy(eth_frame, dst_mac, 6);
    memcpy(eth_frame + 6, src_mac, 6);
    eth_frame[12] = (uint8_t)(ethertype >> 8);
    eth_frame[13] = (uint8_t)(ethertype & 0xFF);

    /* Copy Payload */
    memcpy(eth_frame + 14, wlan_frame + hdr_len + sizeof(llc_snap_hdr_t), payload_len);
    return (int)total_eth_len;
}

/* ============================================================================
 * 3. Beacon & Probe Response Parser
 * ============================================================================ */

bool net_80211_parse_beacon_or_probe(const uint8_t *frame, size_t len,
                                     int8_t rssi,
                                     wlan_bss_entry_t *out_bss) {
    if (!frame || len < sizeof(ieee80211_hdr_t) + 12 || !out_bss) return false;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)frame;
    uint8_t type = (uint8_t)((hdr->frame_control >> 2) & 0x03);
    uint8_t subtype = (uint8_t)((hdr->frame_control >> 4) & 0x0F);

    if (type != IEEE80211_TYPE_MGMT) return false;
    if (subtype != IEEE80211_SUBTYPE_BEACON && subtype != IEEE80211_SUBTYPE_PROBE_RESP) {
        return false;
    }

    memset(out_bss, 0, sizeof(wlan_bss_entry_t));
    memcpy(out_bss->bssid, hdr->addr3, 6);
    out_bss->rssi = rssi;

    /* Fixed parameters: 8 bytes Timestamp + 2 bytes Beacon Interval + 2 bytes Capabilities */
    const uint8_t *fixed = frame + sizeof(ieee80211_hdr_t);
    out_bss->beacon_interval = (uint16_t)(fixed[8] | ((uint16_t)fixed[9] << 8));
    out_bss->capabilities   = (uint16_t)(fixed[10] | ((uint16_t)fixed[11] << 8));

    /* Parse Information Elements */
    size_t off = sizeof(ieee80211_hdr_t) + 12;
    while (off + 2 <= len) {
        uint8_t ie_id  = frame[off];
        uint8_t ie_len = frame[off + 1];
        if (off + 2 + ie_len > len) break;

        const uint8_t *data = &frame[off + 2];

        if (ie_id == IEEE80211_IE_SSID) {
            uint8_t clen = (ie_len > IEEE80211_SSID_MAX_LEN) ? IEEE80211_SSID_MAX_LEN : ie_len;
            memcpy(out_bss->ssid, data, clen);
            out_bss->ssid[clen] = '\0';
            out_bss->ssid_len = clen;
        } else if (ie_id == IEEE80211_IE_DS_PARAM && ie_len >= 1) {
            out_bss->channel = data[0];
        } else if (ie_id == IEEE80211_IE_HT_CAP && ie_len >= 26) {
            out_bss->has_ht = true;
            out_bss->ht_cap_info = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
            out_bss->ht_ampdu_param = data[2];
            memcpy(out_bss->ht_rx_mcs, data + 3, 4);
            if (out_bss->ht_cap_info & 0x0002u) {
                out_bss->ht_chan_width = 1; /* 40 MHz supported */
            }
        } else if (ie_id == IEEE80211_IE_HT_OP && ie_len >= 2) {
            if (out_bss->channel == 0) {
                out_bss->channel = data[0];
            }
            if (data[1] & 0x04u) {
                out_bss->ht_chan_width = 1;
            }
        } else if (ie_id == IEEE80211_IE_VHT_CAP && ie_len >= 12) {
            out_bss->has_vht = true;
            out_bss->vht_cap_info = (uint32_t)data[0] |
                                    ((uint32_t)data[1] << 8) |
                                    ((uint32_t)data[2] << 16) |
                                    ((uint32_t)data[3] << 24);
            out_bss->vht_rx_mcs_map = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
        } else if (ie_id == IEEE80211_IE_VHT_OP && ie_len >= 3) {
            out_bss->vht_chan_width = data[0];
            out_bss->vht_center_freq1 = data[1];
        } else if (ie_id == IEEE80211_IE_RSN && ie_len >= 14) {
            if (ie_len >= 6 && data[2] == 0x00 && data[3] == 0x0F && data[4] == 0xAC) {
                out_bss->group_cipher = data[5];
            } else {
                out_bss->group_cipher = 0x04;
            }
            bool parsed_akm = false;
            if (ie_len >= 8) {
                uint16_t pairwise_count = (uint16_t)(data[6] | (data[7] << 8));
                size_t off_in_ie = 8 + 4 * (size_t)pairwise_count;
                if (off_in_ie + 2 <= ie_len) {
                    uint16_t akm_count = (uint16_t)(data[off_in_ie] | (data[off_in_ie + 1] << 8));
                    off_in_ie += 2;
                    for (uint16_t a = 0; a < akm_count && off_in_ie + 4 <= ie_len; a++) {
                        if (data[off_in_ie + 0] == 0x00 &&
                            data[off_in_ie + 1] == 0x0F &&
                            data[off_in_ie + 2] == 0xAC) {
                            uint8_t akm_type = data[off_in_ie + 3];
                            if (akm_type == 2 || akm_type == 6) {
                                out_bss->has_wpa2_psk = true;
                                parsed_akm = true;
                            } else if (akm_type == 8) {
                                out_bss->has_wpa3_sae = true;
                                parsed_akm = true;
                            }
                        }
                        off_in_ie += 4;
                    }
                    if (off_in_ie + 2 <= ie_len) {
                        out_bss->rsn_caps = (uint16_t)(data[off_in_ie] | (data[off_in_ie + 1] << 8));
                    }
                }
            }
            if (!parsed_akm) {
                out_bss->has_wpa2_psk = true;
            }
        }

        off += 2 + ie_len;
    }

    return true;
}

/* ============================================================================
 * 4. BSS Scan Table Operations
 * ============================================================================ */

void wlan_bss_table_init(wlan_bss_table_t *table) {
    if (!table) return;
    memset(table, 0, sizeof(wlan_bss_table_t));
}

bool wlan_bss_table_update(wlan_bss_table_t *table, const wlan_bss_entry_t *bss) {
    if (!table || !bss) return false;

    /* Check if BSSID already exists */
    for (uint32_t i = 0; i < table->count; i++) {
        if (memcmp(table->entries[i].bssid, bss->bssid, 6) == 0) {
            /* Update existing entry */
            table->entries[i] = *bss;
            return true;
        }
    }

    /* Add new entry */
    if (table->count < IEEE80211_MAX_BSS) {
        table->entries[table->count++] = *bss;
        return true;
    }

    /* If table full, replace entry with lowest RSSI */
    uint32_t min_idx = 0;
    int8_t min_rssi = table->entries[0].rssi;
    for (uint32_t i = 1; i < table->count; i++) {
        if (table->entries[i].rssi < min_rssi) {
            min_rssi = table->entries[i].rssi;
            min_idx = i;
        }
    }
    if (bss->rssi > min_rssi) {
        table->entries[min_idx] = *bss;
        return true;
    }

    return false;
}

const wlan_bss_entry_t *wlan_bss_table_find(const wlan_bss_table_t *table, const char *ssid) {
    if (!table || !ssid) return NULL;
    const wlan_bss_entry_t *best = NULL;
    for (uint32_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].ssid, ssid) == 0) {
            if (!best || table->entries[i].rssi > best->rssi) {
                best = &table->entries[i];
            }
        }
    }
    return best;
}
