#define IWLWIFI_HOST_TEST 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "wifi_abi.h"
#include "netctl_abi.h"
#include "net_80211.h"
#include "wlan_crypto.h"
#include "wlan_wpa2.h"
#include "wlan_mgr.h"
#include "iwlwifi.h"
#include "rtw88.h"

/* Mock spinlock implementation for host testing */
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!pthread_mutex_lock(&l->mutex));
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags;
    assert(!pthread_mutex_unlock(&l->mutex));
}

/* Mock Radio State */
static uint8_t s_mock_sta_mac[6] = { 0x00, 0x24, 0xFD, 0x82, 0x65, 0x01 };
static uint8_t s_mock_ap_bssid[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
static uint8_t s_mock_last_tx[2048];
static size_t  s_mock_last_tx_len = 0;
static uint32_t s_mock_tx_count = 0;
static uint8_t s_mock_auth_channel, s_mock_auth_bssid[6];
static uint16_t s_mock_auth_interval;
static int mock_prepare_auth(uint8_t channel,const uint8_t bssid[6],uint16_t interval) {
    s_mock_auth_channel=channel; memcpy(s_mock_auth_bssid,bssid,6);
    s_mock_auth_interval=interval; return 0;
}

static int mock_get_mac(uint8_t mac[6]) {
    memcpy(mac, s_mock_sta_mac, 6);
    return 0;
}

static int mock_set_channel(uint8_t channel) {
    (void)channel;
    return 0;
}

static int mock_send_raw(const void *buf, size_t len) {
    if (!buf || len > sizeof(s_mock_last_tx)) return -1;
    memcpy(s_mock_last_tx, buf, len);
    s_mock_last_tx_len = len;
    s_mock_tx_count++;
    return 0;
}

static const wlan_hw_ops_t s_mock_hw_ops = {
    .name = "mock_wlan",
    .get_mac = mock_get_mac,
    .set_channel = mock_set_channel,
    .prepare_auth = mock_prepare_auth,
    .send_raw_80211 = mock_send_raw,
    .poll_raw_80211 = NULL,
    .recycle_raw_80211 = NULL,
};

/* Helper: Build mock Beacon frame */
static size_t build_mock_beacon(uint8_t *buf, const uint8_t bssid[6], const char *ssid, uint8_t channel) {
    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    hdr->frame_control = IEEE80211_TYPE_MGMT | (IEEE80211_SUBTYPE_BEACON << 4);
    hdr->duration_id = 0;
    memset(hdr->addr1, 0xFF, 6); /* Broadcast */
    memcpy(hdr->addr2, bssid, 6);
    memcpy(hdr->addr3, bssid, 6);
    hdr->seq_ctrl = 0;

    uint8_t *p = buf + sizeof(ieee80211_hdr_t);
    /* Fixed parameters (12 bytes: timestamp 8B, beacon interval 2B, capabilities 2B) */
    memset(p, 0, 8); p += 8;
    *p++ = 0x64; *p++ = 0x00; /* 100 TU */
    *p++ = 0x11; *p++ = 0x04; /* ESS + Privacy (WPA2) */

    /* SSID IE (0) */
    size_t slen = strlen(ssid);
    *p++ = IEEE80211_IE_SSID;
    *p++ = (uint8_t)slen;
    memcpy(p, ssid, slen);
    p += slen;

    /* DS Parameter IE (3: channel) */
    *p++ = IEEE80211_IE_DS_PARAM;
    *p++ = 1;
    *p++ = channel;

    /* RSN IE (48: WPA2-PSK AES-CCMP) */
    *p++ = IEEE80211_IE_RSN;
    *p++ = 20;
    *p++ = 0x01; *p++ = 0x00; /* Version 1 */
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04; /* Group: AES-CCMP */
    *p++ = 0x01; *p++ = 0x00; /* Pairwise count = 1 */
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x04; /* Pairwise: AES-CCMP */
    *p++ = 0x01; *p++ = 0x00; /* AKM count = 1 */
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 0x02; /* AKM: PSK */
    *p++ = 0x00; *p++ = 0x00; /* RSN Capabilities */

    return p - buf;
}

static unsigned long long diag_counter(const char *text, const char *key) {
    const char *p = strstr(text, key);
    assert(p);
    return strtoull(p + strlen(key), NULL, 10);
}

static void complete_mock_handshake(const uint8_t *pmk, const uint8_t ap_bssid[6], const uint8_t anonce[32]) {
    /* 1. Build and send EAPOL M1 */
    uint8_t m1[256];
    ieee80211_qos_hdr_t *wlan_h = (ieee80211_qos_hdr_t *)m1;
    wlan_h->frame_control = (IEEE80211_TYPE_DATA << 2) | (IEEE80211_SUBTYPE_QOS_DATA << 4) | IEEE80211_FCTL_FROMDS;
    wlan_h->duration_id = 0;
    memcpy(wlan_h->addr1, s_mock_sta_mac, 6);
    memcpy(wlan_h->addr2, ap_bssid, 6);
    memcpy(wlan_h->addr3, ap_bssid, 6);
    wlan_h->seq_ctrl = 0;
    wlan_h->qos_ctrl = 0;

    llc_snap_hdr_t *snap = (llc_snap_hdr_t *)(m1 + sizeof(ieee80211_qos_hdr_t));
    snap->dsap = 0xAA; snap->ssap = 0xAA; snap->control = 0x03;
    snap->oui[0] = 0; snap->oui[1] = 0; snap->oui[2] = 0;
    snap->ethertype = __builtin_bswap16(EAPOL_ETHER_TYPE);

    eapol_hdr_t *eh = (eapol_hdr_t *)(m1 + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t));
    eh->version = 2; eh->type = EAPOL_TYPE_KEY;
    eh->length = __builtin_bswap16(sizeof(eapol_key_desc_t));

    eapol_key_desc_t *desc = (eapol_key_desc_t *)(eh + 1);
    memset(desc, 0, sizeof(*desc));
    desc->desc_type = EAPOL_KEY_DESC_WPA2;
    desc->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK);
    desc->key_length = __builtin_bswap16(16);
    desc->replay_counter = __builtin_bswap64(1);
    memcpy(desc->key_nonce, anonce, 32);

    size_t m1_len = sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
    assert(wlan_mgr_rx_raw_80211(m1, m1_len, -45) == 0);

    /* 2. Station sent M2 -> Extract SNonce and derive PTK */
    const eapol_key_desc_t *m2_desc = (const eapol_key_desc_t *)(s_mock_last_tx + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t));
    uint8_t ptk[64];
    wlan_derive_ptk(pmk, ap_bssid, s_mock_sta_mac, anonce, m2_desc->key_nonce, ptk);

    /* 3. Build and send M3 */
    uint8_t m3[512];
    memcpy(m3, m1, m1_len);
    eapol_key_desc_t *m3_desc = (eapol_key_desc_t *)(m3 + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t));
    m3_desc->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE |
                                          EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_KEY_ACK |
                                          EAPOL_KEY_INFO_KEY_MIC | EAPOL_KEY_INFO_SECURE);
    m3_desc->replay_counter = __builtin_bswap64(2);
    m3_desc->key_data_len = 0;
    memset(m3_desc->key_mic, 0, 16);

    eapol_hdr_t *m3_eh = (eapol_hdr_t *)(m3 + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t));
    uint8_t mic[20];
    wlan_hmac_sha1(ptk, 16, (uint8_t *)m3_eh, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t), mic);
    memcpy(m3_desc->key_mic, mic, 16);

    assert(wlan_mgr_rx_raw_80211(m3, m1_len, -45) == 0);
}

int main(void) {
    printf("[WIFI HOST TEST] Starting Wireless (802.11 / WPA2-PSK / iwlwifi) Verification...\n");

    /* 1. Test Manager Initialization */
    wlan_mgr_init(&s_mock_hw_ops);
    net_dev_t *dev = wlan_get_net_device();
    assert(dev != NULL);
    assert(strcmp(dev->name, "wlan0") == 0);
    assert(memcmp(dev->mac_addr, s_mock_sta_mac, 6) == 0);
    assert(wlan_network_online(dev) == false);
    assert(wlan_link_state_abi(dev) == NET_IF_LINK_DOWN);
    printf("  [PASS] Wi-Fi Manager and net_dev_t registration\n");

    /* 2. Test Beacon Parsing and BSS Scan Table */
    uint8_t beacon_buf[256];
    size_t b_len = build_mock_beacon(beacon_buf, s_mock_ap_bssid, "FortressSecure", 6);
    int rx_rc = wlan_mgr_rx_raw_80211(beacon_buf, b_len, -45);
    assert(rx_rc == 0);

    wifi_scan_result_t scan_res;
    scan_res.max_entries = 16;
    scan_res.count = 0;
    assert(wlan_mgr_scan(&scan_res) == 0);
    assert(scan_res.count == 1);
    assert(strcmp(scan_res.entries[0].ssid, "FortressSecure") == 0);
    assert(memcmp(scan_res.entries[0].bssid, s_mock_ap_bssid, 6) == 0);
    assert(scan_res.entries[0].channel == 6);
    assert(scan_res.entries[0].rssi == -45);
    assert(scan_res.entries[0].security == WIFI_SEC_WPA2_PSK);
    printf("  [PASS] Beacon reception & BSS scan cache population\n");

    /* Fresh install / lost credential: a WPA2 beacon must never become open. */
    uint32_t missing_tx = s_mock_tx_count;
    assert(wlan_mgr_connect("FortressSecure", "", NULL) == -4);
    assert(s_mock_tx_count == missing_tx);
    assert(!wlan_network_online(dev));
    assert(wlan_mgr_connect("FortressSecure", NULL, s_mock_ap_bssid) == -4);
    assert(s_mock_tx_count == missing_tx);
    wifi_status_t missing_status;
    assert(wlan_mgr_status(&missing_status) == 0);
    assert(missing_status.state == WIFI_STATUS_FAILED && !missing_status.link_online);
    printf("  [PASS] Secured AP without credentials fails before authentication/authorization\n");

    /* 3. Test Connection State Machine: Connect & Auth Request */
    uint32_t prev_tx = s_mock_tx_count;
    int conn_rc = wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", s_mock_ap_bssid);
    assert(conn_rc == 0);
    assert(s_mock_tx_count > prev_tx);

    /* Verify transmitted frame was Open Auth Request (Seq 1) */
    ieee80211_hdr_t *auth_tx = (ieee80211_hdr_t *)s_mock_last_tx;
    assert(((auth_tx->frame_control >> 2) & 0x3) == IEEE80211_TYPE_MGMT);
    assert(((auth_tx->frame_control >> 4) & 0xF) == IEEE80211_SUBTYPE_AUTH);
    assert(memcmp(auth_tx->addr1, s_mock_ap_bssid, 6) == 0);
    assert(memcmp(auth_tx->addr2, s_mock_sta_mac, 6) == 0);

    wifi_status_t st;
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);
    printf("  [PASS] Connection initiation and Open Auth Request (Seq 1)\n");

    /* 4. Simulate Auth Response (Seq 2, Status 0) -> Transition to ASSOCIATING & send Assoc Req */
    uint8_t auth_resp[30];
    ieee80211_hdr_t *auth_resp_hdr = (ieee80211_hdr_t *)auth_resp;
    auth_resp_hdr->frame_control = IEEE80211_TYPE_MGMT | (IEEE80211_SUBTYPE_AUTH << 4);
    auth_resp_hdr->duration_id = 0;
    memcpy(auth_resp_hdr->addr1, s_mock_sta_mac, 6);
    memcpy(auth_resp_hdr->addr2, s_mock_ap_bssid, 6);
    memcpy(auth_resp_hdr->addr3, s_mock_ap_bssid, 6);
    auth_resp_hdr->seq_ctrl = 0;
    auth_resp[24] = 0x00; auth_resp[25] = 0x00; /* Auth algorithm = Open */
    auth_resp[26] = 0x02; auth_resp[27] = 0x00; /* Seq = 2 */
    auth_resp[28] = 0x00; auth_resp[29] = 0x00; /* Status = 0 (Success) */

    prev_tx = s_mock_tx_count;
    rx_rc = wlan_mgr_rx_raw_80211(auth_resp, sizeof(auth_resp), -45);
    assert(rx_rc == 0);
    assert(s_mock_tx_count > prev_tx);

    /* Verify transmitted frame was Association Request */
    ieee80211_hdr_t *assoc_tx = (ieee80211_hdr_t *)s_mock_last_tx;
    assert(((assoc_tx->frame_control >> 4) & 0xF) == IEEE80211_SUBTYPE_ASSOC_REQ);

    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_ASSOCIATING);
    printf("  [PASS] Auth Response processed, transitioned to ASSOCIATING & sent Assoc Req\n");

    /* 5. Simulate Association Response (Status 0, AID 1) -> Transition to 4-WAY HANDSHAKE */
    uint8_t assoc_resp[30];
    ieee80211_hdr_t *assoc_resp_hdr = (ieee80211_hdr_t *)assoc_resp;
    assoc_resp_hdr->frame_control = IEEE80211_TYPE_MGMT | (IEEE80211_SUBTYPE_ASSOC_RESP << 4);
    assoc_resp_hdr->duration_id = 0;
    memcpy(assoc_resp_hdr->addr1, s_mock_sta_mac, 6);
    memcpy(assoc_resp_hdr->addr2, s_mock_ap_bssid, 6);
    memcpy(assoc_resp_hdr->addr3, s_mock_ap_bssid, 6);
    assoc_resp_hdr->seq_ctrl = 0;
    assoc_resp[24] = 0x11; assoc_resp[25] = 0x04; /* Capabilities */
    assoc_resp[26] = 0x00; assoc_resp[27] = 0x00; /* Status = 0 (Success) */
    assoc_resp[28] = 0x01; assoc_resp[29] = 0xC0; /* AID = 1 */

    rx_rc = wlan_mgr_rx_raw_80211(assoc_resp, sizeof(assoc_resp), -45);
    assert(rx_rc == 0);

    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_HANDSHAKE);
    printf("  [PASS] Assoc Response processed, transitioned to 4-WAY HANDSHAKE\n");

    /* 6. WPA2 4-Way Handshake: EAPOL Message 1 -> Station sends Message 2 */
    uint8_t m1_frame[256];
    ieee80211_qos_hdr_t *m1_wlan = (ieee80211_qos_hdr_t *)m1_frame;
    m1_wlan->frame_control = (IEEE80211_TYPE_DATA << 2) | (IEEE80211_SUBTYPE_QOS_DATA << 4) | IEEE80211_FCTL_FROMDS;
    m1_wlan->duration_id = 0;
    memcpy(m1_wlan->addr1, s_mock_sta_mac, 6);
    memcpy(m1_wlan->addr2, s_mock_ap_bssid, 6);
    memcpy(m1_wlan->addr3, s_mock_ap_bssid, 6);
    m1_wlan->seq_ctrl = 0;
    m1_wlan->qos_ctrl = 0;

    llc_snap_hdr_t *snap = (llc_snap_hdr_t *)(m1_frame + sizeof(ieee80211_qos_hdr_t));
    snap->dsap = 0xAA; snap->ssap = 0xAA; snap->control = 0x03;
    snap->oui[0] = 0; snap->oui[1] = 0; snap->oui[2] = 0;
    snap->ethertype = __builtin_bswap16(EAPOL_ETHER_TYPE);

    eapol_hdr_t *eapol_h = (eapol_hdr_t *)(m1_frame + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t));
    eapol_h->version = 2;
    eapol_h->type = EAPOL_TYPE_KEY;
    eapol_h->length = __builtin_bswap16(sizeof(eapol_key_desc_t));

    eapol_key_desc_t *desc = (eapol_key_desc_t *)(eapol_h + 1);
    memset(desc, 0, sizeof(*desc));
    desc->desc_type = EAPOL_KEY_DESC_WPA2;
    desc->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK);
    desc->key_length = __builtin_bswap16(16);
    desc->replay_counter = __builtin_bswap64(1);
    memset(desc->key_nonce, 0x42, 32); /* ANonce */

    size_t m1_len = sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);

    prev_tx = s_mock_tx_count;
    rx_rc = wlan_mgr_rx_raw_80211(m1_frame, m1_len, -45);
    assert(rx_rc == 0);
    assert(s_mock_tx_count > prev_tx);

    /* Verify Station sent EAPOL Message 2 */
    ieee80211_qos_hdr_t *m2_tx = (ieee80211_qos_hdr_t *)s_mock_last_tx;
    assert(((m2_tx->frame_control >> 4) & 0xF) == IEEE80211_SUBTYPE_QOS_DATA);
    printf("  [PASS] EAPOL Message 1 parsed, SNonce generated, Message 2 transmitted with MIC\n");

    /* 7. EAPOL Message 3 -> Station sends Message 4 and Authorizes Port */
    uint8_t pmk[32];
    wlan_pbkdf2_sha1("SuperSecretKey2026", (const uint8_t *)"FortressSecure", 14, 4096, pmk, 32);

    /* Extract SNonce from Message 2 */
    const eapol_key_desc_t *m2_desc = (const eapol_key_desc_t *)(s_mock_last_tx + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t));
    uint8_t ptk[64];
    wlan_derive_ptk(pmk, s_mock_ap_bssid, s_mock_sta_mac, desc->key_nonce, m2_desc->key_nonce, ptk);
    uint8_t *kck = ptk;
    uint8_t *tk = ptk + 32;

    /* Build Message 3 */
    uint8_t m3_frame[512];
    memcpy(m3_frame, m1_frame, m1_len);
    eapol_key_desc_t *m3_desc = (eapol_key_desc_t *)(m3_frame + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t));
    m3_desc->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE |
                                          EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_KEY_ACK |
                                          EAPOL_KEY_INFO_KEY_MIC | EAPOL_KEY_INFO_SECURE);
    m3_desc->replay_counter = __builtin_bswap64(2);
    m3_desc->key_data_len = 0;

    eapol_hdr_t *m3_eh = (eapol_hdr_t *)(m3_frame + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t));
    m3_eh->length = __builtin_bswap16(sizeof(eapol_key_desc_t));

    /* Compute MIC for Message 3 */
    memset(m3_desc->key_mic, 0, 16);
    uint8_t m3_mic[20];
    wlan_hmac_sha1(kck, 16, (uint8_t *)m3_eh, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t), m3_mic);
    memcpy(m3_desc->key_mic, m3_mic, 16);

    size_t m3_len = sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);

    prev_tx = s_mock_tx_count;
    rx_rc = wlan_mgr_rx_raw_80211(m3_frame, m3_len, -45);
    assert(rx_rc == 0);
    assert(s_mock_tx_count > prev_tx);

    /* Verify Port is now AUTHORIZED and Link is ONLINE */
    assert(wlan_network_online(dev) == true);
    assert(wlan_link_state_abi(dev) == NET_IF_LINK_ONLINE);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_CONNECTED);
    assert(st.link_online == 1);
    printf("  [PASS] EAPOL Message 3 verified, Message 4 sent, Port AUTHORIZED\n");

    /* 8. Test 802.3 Ethernet <--> 802.11 QoS Data Bridging with AES-CCMP Encryption */
    uint8_t eth_tx_pkt[64];
    memset(eth_tx_pkt, 0x11, 6); /* Dest MAC */
    memcpy(eth_tx_pkt + 6, s_mock_sta_mac, 6); /* Src MAC */
    eth_tx_pkt[12] = 0x08; eth_tx_pkt[13] = 0x00; /* IPv4 */
    for (int i = 14; i < 64; i++) eth_tx_pkt[i] = (uint8_t)i;

    prev_tx = s_mock_tx_count;
    int tx_res = wlan_send_packet(dev, eth_tx_pkt, sizeof(eth_tx_pkt));
    assert(tx_res == 0);
    assert(s_mock_tx_count > prev_tx);

    /* Verify transmitted frame is encrypted (Protected bit = 1) */
    ieee80211_qos_hdr_t *enc_tx = (ieee80211_qos_hdr_t *)s_mock_last_tx;
    assert(enc_tx->frame_control & IEEE80211_FCTL_PROTECTED);

    /* Decrypt with test TK to verify integrity */
    uint8_t dec_buf[128];
    size_t dec_len = sizeof(dec_buf);
    uint64_t last_pn = 0;
    int dec_rc = wlan_ccmp_decrypt_frame(tk, s_mock_last_tx, s_mock_last_tx_len, dec_buf, &dec_len, &last_pn, NULL);
    assert(dec_rc == 0);
    assert(last_pn == 1);
    printf("  [PASS] 802.3 to 802.11 LLC/SNAP encapsulation & AES-128-CCMP packet encryption\n");

    /* 9. Test Inbound Data Frame: Encrypt frame from AP -> feed to rx_raw -> poll_rx */
    uint8_t inbound_eth[60];
    memcpy(inbound_eth, s_mock_sta_mac, 6);
    memcpy(inbound_eth + 6, s_mock_ap_bssid, 6);
    inbound_eth[12] = 0x08; inbound_eth[13] = 0x06; /* ARP */
    for (int i = 14; i < 60; i++) inbound_eth[i] = (uint8_t)(0xAA + i);

    uint8_t inbound_wlan_plain[128];
    int encap_len = net_80211_encapsulate(inbound_eth, sizeof(inbound_eth), s_mock_ap_bssid, s_mock_sta_mac, 1,
                                         inbound_wlan_plain, sizeof(inbound_wlan_plain));
    assert(encap_len > 0);

    /* Modify Frame Control to FROMDS */
    ieee80211_qos_hdr_t *in_hdr = (ieee80211_qos_hdr_t *)inbound_wlan_plain;
    in_hdr->frame_control |= IEEE80211_FCTL_FROMDS;
    memcpy(in_hdr->addr1, s_mock_sta_mac, 6);
    memcpy(in_hdr->addr2, s_mock_ap_bssid, 6);
    memcpy(in_hdr->addr3, s_mock_ap_bssid, 6);

    uint8_t inbound_wlan_cipher[256];
    size_t in_c_len = sizeof(inbound_wlan_cipher);
    assert(wlan_ccmp_encrypt_frame(tk, 1, inbound_wlan_plain, (size_t)encap_len, inbound_wlan_cipher, &in_c_len) == 0);

    rx_rc = wlan_mgr_rx_raw_80211(inbound_wlan_cipher, in_c_len, -45);
    assert(rx_rc == 0);

    /* Poll decapsulated packet from wlan0 */
    pbuf_t *rx_p = wlan_poll_rx(dev);
    assert(rx_p != NULL);
    assert(rx_p->length == sizeof(inbound_eth));
    assert(memcmp(rx_p->payload, inbound_eth, sizeof(inbound_eth)) == 0);
    wlan_recycle_rx(dev, rx_p);
    printf("  [PASS] Inbound 802.11 CCMP decryption, LLC/SNAP decapsulation & 802.3 delivery\n");

    /* 10. Adversarial Replay Counter Attack: Re-inject same Packet Number (PN=1) */
    rx_rc = wlan_mgr_rx_raw_80211(inbound_wlan_cipher, in_c_len, -45);
    assert(rx_rc == 0);
    /* Should be dropped due to replay protection (PN <= last_pn) */
    assert(wlan_poll_rx(dev) == NULL);
    printf("  [PASS] Monotonic 48-bit Packet Number (PN) replay attack rejected\n");

    /* 11. Test Intel iwlwifi Firmware Loader on official microcode blob */
    FILE *fw_f = fopen("assets/firmware/iwlwifi-8265-36.ucode", "rb");
    assert(fw_f != NULL);
    fseek(fw_f, 0, SEEK_END);
    long fw_size = ftell(fw_f);
    fseek(fw_f, 0, SEEK_SET);
    assert(fw_size > 100000);
    uint8_t *fw_bytes = malloc(fw_size);
    assert(fw_bytes != NULL);
    assert(fread(fw_bytes, 1, fw_size, fw_f) == (size_t)fw_size);
    fclose(fw_f);

    bool fw_valid = iwlwifi_load_firmware(fw_bytes, (size_t)fw_size);
    assert(fw_valid == true);
    free(fw_bytes);
    assert(iwlwifi_is_supported_device(IWL_PCI_VENDOR_ID, IWL_DEV_ID_AC_8265));
    assert(iwlwifi_is_supported_device(IWL_PCI_VENDOR_ID, IWL_DEV_ID_AX_211));
    printf("  [PASS] Intel iwlwifi official microcode blob (iwlwifi-8265-36.ucode, %ld bytes) validated\n", fw_size);

    /* 12. Test Disconnect */
    assert(wlan_mgr_disconnect() == 0);
    assert(wlan_network_online(dev) == false);
    assert(wlan_link_state_abi(dev) == NET_IF_LINK_DOWN);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_IDLE);
    printf("  [PASS] Deauthentication and clean disconnect\n");

    /* 13. Test Bad Passphrase & Handshake Failure Handling */
    /* Attempt connect with wrong passphrase */
    assert(wlan_mgr_connect("FortressSecure", "CompletelyWrongPassword123", s_mock_ap_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);

    /* Simulate Auth Response */
    rx_rc = wlan_mgr_rx_raw_80211(auth_resp, sizeof(auth_resp), -40);
    assert(rx_rc == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_ASSOCIATING);

    /* Simulate Assoc Response */
    rx_rc = wlan_mgr_rx_raw_80211(assoc_resp, sizeof(assoc_resp), -40);
    assert(rx_rc == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_HANDSHAKE);

    /* Simulate EAPOL Message 1 */
    rx_rc = wlan_mgr_rx_raw_80211(m1_frame, m1_len, -45);
    assert(rx_rc == 0);

    /* AP sends Message 3 computed with real PSK (mismatches client's bad PSK MIC) */
    rx_rc = wlan_mgr_rx_raw_80211(m3_frame, m3_len, -45);
    assert(rx_rc == 0);

    /* Client must immediately transition to FAILED without panicking or hanging */
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_FAILED);
    assert(wlan_network_online(dev) == false);
    assert(wlan_link_state_abi(dev) == NET_IF_LINK_DOWN);
    printf("  [PASS] Invalid passphrase / Message 3 MIC mismatch fail-closed to WIFI_STATUS_FAILED\n");

    /* Also verify AP Deauthentication frame during handshake marks FAILED */
    assert(wlan_mgr_connect("FortressSecure", "AnotherWrongPassword", s_mock_ap_bssid) == 0);
    assert(wlan_mgr_rx_raw_80211(auth_resp, sizeof(auth_resp), -40) == 0);
    assert(wlan_mgr_rx_raw_80211(assoc_resp, sizeof(assoc_resp), -40) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_HANDSHAKE);

    /* Simulate AP Deauth */
    uint8_t deauth_frame[32];
    size_t deauth_len = net_80211_build_deauth(deauth_frame, sizeof(deauth_frame), s_mock_ap_bssid, s_mock_sta_mac, 14);
    assert(deauth_len > 0);
    rx_rc = wlan_mgr_rx_raw_80211(deauth_frame, deauth_len, -40);
    assert(rx_rc == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_FAILED);
    printf("  [PASS] AP Deauthentication during handshake transitions cleanly to WIFI_STATUS_FAILED\n");

    /* 14. Test Asynchronous Non-blocking Connect, Retransmission & Timeout */
    assert(wlan_mgr_disconnect() == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_IDLE);

    /* Initiate non-blocking connect */
    uint32_t tx_before = s_mock_tx_count;
    assert(wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", s_mock_ap_bssid) == 0);
    assert(s_mock_tx_count == tx_before + 1); /* Auth req sent */
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);

    /* Simulate dropped Auth Response on the air: call service_link after 1.6s */
    uint64_t tick_rate = 1000; /* 1000 Hz */
    uint64_t fake_now = 1000;
    wlan_service_link(dev, fake_now, tick_rate);
    fake_now += 1600; /* 1.6s later */
    wlan_service_link(dev, fake_now, tick_rate);
    /* Retransmit must have fired */
    assert(s_mock_tx_count == tx_before + 2);

    /* Now advance past 6.0 seconds without AP response */
    fake_now += 5000; /* 6.6s total */
    wlan_service_link(dev, fake_now, tick_rate);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_FAILED);
    printf("  [PASS] Asynchronous progression: autonomous retransmission & 6s timeout fail-closed\n");

    /* 15. Test Fast-path PMK Caching */
    assert(wlan_mgr_disconnect() == 0);
    /* 15. Test Fast-path PMK Caching */
    assert(wlan_mgr_disconnect() == 0);
    /* First connect caches PMK */
    assert(wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", s_mock_ap_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);

    /* Disconnect and reconnect to same SSID/PSK: fast path reuses cached PMK */
    assert(wlan_mgr_disconnect() == 0);
    assert(wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", s_mock_ap_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);
    printf("  [PASS] Fast-path PMK caching (sub-millisecond instant reconnect)\n");

    /* 16. Test Multi-BSSID Best RSSI Selection & Autonomous Roaming */
    assert(wlan_mgr_disconnect() == 0);
    uint8_t bssid_alt[6]  = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0x02 };
    uint8_t bssid_best[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0x03 };

    /* Populate BSS cache with alt AP (-75 dBm on ch 1) and best AP (-35 dBm on ch 11) */
    uint8_t b_alt[256];
    size_t b_alt_len = build_mock_beacon(b_alt, bssid_alt, "FortressSecure", 1);
    assert(wlan_mgr_rx_raw_80211(b_alt, b_alt_len, -75) == 0);

    uint8_t b_best[256];
    size_t b_best_len = build_mock_beacon(b_best, bssid_best, "FortressSecure", 11);
    assert(wlan_mgr_rx_raw_80211(b_best, b_best_len, -35) == 0);

    /* Connect without specifying BSSID: must auto-select bssid_best (-35 dBm) */
    assert(wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", NULL) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(memcmp(st.bssid, bssid_best, 6) == 0);
    assert(st.channel == 11);

    /* GUI selection pins a BSSID from the last scan. A newer beacon may have
     * made a different AP of the same SSID stronger, on another channel.
     * Never combine the selected AP's address with the strongest AP's channel. */
    assert(wlan_mgr_disconnect()==0);
    assert(wlan_mgr_connect("FortressSecure","SuperSecretKey2026",bssid_alt)==0);
    assert(wlan_mgr_status(&st)==0);
    assert(!memcmp(st.bssid,bssid_alt,6) && st.channel==1 && st.rssi==-75);
    assert(!memcmp(s_mock_auth_bssid,bssid_alt,6) && s_mock_auth_channel==1 && s_mock_auth_interval==100);
    printf("  [PASS] Selected BSSID keeps its own channel/RSSI/timing despite stronger same-SSID AP\n");

    /* Now test Autonomous Roaming: connect directly to s_mock_ap_bssid and complete handshake */
    assert(wlan_mgr_disconnect() == 0);
    assert(wlan_mgr_connect("FortressSecure", "SuperSecretKey2026", s_mock_ap_bssid) == 0);
    assert(wlan_mgr_rx_raw_80211(auth_resp, sizeof(auth_resp), -45) == 0);
    assert(wlan_mgr_rx_raw_80211(assoc_resp, sizeof(assoc_resp), -45) == 0);
    uint8_t fresh_anonce[32];
    memset(fresh_anonce, 0x55, 32);
    complete_mock_handshake(pmk, s_mock_ap_bssid, fresh_anonce);
    assert(wlan_network_online(dev) == true);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_CONNECTED);

    /* Simulate severe signal degradation on current AP to -82 dBm */
    assert(wlan_mgr_rx_raw_80211(beacon_buf, b_len, -82) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.rssi == -82);

    /* Refresh beacon from bssid_best with strong signal (-35 dBm) */
    assert(wlan_mgr_rx_raw_80211(b_best, b_best_len, -35) == 0);

    /* Service link tick detects weak current signal and available +47 dBm candidate */
    tx_before = s_mock_tx_count;
    wlan_service_link(dev, fake_now + 20000, tick_rate);

    /* Verify roam triggered: deauth old BSSID + auth req to new bssid_best */
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);
    assert(memcmp(st.bssid, bssid_best, 6) == 0);
    assert(st.channel == 11);
    printf("  [PASS] Multi-BSSID best RSSI selection & autonomous background roaming verified\n");

    /* 17. Test Open / Public Wi-Fi Network Support (Zero Passphrase) */
    assert(wlan_mgr_disconnect() == 0);
    uint8_t open_bssid[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
    uint8_t open_beacon[256];
    {
        ieee80211_hdr_t *oh = (ieee80211_hdr_t *)open_beacon;
        memset(oh, 0, sizeof(*oh));
        oh->frame_control = IEEE80211_TYPE_MGMT | (IEEE80211_SUBTYPE_BEACON << 4);
        memset(oh->addr1, 0xFF, 6);
        memcpy(oh->addr2, open_bssid, 6);
        memcpy(oh->addr3, open_bssid, 6);
        uint8_t *p = open_beacon + sizeof(ieee80211_hdr_t);
        memset(p, 0, 8); p += 8;
        *p++ = 0x64; *p++ = 0x00;
        *p++ = 0x01; *p++ = 0x04; /* ESS only, Privacy = 0 */
        const char *open_ssid = "AirportFreeWiFi";
        size_t oslen = strlen(open_ssid);
        *p++ = IEEE80211_IE_SSID; *p++ = (uint8_t)oslen;
        memcpy(p, open_ssid, oslen); p += oslen;
        *p++ = IEEE80211_IE_DS_PARAM; *p++ = 1; *p++ = 6;
        /* HT Cap IE (45) + VHT Cap IE (191) to test HT/VHT negotiation too */
        *p++ = IEEE80211_IE_HT_CAP; *p++ = 26;
        memset(p, 0, 26); p[0] = 0x6F; p[1] = 0x01; p[3] = 0xFF; p[4] = 0xFF; p += 26;
        *p++ = IEEE80211_IE_VHT_CAP; *p++ = 12;
        memset(p, 0, 12); p += 12;
        assert(wlan_mgr_rx_raw_80211(open_beacon, (size_t)(p - open_beacon), -42) == 0);
    }

    /* Connect with empty passphrase */
    assert(wlan_mgr_connect("AirportFreeWiFi", "", open_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);

    /* Simulate Auth Response from open_bssid */
    uint8_t open_auth_resp[sizeof(ieee80211_hdr_t) + 6];
    memset(open_auth_resp, 0, sizeof(open_auth_resp));
    ((ieee80211_hdr_t *)open_auth_resp)->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_AUTH << 4);
    memcpy(((ieee80211_hdr_t *)open_auth_resp)->addr1, s_mock_sta_mac, 6);
    memcpy(((ieee80211_hdr_t *)open_auth_resp)->addr2, open_bssid, 6);
    memcpy(((ieee80211_hdr_t *)open_auth_resp)->addr3, open_bssid, 6);
    open_auth_resp[sizeof(ieee80211_hdr_t) + 2] = 0x02; /* Seq 2 */
    assert(wlan_mgr_rx_raw_80211(open_auth_resp, sizeof(open_auth_resp), -42) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_ASSOCIATING);

    /* Verify transmitted Association Request omits RSN IE and includes HT/VHT IEs */
    {
        bool has_rsn = false, has_ht = false, has_vht = false;
        size_t off = sizeof(ieee80211_hdr_t) + 4;
        while (off + 2 <= s_mock_last_tx_len) {
            if (s_mock_last_tx[off] == IEEE80211_IE_RSN) has_rsn = true;
            if (s_mock_last_tx[off] == IEEE80211_IE_HT_CAP) has_ht = true;
            if (s_mock_last_tx[off] == IEEE80211_IE_VHT_CAP) has_vht = true;
            off += 2 + s_mock_last_tx[off + 1];
        }
        assert(!has_rsn);
        assert(has_ht && has_vht);
    }

    /* Simulate Assoc Response SUCCESS: must skip 4-way handshake and transition directly to CONNECTED */
    uint8_t open_assoc_resp[sizeof(ieee80211_hdr_t) + 6];
    memset(open_assoc_resp, 0, sizeof(open_assoc_resp));
    ((ieee80211_hdr_t *)open_assoc_resp)->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_ASSOC_RESP << 4);
    memcpy(((ieee80211_hdr_t *)open_assoc_resp)->addr1, s_mock_sta_mac, 6);
    memcpy(((ieee80211_hdr_t *)open_assoc_resp)->addr2, open_bssid, 6);
    memcpy(((ieee80211_hdr_t *)open_assoc_resp)->addr3, open_bssid, 6);
    open_assoc_resp[sizeof(ieee80211_hdr_t) + 4] = 0x01; open_assoc_resp[sizeof(ieee80211_hdr_t) + 5] = 0xC0;
    assert(wlan_mgr_rx_raw_80211(open_assoc_resp, sizeof(open_assoc_resp), -42) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_CONNECTED);
    assert(wlan_network_online(dev) == true);

    /* Verify outbound packet on Open network is unencrypted (Protected bit == 0) */
    assert(wlan_send_packet(dev, eth_tx_pkt, sizeof(eth_tx_pkt)) == 0);
    assert((s_mock_last_tx[1] & 0x40u) == 0);

    /* Verify inbound unencrypted QoS Data frame on Open network is accepted and decapsulated */
    uint8_t peer_mac[6] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    uint8_t open_rx_wlan[256];
    int open_rx_len = net_80211_encapsulate(inbound_eth, sizeof(inbound_eth), open_bssid, s_mock_ap_bssid, 77, open_rx_wlan, sizeof(open_rx_wlan));
    assert(open_rx_len > 0);
    ieee80211_qos_hdr_t *orx_h = (ieee80211_qos_hdr_t *)open_rx_wlan;
    orx_h->frame_control = (IEEE80211_TYPE_DATA << 2) | (IEEE80211_SUBTYPE_QOS_DATA << 4) | IEEE80211_FCTL_FROMDS;
    memcpy(orx_h->addr1, s_mock_sta_mac, 6);
    memcpy(orx_h->addr2, open_bssid, 6);
    memcpy(orx_h->addr3, s_mock_ap_bssid, 6);
    assert(wlan_mgr_rx_raw_80211(open_rx_wlan, (size_t)open_rx_len, -42) == 0);
    rx_p = wlan_poll_rx(dev);
    assert(rx_p != NULL);
    assert(rx_p->length == sizeof(inbound_eth));
    assert(memcmp(rx_p->payload, inbound_eth, sizeof(inbound_eth)) == 0);
    wlan_recycle_rx(dev, rx_p);
    printf("  [PASS] Open / Public Wi-Fi connection (no RSN IE, skips 4-way handshake, unencrypted TX/RX)\n");

    /* 18. Test 802.11n/ac A-MPDU ADDBA Request -> ADDBA Response Action Frame */
    {
        uint8_t addba_req[sizeof(ieee80211_hdr_t) + 9];
        memset(addba_req, 0, sizeof(addba_req));
        ieee80211_hdr_t *ah = (ieee80211_hdr_t *)addba_req;
        ah->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_ACTION << 4);
        memcpy(ah->addr1, s_mock_sta_mac, 6);
        memcpy(ah->addr2, open_bssid, 6);
        memcpy(ah->addr3, open_bssid, 6);
        uint8_t *ab = addba_req + sizeof(ieee80211_hdr_t);
        ab[0] = IEEE80211_CATEGORY_BACK;
        ab[1] = IEEE80211_ACTION_ADDBA_REQ;
        ab[2] = 0x42; /* Dialog token */
        ab[3] = 0x02; ab[4] = 0x10; /* TID=0, buf_size=64 */
        uint32_t tx_pre = s_mock_tx_count;
        assert(wlan_mgr_rx_raw_80211(addba_req, sizeof(addba_req), -42) == 0);
        assert(s_mock_tx_count == tx_pre + 1);
        assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 0] == IEEE80211_CATEGORY_BACK);
        assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 1] == IEEE80211_ACTION_ADDBA_RESP);
        assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 2] == 0x42);
    }
    printf("  [PASS] 802.11n/ac HT/VHT capability negotiation & A-MPDU ADDBA Response verified\n");

    /* 19. Test Mixed-Mode AP TKIP Group Broadcast Frame Decryption */
    assert(wlan_mgr_disconnect() == 0);
    uint8_t tkip_ap_bssid[6] = { 0x00, 0x22, 0x44, 0x66, 0x88, 0xAA };
    {
        uint8_t tkip_beacon[256];
        size_t tb_len = build_mock_beacon(tkip_beacon, tkip_ap_bssid, "MixedModeRouter", 6);
        /* Patch RSN IE Group Cipher from 0x04 (CCMP) to 0x02 (TKIP) */
        size_t off = sizeof(ieee80211_hdr_t) + 12;
        while (off + 2 <= tb_len) {
            if (tkip_beacon[off] == IEEE80211_IE_RSN) {
                tkip_beacon[off + 7] = 0x02; /* Group Cipher = TKIP */
                break;
            }
            off += 2 + tkip_beacon[off + 1];
        }
        assert(wlan_mgr_rx_raw_80211(tkip_beacon, tb_len, -45) == 0);
    }
    assert(wlan_mgr_connect("MixedModeRouter", "SuperSecretKey2026", tkip_ap_bssid) == 0);
    uint8_t tkip_pmk[32];
    assert(wlan_pbkdf2_sha1("SuperSecretKey2026", (const uint8_t *)"MixedModeRouter", 15, 4096, tkip_pmk, 32) == 0);

    /* Auth + Assoc */
    memcpy(((ieee80211_hdr_t *)auth_resp)->addr2, tkip_ap_bssid, 6);
    memcpy(((ieee80211_hdr_t *)auth_resp)->addr3, tkip_ap_bssid, 6);
    memcpy(((ieee80211_hdr_t *)assoc_resp)->addr2, tkip_ap_bssid, 6);
    memcpy(((ieee80211_hdr_t *)assoc_resp)->addr3, tkip_ap_bssid, 6);
    assert(wlan_mgr_rx_raw_80211(auth_resp, sizeof(auth_resp), -45) == 0);
    assert(wlan_mgr_rx_raw_80211(assoc_resp, sizeof(assoc_resp), -45) == 0);

    /* Complete handshake with 32-byte TKIP GTK in M3 */
    uint8_t tkip_gtk[32];
    for (int i = 0; i < 32; i++) tkip_gtk[i] = (uint8_t)(0x70 + i);
    {
        uint8_t m1[256];
        memset(m1, 0, sizeof(m1));
        ieee80211_qos_hdr_t *wh = (ieee80211_qos_hdr_t *)m1;
        wh->frame_control = (IEEE80211_TYPE_DATA << 2) | (IEEE80211_SUBTYPE_QOS_DATA << 4) | IEEE80211_FCTL_FROMDS;
        memcpy(wh->addr1, s_mock_sta_mac, 6);
        memcpy(wh->addr2, tkip_ap_bssid, 6);
        memcpy(wh->addr3, tkip_ap_bssid, 6);
        llc_snap_hdr_t *sn = (llc_snap_hdr_t *)(m1 + sizeof(ieee80211_qos_hdr_t));
        sn->dsap = 0xAA; sn->ssap = 0xAA; sn->control = 0x03;
        sn->ethertype = __builtin_bswap16(EAPOL_ETHER_TYPE);
        eapol_hdr_t *eh = (eapol_hdr_t *)(sn + 1);
        eh->version = 2; eh->type = EAPOL_TYPE_KEY;
        eh->length = __builtin_bswap16(sizeof(eapol_key_desc_t));
        eapol_key_desc_t *kd = (eapol_key_desc_t *)(eh + 1);
        kd->desc_type = EAPOL_KEY_DESC_WPA2;
        kd->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK);
        kd->key_length = __builtin_bswap16(16);
        kd->replay_counter = __builtin_bswap64(1);
        memset(kd->key_nonce, 0x33, 32);
        size_t m1_l = sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
        assert(wlan_mgr_rx_raw_80211(m1, m1_l, -45) == 0);

        const eapol_key_desc_t *m2_d = (const eapol_key_desc_t *)(s_mock_last_tx + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + sizeof(eapol_hdr_t));
        uint8_t t_ptk[64];
        wlan_derive_ptk(tkip_pmk, tkip_ap_bssid, s_mock_sta_mac, kd->key_nonce, m2_d->key_nonce, t_ptk);

        uint8_t m3[512];
        memset(m3, 0, sizeof(m3));
        memcpy(m3, m1, m1_l);
        eapol_hdr_t *eh3 = (eapol_hdr_t *)(m3 + sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t));
        eh3->length = __builtin_bswap16(sizeof(eapol_key_desc_t) + 40);
        eapol_key_desc_t *kd3 = (eapol_key_desc_t *)(eh3 + 1);
        kd3->key_info = __builtin_bswap16(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE |
                                          EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_KEY_ACK |
                                          EAPOL_KEY_INFO_KEY_MIC | EAPOL_KEY_INFO_SECURE);
        kd3->replay_counter = __builtin_bswap64(2);
        kd3->key_data_len = __builtin_bswap16(40);
        uint8_t *kde = (uint8_t *)(kd3 + 1);
        kde[0] = 0xDD; kde[1] = 38;
        kde[2] = 0x00; kde[3] = 0x0F; kde[4] = 0xAC; kde[5] = 0x01;
        kde[6] = 0x01; kde[7] = 0x00;
        memcpy(kde + 8, tkip_gtk, 32);
        uint8_t mic3[20];
        wlan_hmac_sha1(t_ptk, 16, eh3, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t) + 40, mic3);
        memcpy(kd3->key_mic, mic3, 16);
        assert(wlan_mgr_rx_raw_80211(m3, m1_l + 40, -45) == 0);
        assert(wlan_network_online(dev) == true);
    }

    /* Inject a TKIP-encrypted broadcast ARP frame from the AP */
    {
        uint8_t bcast_eth[42];
        memset(bcast_eth, 0xFF, 6); /* Broadcast DA */
        memcpy(bcast_eth + 6, peer_mac, 6);
        bcast_eth[12] = 0x08; bcast_eth[13] = 0x06; /* ARP */
        for (int i = 14; i < 42; i++) bcast_eth[i] = (uint8_t)i;

        uint8_t bcast_wlan_plain[128];
        int bp_len = net_80211_encapsulate(bcast_eth, sizeof(bcast_eth), tkip_ap_bssid, peer_mac, 88, bcast_wlan_plain, sizeof(bcast_wlan_plain));
        assert(bp_len > 0);
        ieee80211_qos_hdr_t *bh = (ieee80211_qos_hdr_t *)bcast_wlan_plain;
        bh->frame_control = (IEEE80211_TYPE_DATA << 2) | (IEEE80211_SUBTYPE_QOS_DATA << 4) | IEEE80211_FCTL_FROMDS;
        memset(bh->addr1, 0xFF, 6); /* Broadcast Receiver */
        memcpy(bh->addr2, tkip_ap_bssid, 6);
        memcpy(bh->addr3, peer_mac, 6);

        uint8_t bcast_tkip_cipher[256];
        size_t btc_len = sizeof(bcast_tkip_cipher);
        assert(wlan_tkip_encrypt_frame(tkip_gtk, 1, 1, bcast_wlan_plain, (size_t)bp_len, bcast_tkip_cipher, &btc_len) == 0);

        char diag[1024];
        size_t dn = wlan_rx_profile_format(diag, sizeof(diag) - 1); diag[dn] = 0;
        unsigned long long ok0 = diag_counter(diag, "group-ok=");
        unsigned long long replay0 = diag_counter(diag, "group-replay=");
        unsigned long long fail0 = diag_counter(diag, "group-decrypt-fail=");
        unsigned long long arp0 = diag_counter(diag, " arp=");
        uint32_t tx0 = s_mock_tx_count;
        assert(wlan_mgr_rx_raw_80211(bcast_tkip_cipher, btc_len, -45) == 0);
        rx_p = wlan_poll_rx(dev);
        assert(rx_p != NULL);
        assert(rx_p->length == sizeof(bcast_eth));
        assert(memcmp(rx_p->payload, bcast_eth, sizeof(bcast_eth)) == 0);
        wlan_recycle_rx(dev, rx_p);
        /* The same authenticated PN is a replay; a fresh PN with a corrupt
         * encrypted body is a decrypt failure. Neither may reach Ethernet. */
        assert(!wlan_mgr_rx_raw_80211(bcast_tkip_cipher, btc_len, -45));
        assert(!wlan_poll_rx(dev));
        btc_len = sizeof(bcast_tkip_cipher);
        assert(!wlan_tkip_encrypt_frame(tkip_gtk, 2, 1, bcast_wlan_plain,
                                       (size_t)bp_len, bcast_tkip_cipher, &btc_len));
        bcast_tkip_cipher[btc_len - 1] ^= 1;
        assert(!wlan_mgr_rx_raw_80211(bcast_tkip_cipher, btc_len, -45));
        assert(!wlan_poll_rx(dev));
        dn = wlan_rx_profile_format(diag, sizeof(diag) - 1); diag[dn] = 0;
        assert(diag_counter(diag, "group-ok=") == ok0 + 1);
        assert(diag_counter(diag, "group-replay=") == replay0 + 1);
        assert(diag_counter(diag, "group-decrypt-fail=") == fail0 + 1);
        assert(diag_counter(diag, " arp=") == arp0 + 1);
        assert(diag_counter(diag, "cipher=") == 2);
        assert(diag_counter(diag, "gtk-len=") == 32);
        assert(s_mock_tx_count == tx0); /* Snapshot never polls/sends radio. */
        for (size_t cap = 0; cap < sizeof(diag); ++cap) {
            memset(diag, 0x5a, sizeof(diag));
            dn = wlan_rx_profile_format(diag, cap);
            assert(dn <= cap && diag[cap] == 0x5a);
            if (dn) assert(diag[dn - 1] == '\n');
        }
        puts("  [PASS] RX diagnostics distinguish authenticated group ARP, replay and corrupt ciphertext without radio calls");
    }
    printf("  [PASS] Mixed-mode TKIP group broadcast ARP frame decryption verified\n");

    /* 20. Test WPA3-Personal (SAE / Dragonfly Handshake) Full Connection Flow */
    assert(wlan_mgr_disconnect() == 0);
    uint8_t sae_bssid[6] = { 0x00, 0x33, 0x55, 0x77, 0x99, 0xBB };
    {
        uint8_t sae_beacon[256];
        size_t sb_len = build_mock_beacon(sae_beacon, sae_bssid, "FortressWPA3", 36);
        /* Patch RSN IE AKM to 0x08 (SAE) and RSN caps to 0x00C0 (MFPC|MFPR) */
        size_t off = sizeof(ieee80211_hdr_t) + 12;
        while (off + 2 <= sb_len) {
            if (sae_beacon[off] == IEEE80211_IE_RSN) {
                sae_beacon[off + 19] = 0x08; /* AKM = SAE */
                sae_beacon[off + 20] = 0xC0; /* MFPC | MFPR */
                break;
            }
            off += 2 + sae_beacon[off + 1];
        }
        assert(wlan_mgr_rx_raw_80211(sae_beacon, sb_len, -38) == 0);
    }

    assert(wlan_mgr_connect("FortressWPA3", "DragonflySecret2026!", sae_bssid) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_AUTHENTICATING);

    /* Verify Station transmitted SAE Commit (Auth Alg = 3, Seq = 1, 66-byte payload) */
    assert(s_mock_last_tx_len == sizeof(ieee80211_hdr_t) + 6 + WLAN_SAE_COMMIT_LEN);
    assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 0] == 3); /* IEEE80211_AUTH_ALG_SAE */
    assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 2] == 1); /* Seq 1 */

    uint8_t sta_sae_commit[WLAN_SAE_COMMIT_LEN];
    memcpy(sta_sae_commit, s_mock_last_tx + sizeof(ieee80211_hdr_t) + 6, WLAN_SAE_COMMIT_LEN);

    /* Mock AP runs SAE P-256 Commit + Confirm */
    wlan_sae_ctx_t ap_sae;
    assert(wlan_sae_derive_pwe(&ap_sae, sae_bssid, s_mock_sta_mac, "DragonflySecret2026!") == 0);
    uint8_t ap_sae_commit[WLAN_SAE_COMMIT_LEN];
    assert(wlan_sae_build_commit(&ap_sae, ap_sae_commit) == 0);
    assert(wlan_sae_process_commit(&ap_sae, sta_sae_commit, sizeof(sta_sae_commit)) == 0);

    uint8_t ap_commit_frame[256];
    size_t ap_cf_len = net_80211_build_sae_auth(ap_commit_frame, sizeof(ap_commit_frame),
                                                sae_bssid, s_mock_sta_mac, 1, 0,
                                                ap_sae_commit, sizeof(ap_sae_commit));
    /* Fix addr1 = sta_mac, addr2 = sae_bssid, addr3 = sae_bssid */
    memcpy(((ieee80211_hdr_t *)ap_commit_frame)->addr1, s_mock_sta_mac, 6);
    memcpy(((ieee80211_hdr_t *)ap_commit_frame)->addr2, sae_bssid, 6);
    memcpy(((ieee80211_hdr_t *)ap_commit_frame)->addr3, sae_bssid, 6);

    assert(wlan_mgr_rx_raw_80211(ap_commit_frame, ap_cf_len, -38) == 0);

    /* Verify Station transmitted SAE Confirm (Auth Alg = 3, Seq = 2, 34-byte payload) */
    assert(s_mock_last_tx_len == sizeof(ieee80211_hdr_t) + 6 + WLAN_SAE_CONFIRM_LEN);
    assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 0] == 3);
    assert(s_mock_last_tx[sizeof(ieee80211_hdr_t) + 2] == 2); /* Seq 2 */
    assert(wlan_sae_verify_confirm(&ap_sae, s_mock_last_tx + sizeof(ieee80211_hdr_t) + 6, WLAN_SAE_CONFIRM_LEN) == 0);

    /* AP sends SAE Confirm (Seq = 2) -> Station transitions to ASSOCIATING */
    uint8_t ap_sae_confirm[WLAN_SAE_CONFIRM_LEN];
    assert(wlan_sae_build_confirm(&ap_sae, ap_sae_confirm) == 0);
    uint8_t ap_confirm_frame[256];
    size_t ap_cnf_len = net_80211_build_sae_auth(ap_confirm_frame, sizeof(ap_confirm_frame),
                                                 sae_bssid, s_mock_sta_mac, 2, 0,
                                                 ap_sae_confirm, sizeof(ap_sae_confirm));
    memcpy(((ieee80211_hdr_t *)ap_confirm_frame)->addr1, s_mock_sta_mac, 6);
    memcpy(((ieee80211_hdr_t *)ap_confirm_frame)->addr2, sae_bssid, 6);
    memcpy(((ieee80211_hdr_t *)ap_confirm_frame)->addr3, sae_bssid, 6);

    assert(wlan_mgr_rx_raw_80211(ap_confirm_frame, ap_cnf_len, -38) == 0);
    assert(wlan_mgr_status(&st) == 0);
    assert(st.state == WIFI_STATUS_ASSOCIATING);
    printf("  [PASS] WPA3-Personal SAE Commit-Confirm handshake & transition to ASSOCIATING verified\n");

    /* 21. Test Multi-Vendor Driver Architecture (rtw88 PCIe ID table & wlan_register_driver) */
    assert(rtw88_is_supported_device(RTW_PCI_VENDOR_ID, RTW_DEV_ID_8821CE));
    assert(rtw88_is_supported_device(RTW_PCI_VENDOR_ID, RTW_DEV_ID_8822CE));
    assert(rtw88_is_supported_device(RTW_PCI_VENDOR_ID, RTW_DEV_ID_8852AE));
    assert(rtw88_is_supported_device(ATH_PCI_VENDOR_ID, ATH_DEV_ID_QCA6174));
    assert(!rtw88_is_supported_device(0x1234, 0x5678));
    assert(wlan_register_driver(rtw88_get_mac_ops()) == 0);
    printf("  [PASS] Multi-vendor wlan_mac_ops_t registration & Realtek/Qualcomm PCIe table verified\n");

    printf("\n>>> ALL WIRELESS (802.11 / WPA2 / WPA3 / HT-VHT / IWLWIFI / RTW88) HOST TESTS PASSED (21/21) <<<\n");
    return 0;
}
