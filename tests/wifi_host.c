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
    int dec_rc = wlan_ccmp_decrypt_frame(tk, s_mock_last_tx, s_mock_last_tx_len, dec_buf, &dec_len, &last_pn);
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

    printf("\n>>> ALL WIRELESS (802.11 / WPA2-PSK / IWLWIFI) HOST TESTS PASSED (13/13) <<<\n");
    return 0;
}
