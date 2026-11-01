#define WLAN_HOST_TEST 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "wlan_wpa2.h"

/* Endianness helpers for test AP */
static inline uint16_t t_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint64_t t_hton64(uint64_t v) { return __builtin_bswap64(v); }

static void test_full_4way_handshake(void) {
    printf("[TEST 1] Testing complete WPA2-PSK 4-Way Handshake state machine...\n");

    /* 1. Setup Station and AP parameters */
    uint8_t sta_mac[6] = {0x00, 0x0F, 0xB5, 0x32, 0x88, 0x6E};
    uint8_t ap_bssid[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};

    const char *passphrase = "SecretWiFi2026";
    const uint8_t ssid[12] = {'F', 'o', 'r', 't', 'r', 'e', 's', 's', 'W', 'L', 'A', 'N'};

    uint8_t pmk[WLAN_PMK_LEN];
    assert(wlan_pbkdf2_sha1(passphrase, ssid, 12, 4096, pmk, WLAN_PMK_LEN) == 0);

    wpa2_sm_t sta_sm;
    wpa2_sm_init(&sta_sm, sta_mac, ap_bssid, pmk);
    assert(!wpa2_sm_is_authorized(&sta_sm));

    /* 2. Mock AP constructs Message 1 */
    uint8_t anonce[32];
    for (int i = 0; i < 32; i++) anonce[i] = (uint8_t)(i + 0x10);

    uint8_t msg1[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)];
    memset(msg1, 0, sizeof(msg1));

    eapol_hdr_t *h1 = (eapol_hdr_t *)msg1;
    h1->version = EAPOL_VERSION_1;
    h1->type = EAPOL_TYPE_KEY;
    h1->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));

    eapol_key_desc_t *kd1 = (eapol_key_desc_t *)(msg1 + sizeof(eapol_hdr_t));
    kd1->desc_type = EAPOL_KEY_DESC_WPA2;
    /* Key Info: HMAC-SHA1 | Pairwise | KeyACK (0x008A) */
    kd1->key_info = t_htons(EAPOL_KEY_INFO_HMAC_SHA1 |
                            EAPOL_KEY_INFO_KEY_TYPE |
                            EAPOL_KEY_INFO_KEY_ACK);
    kd1->key_length = t_htons(16);
    kd1->replay_counter = t_hton64(1);
    memcpy(kd1->key_nonce, anonce, 32);

    /* 3. Station processes Message 1 and produces Message 2 */
    uint8_t msg2[256];
    int msg2_len = wpa2_sm_rx_eapol(&sta_sm, msg1, sizeof(msg1), msg2, sizeof(msg2));
    assert(msg2_len > 0);
    assert(!wpa2_sm_is_authorized(&sta_sm)); /* Not yet authorized */

    /* 4. Mock AP verifies Station's Message 2 */
    eapol_key_desc_t *kd2 = (eapol_key_desc_t *)(msg2 + sizeof(eapol_hdr_t));
    assert(kd2->desc_type == EAPOL_KEY_DESC_WPA2);
    assert(kd2->replay_counter == t_hton64(1)); /* Replay counter echoed */

    /* AP derives PTK using Station's SNonce from Message 2 */
    uint8_t snonce[32];
    memcpy(snonce, kd2->key_nonce, 32);

    uint8_t ap_ptk[WLAN_PTK_LEN];
    wlan_derive_ptk(pmk, ap_bssid, sta_mac, anonce, snonce, ap_ptk);

    /* AP verifies Message 2 MIC */
    uint8_t msg2_verify[256];
    memcpy(msg2_verify, msg2, msg2_len);
    eapol_key_desc_t *kd2_v = (eapol_key_desc_t *)(msg2_verify + sizeof(eapol_hdr_t));
    uint8_t sta_mic[16];
    memcpy(sta_mic, kd2_v->key_mic, 16);
    memset(kd2_v->key_mic, 0, 16);

    uint8_t computed_mic[WLAN_SHA1_DIGEST_SIZE];
    wlan_hmac_sha1(ap_ptk /* KCK */, WLAN_KCK_LEN, msg2_verify, msg2_len, computed_mic);
    assert(memcmp(sta_mic, computed_mic, 16) == 0);

    /* 5. Mock AP constructs Message 3 */
    uint8_t msg3[256];
    memset(msg3, 0, sizeof(msg3));

    eapol_hdr_t *h3 = (eapol_hdr_t *)msg3;
    h3->version = EAPOL_VERSION_1;
    h3->type = EAPOL_TYPE_KEY;
    h3->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));

    eapol_key_desc_t *kd3 = (eapol_key_desc_t *)(msg3 + sizeof(eapol_hdr_t));
    kd3->desc_type = EAPOL_KEY_DESC_WPA2;
    /* Key Info: HMAC-SHA1 | Pairwise | Install | KeyACK | KeyMIC | Secure (0x03CA) */
    kd3->key_info = t_htons(EAPOL_KEY_INFO_HMAC_SHA1 |
                            EAPOL_KEY_INFO_KEY_TYPE |
                            EAPOL_KEY_INFO_INSTALL |
                            EAPOL_KEY_INFO_KEY_ACK |
                            EAPOL_KEY_INFO_KEY_MIC |
                            EAPOL_KEY_INFO_SECURE);
    kd3->key_length = t_htons(16);
    kd3->replay_counter = t_hton64(2);
    memcpy(kd3->key_nonce, anonce, 32);

    /* AP computes Message 3 MIC */
    uint8_t m3_mic[WLAN_SHA1_DIGEST_SIZE];
    wlan_hmac_sha1(ap_ptk, WLAN_KCK_LEN, msg3, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t), m3_mic);
    memcpy(kd3->key_mic, m3_mic, 16);

    /* 6. Station processes Message 3 and produces Message 4 */
    uint8_t msg4[256];
    int msg4_len = wpa2_sm_rx_eapol(&sta_sm, msg3, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t),
                                    msg4, sizeof(msg4));
    assert(msg4_len > 0);

    /* Station must now be AUTHORIZED! */
    assert(wpa2_sm_is_authorized(&sta_sm));

    /* 7. AP verifies Message 4 MIC */
    eapol_key_desc_t *kd4 = (eapol_key_desc_t *)(msg4 + sizeof(eapol_hdr_t));
    assert(kd4->replay_counter == t_hton64(2));
    uint8_t m4_mic[16];
    memcpy(m4_mic, kd4->key_mic, 16);
    memset(kd4->key_mic, 0, 16);

    uint8_t ap_m4_mic[WLAN_SHA1_DIGEST_SIZE];
    wlan_hmac_sha1(ap_ptk, WLAN_KCK_LEN, msg4, msg4_len, ap_m4_mic);
    assert(memcmp(m4_mic, ap_m4_mic, 16) == 0);

    printf("[TEST 1] PASS: 4-Way Handshake completed, MICs verified, port AUTHORIZED.\n");
}

static void test_adversarial_gates(void) {
    printf("[TEST 2] Testing adversarial gates: replay counters and tampered MIC...\n");

    uint8_t sta_mac[6] = {0x00, 0x0F, 0xB5, 0x32, 0x88, 0x6E};
    uint8_t ap_bssid[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};
    uint8_t pmk[WLAN_PMK_LEN];
    memset(pmk, 0x77, WLAN_PMK_LEN);

    wpa2_sm_t sm;
    wpa2_sm_init(&sm, sta_mac, ap_bssid, pmk);

    /* Normal Message 1 */
    uint8_t msg1[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)];
    memset(msg1, 0, sizeof(msg1));
    eapol_hdr_t *h1 = (eapol_hdr_t *)msg1;
    h1->version = 1; h1->type = EAPOL_TYPE_KEY;
    h1->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));
    eapol_key_desc_t *kd1 = (eapol_key_desc_t *)(msg1 + sizeof(eapol_hdr_t));
    kd1->desc_type = EAPOL_KEY_DESC_WPA2;
    kd1->key_info = t_htons(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK);
    kd1->replay_counter = t_hton64(5);
    memset(kd1->key_nonce, 0x55, 32);

    uint8_t msg2[256];
    assert(wpa2_sm_rx_eapol(&sm, msg1, sizeof(msg1), msg2, sizeof(msg2)) > 0);

    /* Adversarial Case 1: Corrupted Message 3 MIC */
    uint8_t msg3_bad[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)];
    memset(msg3_bad, 0, sizeof(msg3_bad));
    eapol_hdr_t *h3 = (eapol_hdr_t *)msg3_bad;
    h3->version = 1; h3->type = EAPOL_TYPE_KEY;
    h3->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));
    eapol_key_desc_t *kd3 = (eapol_key_desc_t *)(msg3_bad + sizeof(eapol_hdr_t));
    kd3->desc_type = EAPOL_KEY_DESC_WPA2;
    kd3->key_info = t_htons(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE |
                            EAPOL_KEY_INFO_KEY_ACK | EAPOL_KEY_INFO_KEY_MIC);
    kd3->replay_counter = t_hton64(6);
    memset(kd3->key_mic, 0xAA, 16); /* Bogus MIC */

    uint8_t resp[256];
    assert(wpa2_sm_rx_eapol(&sm, msg3_bad, sizeof(msg3_bad), resp, sizeof(resp)) == -1);
    assert(!wpa2_sm_is_authorized(&sm));

    /* Adversarial Case 2: Replay counter regression */
    kd3->replay_counter = t_hton64(4); /* Less than 5 */
    assert(wpa2_sm_rx_eapol(&sm, msg3_bad, sizeof(msg3_bad), resp, sizeof(resp)) == -1);

    printf("[TEST 2] PASS: Replay counters and tampered MICs fail-closed.\n");
}

static void test_rsn_ie_mirroring(void) {
    printf("[TEST 3] Testing dynamic Group Cipher and PMF (MFPC) RSN IE mirroring in Message 2...\n");

    uint8_t sta_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t ap_bssid[6] = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    uint8_t pmk[WLAN_PMK_LEN];
    memset(pmk, 0x42, sizeof(pmk));

    /* Case A: Halloween network (TKIP group cipher 0x02, no PMF 0x0000) */
    wpa2_sm_t sm_tkip;
    wpa2_sm_init(&sm_tkip, sta_mac, ap_bssid, pmk);
    wpa2_sm_set_rsn(&sm_tkip, 0x02, 0x0000);

    uint8_t msg1[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)];
    memset(msg1, 0, sizeof(msg1));
    eapol_hdr_t *h1 = (eapol_hdr_t *)msg1;
    h1->version = 1; h1->type = EAPOL_TYPE_KEY;
    h1->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));
    eapol_key_desc_t *kd1 = (eapol_key_desc_t *)(msg1 + sizeof(eapol_hdr_t));
    kd1->desc_type = EAPOL_KEY_DESC_WPA2;
    kd1->key_info = t_htons(EAPOL_KEY_INFO_HMAC_SHA1 | EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK);
    kd1->replay_counter = t_hton64(1);
    memset(kd1->key_nonce, 0x11, 32);

    uint8_t msg2_tkip[256];
    int len_tkip = wpa2_sm_rx_eapol(&sm_tkip, msg1, sizeof(msg1), msg2_tkip, sizeof(msg2_tkip));
    assert(len_tkip > 0);

    const uint8_t *ie_tkip = msg2_tkip + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
    assert(ie_tkip[0] == 0x30 && ie_tkip[1] == 0x14); /* RSN IE */
    assert(ie_tkip[7] == 0x02);                       /* Group cipher = TKIP (0x02) */
    assert(ie_tkip[20] == 0x00 && ie_tkip[21] == 0x00);/* RSN caps = 0x0000 */

    /* Case B: Pixel Hotspot (CCMP group cipher 0x04, PMF MFPC 0x0080) */
    wpa2_sm_t sm_pmf;
    wpa2_sm_init(&sm_pmf, sta_mac, ap_bssid, pmk);
    wpa2_sm_set_rsn(&sm_pmf, 0x04, 0x0080);

    uint8_t msg2_pmf[256];
    int len_pmf = wpa2_sm_rx_eapol(&sm_pmf, msg1, sizeof(msg1), msg2_pmf, sizeof(msg2_pmf));
    assert(len_pmf > 0);

    const uint8_t *ie_pmf = msg2_pmf + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
    assert(ie_pmf[0] == 0x30 && ie_pmf[1] == 0x14);   /* RSN IE */
    assert(ie_pmf[7] == 0x04);                         /* Group cipher = CCMP (0x04) */
    assert(ie_pmf[20] == 0x80 && ie_pmf[21] == 0x00); /* RSN caps = 0x0080 (MFPC = 1) */

    printf("[TEST 3] PASS: Message 2 accurately mirrors Group Cipher and PMF (MFPC) capabilities.\n");
}

int main(void) {
    printf("================================================================\n");
    printf("  FortressOS WPA2-PSK 4-Way Handshake (WIFI-2) Test Suite       \n");
    printf("================================================================\n");

    test_full_4way_handshake();
    test_adversarial_gates();
    test_rsn_ie_mirroring();

    printf("================================================================\n");
    printf("  ALL WPA2-PSK 4-WAY HANDSHAKE TESTS PASSED (100%%)             \n");
    printf("================================================================\n");
    return 0;
}
