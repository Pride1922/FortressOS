#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "net_80211.h"

/* ============================================================================
 * 1. Frame Builders Verification
 * ============================================================================ */

static void test_frame_builders(void) {
    printf("[TEST 1] Testing 802.11 Management Frame Builders...\n");

    uint8_t sta_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t ap_bssid[6] = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    uint8_t buf[256];

    /* Probe Request */
    size_t len = net_80211_build_probe_req(buf, sizeof(buf), sta_mac, "TestSSID");
    assert(len > 0);
    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)buf;
    assert((hdr->frame_control & 0x00FF) == 0x40); /* MGMT, PROBE_REQ */
    assert(memcmp(hdr->addr2, sta_mac, 6) == 0);
    assert(buf[sizeof(ieee80211_hdr_t)] == IEEE80211_IE_SSID);
    assert(buf[sizeof(ieee80211_hdr_t) + 1] == 8);
    assert(memcmp(&buf[sizeof(ieee80211_hdr_t) + 2], "TestSSID", 8) == 0);

    /* Auth Request */
    len = net_80211_build_auth_req(buf, sizeof(buf), sta_mac, ap_bssid);
    assert(len == sizeof(ieee80211_hdr_t) + 6);
    hdr = (ieee80211_hdr_t *)buf;
    assert((hdr->frame_control & 0x00FF) == 0xB0); /* MGMT, AUTH */
    assert(memcmp(hdr->addr1, ap_bssid, 6) == 0);
    assert(memcmp(hdr->addr2, sta_mac, 6) == 0);
    assert(memcmp(hdr->addr3, ap_bssid, 6) == 0);
    assert(buf[sizeof(ieee80211_hdr_t) + 2] == 0x01); /* Auth Seq 1 */

    /* Assoc Request */
    len = net_80211_build_assoc_req(buf, sizeof(buf), sta_mac, ap_bssid, "TestSSID");
    assert(len > 0);
    hdr = (ieee80211_hdr_t *)buf;
    assert((hdr->frame_control & 0x00FF) == 0x00); /* MGMT, ASSOC_REQ */
    assert(memcmp(hdr->addr1, ap_bssid, 6) == 0);

    /* Verify RSN IE inside Assoc Req */
    bool found_rsn = false;
    size_t off = sizeof(ieee80211_hdr_t) + 4;
    while (off + 2 <= len) {
        if (buf[off] == IEEE80211_IE_RSN) {
            found_rsn = true;
            assert(buf[off + 1] == 20);
            break;
        }
        off += 2 + buf[off + 1];
    }
    assert(found_rsn);

    /* Deauth Frame */
    len = net_80211_build_deauth(buf, sizeof(buf), sta_mac, ap_bssid, 3);
    assert(len == sizeof(ieee80211_hdr_t) + 2);
    hdr = (ieee80211_hdr_t *)buf;
    assert((hdr->frame_control & 0x00FF) == 0xC0); /* MGMT, DEAUTH */
    assert(buf[sizeof(ieee80211_hdr_t)] == 0x03);

    printf("[TEST 1] PASS: Management frames and RSN IEs built accurately.\n");
}

/* ============================================================================
 * 2. 802.3 <--> 802.11 LLC/SNAP Bridging Roundtrip
 * ============================================================================ */

static void test_bridging_roundtrip(void) {
    printf("[TEST 2] Testing 802.3 <--> 802.11 LLC/SNAP bridging roundtrip...\n");

    uint8_t eth_orig[100];
    uint8_t dst_mac[6] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t bssid[6]   = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};

    memcpy(eth_orig + 0, dst_mac, 6);
    memcpy(eth_orig + 6, src_mac, 6);
    eth_orig[12] = 0x08; eth_orig[13] = 0x00; /* IPv4 */
    for (int i = 14; i < 100; i++) eth_orig[i] = (uint8_t)(i ^ 0x3C);

    /* Encapsulate to 802.11 QoS Data */
    uint8_t wlan_frame[256];
    int wlan_len = net_80211_encapsulate(eth_orig, 100, bssid, src_mac, 42, wlan_frame, sizeof(wlan_frame));
    assert(wlan_len == (int)(sizeof(ieee80211_qos_hdr_t) + sizeof(llc_snap_hdr_t) + 86));

    /* Verify 802.11 QoS Data Header fields */
    ieee80211_qos_hdr_t *hdr = (ieee80211_qos_hdr_t *)wlan_frame;
    assert(hdr->frame_control & IEEE80211_FCTL_TODS);
    assert(memcmp(hdr->addr1, bssid, 6) == 0);
    assert(memcmp(hdr->addr2, src_mac, 6) == 0);
    assert(memcmp(hdr->addr3, dst_mac, 6) == 0);

    /* Simulate AP transmission to Station (FromDS=1, ToDS=0) */
    hdr->frame_control = (IEEE80211_TYPE_DATA << 2) |
                         (IEEE80211_SUBTYPE_QOS_DATA << 4) |
                         IEEE80211_FCTL_FROMDS;
    memcpy(hdr->addr1, dst_mac, 6); /* Station is Receiver */
    memcpy(hdr->addr2, bssid, 6);   /* BSSID is Transmitter */
    memcpy(hdr->addr3, src_mac, 6); /* Original Source */

    /* Decapsulate back to 802.3 */
    uint8_t eth_recovered[256];
    int eth_len = net_80211_decapsulate(wlan_frame, (size_t)wlan_len, eth_recovered, sizeof(eth_recovered));
    assert(eth_len == 100);

    /* Must match original Ethernet frame byte-for-byte */
    assert(memcmp(eth_orig, eth_recovered, 100) == 0);

    printf("[TEST 2] PASS: Ethernet <--> 802.11 LLC/SNAP roundtrip matches exactly.\n");
}

/* ============================================================================
 * 3. Beacon & Probe Response Parsing
 * ============================================================================ */

static void test_beacon_parser(void) {
    printf("[TEST 3] Testing Beacon and Probe Response parsing...\n");

    uint8_t frame[256];
    ieee80211_hdr_t *hdr = (ieee80211_hdr_t *)frame;
    memset(hdr, 0, sizeof(ieee80211_hdr_t));
    /* Frame Control: MGMT (0), BEACON (8) -> 0x0080 */
    hdr->frame_control = (IEEE80211_TYPE_MGMT << 2) | (IEEE80211_SUBTYPE_BEACON << 4);
    memset(hdr->addr1, 0xFF, 6);
    uint8_t ap_mac[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};
    memcpy(hdr->addr2, ap_mac, 6);
    memcpy(hdr->addr3, ap_mac, 6);

    uint8_t *body = frame + sizeof(ieee80211_hdr_t);
    /* 8 bytes timestamp */
    memset(body, 0, 8);
    /* 2 bytes beacon interval (100 TU) */
    body[8] = 100; body[9] = 0;
    /* 2 bytes capabilities (ESS | Privacy = 0x0011) */
    body[10] = 0x11; body[11] = 0x00;

    uint8_t *ie = body + 12;
    /* SSID: "OfficeWiFi" */
    *ie++ = IEEE80211_IE_SSID;
    *ie++ = 10;
    memcpy(ie, "OfficeWiFi", 10);
    ie += 10;

    /* Channel: 6 */
    *ie++ = IEEE80211_IE_DS_PARAM;
    *ie++ = 1;
    *ie++ = 6;

    /* RSN IE (WPA2-PSK) */
    *ie++ = IEEE80211_IE_RSN;
    *ie++ = 20;
    memset(ie, 0, 20);
    ie += 20;

    size_t frame_len = (size_t)(ie - frame);

    wlan_bss_entry_t bss;
    bool parsed = net_80211_parse_beacon_or_probe(frame, frame_len, -65, &bss);
    assert(parsed);
    assert(memcmp(bss.bssid, ap_mac, 6) == 0);
    assert(strcmp(bss.ssid, "OfficeWiFi") == 0);
    assert(bss.channel == 6);
    assert(bss.rssi == -65);
    assert(bss.has_wpa2_psk == true);
    assert(bss.beacon_interval == 100);

    printf("[TEST 3] PASS: Beacon parsing, SSID, channel, and RSN detection verified.\n");
}

/* ============================================================================
 * 4. BSS Scan Table Operations
 * ============================================================================ */

static void test_bss_table(void) {
    printf("[TEST 4] Testing BSS Scan Table insertion, deduplication, and lookup...\n");

    wlan_bss_table_t table;
    wlan_bss_table_init(&table);
    assert(table.count == 0);

    wlan_bss_entry_t b1 = {
        .bssid = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        .ssid = "HomeWiFi",
        .channel = 1,
        .rssi = -50,
        .has_wpa2_psk = true,
    };
    assert(wlan_bss_table_update(&table, &b1));
    assert(table.count == 1);

    /* Update same BSSID with new RSSI */
    b1.rssi = -42;
    assert(wlan_bss_table_update(&table, &b1));
    assert(table.count == 1); /* Count unchanged */
    assert(table.entries[0].rssi == -42);

    /* Add distinct AP */
    wlan_bss_entry_t b2 = {
        .bssid = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE},
        .ssid = "GuestWiFi",
        .channel = 11,
        .rssi = -75,
        .has_wpa2_psk = true,
    };
    assert(wlan_bss_table_update(&table, &b2));
    assert(table.count == 2);

    /* Lookup by SSID */
    const wlan_bss_entry_t *found = wlan_bss_table_find(&table, "HomeWiFi");
    assert(found != NULL);
    assert(found->channel == 1);
    assert(found->rssi == -42);

    found = wlan_bss_table_find(&table, "GuestWiFi");
    assert(found != NULL);
    assert(found->channel == 11);

    found = wlan_bss_table_find(&table, "NonExistent");
    assert(found == NULL);

    /* Multi-BSSID test: add second AP with same SSID "HomeWiFi" but stronger RSSI */
    wlan_bss_entry_t b3 = {
        .bssid = {0x00, 0x11, 0x22, 0x33, 0x44, 0x99},
        .ssid = "HomeWiFi",
        .channel = 36,
        .rssi = -30, /* Stronger than b1 (-42) */
        .has_wpa2_psk = true,
    };
    assert(wlan_bss_table_update(&table, &b3));
    assert(table.count == 3);

    found = wlan_bss_table_find(&table, "HomeWiFi");
    assert(found != NULL);
    assert(found->channel == 36);
    assert(found->rssi == -30);
    assert(memcmp(found->bssid, b3.bssid, 6) == 0);

    printf("[TEST 4] PASS: BSS scan table operations and multi-BSSID best-RSSI verified.\n");
}

int main(void) {
    printf("================================================================\n");
    printf("  FortressOS 802.11 MAC Frame & State Machine (WIFI-1) Tests     \n");
    printf("================================================================\n");

    test_frame_builders();
    test_bridging_roundtrip();
    test_beacon_parser();
    test_bss_table();

    printf("================================================================\n");
    printf("  ALL 4 WLAN MAC TESTS PASSED (100%%)                            \n");
    printf("================================================================\n");
    return 0;
}
