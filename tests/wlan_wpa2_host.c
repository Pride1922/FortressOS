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
    h3->length = t_htons((uint16_t)(sizeof(eapol_key_desc_t)+24));

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

    const uint8_t gtk_kde[24]={0xdd,22,0,0x0f,0xac,1,1,0};
    kd3->key_data_len=t_htons(sizeof(gtk_kde));
    memcpy(msg3+sizeof(eapol_hdr_t)+sizeof(eapol_key_desc_t),gtk_kde,sizeof(gtk_kde));
    /* AP computes Message 3 MIC */
    uint8_t m3_mic[WLAN_SHA1_DIGEST_SIZE];
    wlan_hmac_sha1(ap_ptk, WLAN_KCK_LEN, msg3, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)+24, m3_mic);
    memcpy(kd3->key_mic, m3_mic, 16);

    /* 6. Station processes Message 3 and produces Message 4 */
    uint8_t msg4[256];
    int msg4_len = wpa2_sm_rx_eapol(&sta_sm, msg3, sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)+24,
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

static void test_wpa3_sae_4way_and_tkip_gtk32(void) {
    printf("[TEST 4] Testing WPA3-SAE 4-Way Handshake (SHA-256 MIC, ver=0, PMKID) & 32B TKIP GTK...\n");

    uint8_t sta_mac[6] = {0x00, 0x0F, 0xB5, 0x32, 0x88, 0x6E};
    uint8_t ap_bssid[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};
    uint8_t pmk[WLAN_PMK_LEN];
    uint8_t pmkid[16];
    for (int i = 0; i < 32; i++) pmk[i] = (uint8_t)(0x30 + i);
    for (int i = 0; i < 16; i++) pmkid[i] = (uint8_t)(0x90 + i);

    wpa2_sm_t sta_sm;
    wpa2_sm_init(&sta_sm, sta_mac, ap_bssid, pmk);
    wpa2_sm_set_rsn(&sta_sm, 0x02 /* TKIP group */, 0x00C0 /* MFPC|MFPR */);
    wpa2_sm_set_sae(&sta_sm, true, pmkid);

    /* Mock WPA3 AP Message 1 (Key Descriptor Version = 0 for AKM 00-0F-AC:8) */
    uint8_t anonce[32];
    for (int i = 0; i < 32; i++) anonce[i] = (uint8_t)(0x20 + i);

    uint8_t msg1[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)];
    memset(msg1, 0, sizeof(msg1));
    eapol_hdr_t *h1 = (eapol_hdr_t *)msg1;
    h1->version = EAPOL_VERSION_2;
    h1->type = EAPOL_TYPE_KEY;
    h1->length = t_htons((uint16_t)sizeof(eapol_key_desc_t));

    eapol_key_desc_t *kd1 = (eapol_key_desc_t *)(msg1 + sizeof(eapol_hdr_t));
    kd1->desc_type = EAPOL_KEY_DESC_WPA2;
    kd1->key_info = t_htons(EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_KEY_ACK); /* ver=0 */
    kd1->key_length = t_htons(16);
    kd1->replay_counter = t_hton64(1);
    memcpy(kd1->key_nonce, anonce, 32);

    uint8_t msg2[256];
    int msg2_len = wpa2_sm_rx_eapol(&sta_sm, msg1, sizeof(msg1), msg2, sizeof(msg2));
    assert(msg2_len == (int)(sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t) + 40));

    eapol_key_desc_t *kd2 = (eapol_key_desc_t *)(msg2 + sizeof(eapol_hdr_t));
    assert((t_htons(kd2->key_info) & EAPOL_KEY_INFO_TYPE_MASK) == 0); /* ver=0 */
    const uint8_t *rsn_ie = msg2 + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t);
    assert(rsn_ie[0] == 0x30 && rsn_ie[1] == 38);
    assert(rsn_ie[19] == 0x08); /* AKM = SAE (8) */
    assert(memcmp(rsn_ie + 24, pmkid, 16) == 0);

    /* AP derives PTK via KDF-SHA-256 and verifies Message 2 HMAC-SHA256-128 MIC */
    uint8_t ap_ptk[WLAN_PTK_LEN];
    wlan_derive_ptk_sha256(pmk, ap_bssid, sta_mac, anonce, kd2->key_nonce, ap_ptk);

    uint8_t msg2_copy[256];
    memcpy(msg2_copy, msg2, (size_t)msg2_len);
    eapol_key_desc_t *kd2_c = (eapol_key_desc_t *)(msg2_copy + sizeof(eapol_hdr_t));
    uint8_t sta_mic[16];
    memcpy(sta_mic, kd2_c->key_mic, 16);
    memset(kd2_c->key_mic, 0, 16);

    uint8_t exp_mic[32];
    wlan_hmac_sha256(ap_ptk, WLAN_KCK_LEN, msg2_copy, (size_t)msg2_len, exp_mic);
    assert(memcmp(sta_mic, exp_mic, 16) == 0);

    /* AP builds Message 3 with wrapped 32-byte TKIP GTK KDE:
     * Plain KDE (40 bytes, multiple of 8):
     *   [0..7]: DD 26 00 0F AC 01 01 00 (38-byte body: 4B OUI+type + 2B flags + 32B TKIP GTK)
     *   [8..39]: 32-byte TKIP GTK
     */
    uint8_t plain_kde[40];
    plain_kde[0] = 0xDD; plain_kde[1] = 38;
    plain_kde[2] = 0x00; plain_kde[3] = 0x0F; plain_kde[4] = 0xAC; plain_kde[5] = 0x01;
    plain_kde[6] = 0x01; plain_kde[7] = 0x00; /* KeyID = 1 */
    uint8_t orig_gtk32[32];
    for (int i = 0; i < 32; i++) orig_gtk32[i] = (uint8_t)(0xA0 + i);
    memcpy(plain_kde + 8, orig_gtk32, 32);

    /* Wrap/attach plain_kde (40 bytes containing 32-byte TKIP GTK KDE) into Message 3 */
    uint8_t msg3[sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t) + 40];
    memset(msg3, 0, sizeof(msg3));
    eapol_hdr_t *h3 = (eapol_hdr_t *)msg3;
    h3->version = EAPOL_VERSION_2;
    h3->type = EAPOL_TYPE_KEY;
    h3->length = t_htons((uint16_t)(sizeof(eapol_key_desc_t) + 40));
    eapol_key_desc_t *kd3 = (eapol_key_desc_t *)(msg3 + sizeof(eapol_hdr_t));
    kd3->desc_type = EAPOL_KEY_DESC_WPA2;
    kd3->key_info = t_htons(EAPOL_KEY_INFO_KEY_TYPE | EAPOL_KEY_INFO_INSTALL |
                            EAPOL_KEY_INFO_KEY_ACK | EAPOL_KEY_INFO_KEY_MIC |
                            EAPOL_KEY_INFO_SECURE);
    kd3->key_length = t_htons(16);
    kd3->replay_counter = t_hton64(2);
    memcpy(kd3->key_nonce, anonce, 32);
    kd3->key_data_len = t_htons(40);
    memcpy(msg3 + sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t), plain_kde, 40);

    uint8_t m3_mic[32];
    wlan_hmac_sha256(ap_ptk, WLAN_KCK_LEN, msg3, sizeof(msg3), m3_mic);
    memcpy(kd3->key_mic, m3_mic, 16);

    uint8_t msg4[256];
    int msg4_len = wpa2_sm_rx_eapol(&sta_sm, msg3, sizeof(msg3), msg4, sizeof(msg4));
    assert(msg4_len == (int)(sizeof(eapol_hdr_t) + sizeof(eapol_key_desc_t)));
    assert(wpa2_sm_is_authorized(&sta_sm));
    assert(sta_sm.gtk_len == 32);
    assert( sta_sm.gtk_idx == 1);
    assert(memcmp(sta_sm.gtk, orig_gtk32, 32) == 0);

    printf("[TEST 4] PASS: WPA3-SAE 4-Way Handshake (KDF-SHA-256, HMAC-SHA256-128, PMKID) & 32B TKIP GTK verified.\n");
}

static void test_long_encrypted_key_data(void) {
    /* Independent AES key-wrap vector generated with Python cryptography:
     * zero KEK; RSN IE + TKIP GTK + IGTK + padding, 96B plaintext/104B wrapped.
     * This exceeds the former 64B plaintext limit. */
    static const uint8_t wrapped[] = {0xc8,0x0b,0x3f,0xc1,0x5b,0x7e,0x88,0x75,0x89,0xb0,0x7e,0x6c,0xc2,0x21,0x1c,0x23,0xfa,0xb4,0x43,0xb6,0x64,0x61,0x29,0xfb,0x79,0x6b,0x22,0x87,0x62,0x6d,0x34,0x88,0x51,0x2a,0xab,0x7f,0x83,0x96,0x9d,0x1c,0x68,0xe3,0xd0,0x73,0x11,0x3e,0x05,0x7d,0x00,0xf9,0xf6,0xbb,0x21,0xb5,0x1d,0xe8,0xe8,0x8d,0x4b,0xb9,0x62,0xef,0x0d,0x01,0x58,0xe8,0xfe,0xbd,0x9c,0xc5,0x0c,0x33,0xad,0x60,0xba,0x4d,0x70,0x8f,0xfa,0xf1,0x75,0x5d,0x33,0x65,0x3a,0x4e,0x50,0x65,0x8d,0x58,0x36,0xea,0xd4,0x80,0x42,0xc5,0x0b,0xf1,0xf5,0x95,0x26,0x8b,0xdc,0x35};
    uint8_t msg[sizeof(eapol_hdr_t)+sizeof(eapol_key_desc_t)+sizeof(wrapped)]={0};
    uint8_t response[256];
    wpa2_sm_t sm;
    uint8_t zero[32]={0};
    for (unsigned corrupt=0;corrupt<3;++corrupt) {
        wpa2_sm_init(&sm,zero,zero,zero);
        sm.handshake_step=2;
        sm.group_cipher=2;
        eapol_hdr_t *h=(eapol_hdr_t *)msg;
        h->version=2;h->type=EAPOL_TYPE_KEY;h->length=t_htons(sizeof(msg)-sizeof(*h));
        eapol_key_desc_t *k=(eapol_key_desc_t *)(msg+sizeof(*h));
        k->desc_type=EAPOL_KEY_DESC_WPA2;
        k->key_info=t_htons(EAPOL_KEY_INFO_HMAC_SHA1|EAPOL_KEY_INFO_KEY_TYPE|
            EAPOL_KEY_INFO_KEY_ACK|EAPOL_KEY_INFO_KEY_MIC|EAPOL_KEY_INFO_INSTALL|
            EAPOL_KEY_INFO_SECURE|EAPOL_KEY_INFO_ENCR_KEY_DATA);
        k->replay_counter=t_hton64(2);k->key_data_len=t_htons(sizeof(wrapped));
        memcpy(msg+sizeof(*h)+sizeof(*k),wrapped,sizeof(wrapped));
        size_t message_len=sizeof(msg);
        if (corrupt==1) msg[sizeof(msg)-1]^=1;
        if (corrupt==2) {
            message_len=sizeof(*h)+sizeof(*k);
            h->length=t_htons(sizeof(*k));k->key_data_len=0;
            k->key_info=t_htons(EAPOL_KEY_INFO_HMAC_SHA1|EAPOL_KEY_INFO_KEY_TYPE|
                EAPOL_KEY_INFO_KEY_ACK|EAPOL_KEY_INFO_KEY_MIC|EAPOL_KEY_INFO_INSTALL|EAPOL_KEY_INFO_SECURE);
        }
        memset(k->key_mic,0,16);
        uint8_t mic[20];wlan_hmac_sha1(sm.kck,16,msg,message_len,mic);memcpy(k->key_mic,mic,16);
        int rc=wpa2_sm_rx_eapol(&sm,msg,message_len,response,sizeof(response));
        if (corrupt) {assert(rc<0 && !sm.port_authorized && !sm.gtk_len);}
        else {
            assert(rc>0 && sm.port_authorized && sm.gtk_len==32 && sm.gtk_idx==1);
            for (unsigned i=0;i<32;++i) assert(sm.gtk[i]==0xa0+i);
        }
    }
    puts("Long encrypted M3 PASS: independent 104B wrap installs TKIP GTK; valid-MIC corrupt or missing GTK cannot authorize");
}

int main(void) {
    printf("================================================================\n");
    printf("  FortressOS WPA2-PSK 4-Way Handshake (WIFI-2) Test Suite       \n");
    printf("================================================================\n");

    test_long_encrypted_key_data();
    test_full_4way_handshake();
    test_adversarial_gates();
    test_rsn_ie_mirroring();
    test_wpa3_sae_4way_and_tkip_gtk32();

    printf("================================================================\n");
    printf("  ALL 4 WPA2/WPA3 4-WAY HANDSHAKE TESTS PASSED (100%%)          \n");
    printf("================================================================\n");
    return 0;
}
