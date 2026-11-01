#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "wlan_crypto.h"

/* ============================================================================
 * 1. SHA-1 Official Test Vectors (RFC 3174 / FIPS 180-1)
 * ============================================================================ */

static void test_sha1(void) {
    printf("[TEST 1] Testing SHA-1 official test vectors (RFC 3174)...\n");

    /* Vector 1: "abc" */
    uint8_t d1[20];
    wlan_sha1("abc", 3, d1);
    const uint8_t exp1[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    assert(memcmp(d1, exp1, 20) == 0);

    /* Vector 2: "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" (56 bytes) */
    const char *str2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t d2[20];
    wlan_sha1(str2, strlen(str2), d2);
    const uint8_t exp2[20] = {
        0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e, 0xba, 0xae,
        0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1
    };
    assert(memcmp(d2, exp2, 20) == 0);

    printf("[TEST 1] PASS: SHA-1 standard vectors verified.\n");
}

/* ============================================================================
 * 2. HMAC-SHA-1 Official Test Vectors (RFC 2202)
 * ============================================================================ */

static void test_hmac_sha1(void) {
    printf("[TEST 2] Testing HMAC-SHA-1 test vectors (RFC 2202)...\n");

    /* Test Case 1: Key = 20 bytes 0x0b, Data = "Hi There" */
    uint8_t k1[20];
    memset(k1, 0x0b, 20);
    const char *data1 = "Hi There";
    uint8_t mac1[20];
    wlan_hmac_sha1(k1, 20, data1, strlen(data1), mac1);

    const uint8_t exp1[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
        0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00
    };
    assert(memcmp(mac1, exp1, 20) == 0);

    /* Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?" */
    const char *k2 = "Jefe";
    const char *data2 = "what do ya want for nothing?";
    uint8_t mac2[20];
    wlan_hmac_sha1(k2, strlen(k2), data2, strlen(data2), mac2);

    const uint8_t exp2[20] = {
        0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
        0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79
    };
    assert(memcmp(mac2, exp2, 20) == 0);

    printf("[TEST 2] PASS: HMAC-SHA-1 standard vectors verified.\n");
}

/* ============================================================================
 * 3. WPA2 PBKDF2-HMAC-SHA1 Key Derivation (RFC 6070 / IEEE 802.11i)
 * ============================================================================ */

static void test_pbkdf2_wpa2(void) {
    printf("[TEST 3] Testing WPA2 PBKDF2-HMAC-SHA1 key derivation (4096 iterations)...\n");

    /* RFC 6070 Test Vector:
     * P = "password"
     * S = "IEEE"
     * c = 4096
     * dkLen = 32
     * DK = f42c6fc52df0ebef9ebb4b90f30ab51beb3073d433a930db22f53f5f05928d45 */
    const char *pass = "password";
    const uint8_t ssid[4] = {'I', 'E', 'E', 'E'};
    uint8_t pmk[32];

    int ret = wlan_pbkdf2_sha1(pass, ssid, 4, 4096, pmk, 32);
    assert(ret == 0);

    const uint8_t exp_pmk[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
        0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
        0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
    };
    assert(memcmp(pmk, exp_pmk, 32) == 0);

    printf("[TEST 3] PASS: WPA2 PMK derivation matches RFC 6070 exact vector.\n");
}

/* ============================================================================
 * 4. PRF-512 and PTK Key Expansion (IEEE 802.11i)
 * ============================================================================ */

static void test_ptk_derivation(void) {
    printf("[TEST 4] Testing IEEE 802.11i PRF-512 and PTK derivation...\n");

    uint8_t pmk[32];
    memset(pmk, 0x55, 32);

    uint8_t addr_ap[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};
    uint8_t addr_sta[6] = {0x00, 0x0F, 0xB5, 0x32, 0x88, 0x6E};

    uint8_t anonce[32];
    memset(anonce, 0xAA, 32);
    uint8_t snonce[32];
    memset(snonce, 0xBB, 32);

    uint8_t ptk1[64];
    uint8_t ptk2[64];

    /* Address and Nonce ordering invariance */
    wlan_derive_ptk(pmk, addr_ap, addr_sta, anonce, snonce, ptk1);
    wlan_derive_ptk(pmk, addr_sta, addr_ap, snonce, anonce, ptk2);

    /* Must yield identical PTK regardless of caller order */
    assert(memcmp(ptk1, ptk2, 64) == 0);

    /* Distinct nonces must yield distinct PTKs */
    uint8_t snonce_diff[32];
    memset(snonce_diff, 0xBC, 32);
    uint8_t ptk_diff[64];
    wlan_derive_ptk(pmk, addr_ap, addr_sta, anonce, snonce_diff, ptk_diff);
    assert(memcmp(ptk1, ptk_diff, 64) != 0);

    printf("[TEST 4] PASS: PRF-512 and PTK ordering invariance verified.\n");
}

/* ============================================================================
 * 5. NIST AES Key Wrap (RFC 3394)
 * ============================================================================ */

static void test_aes_key_unwrap(void) {
    printf("[TEST 5] Testing NIST AES Key Wrap (RFC 3394) GTK unwrapping...\n");

    /* RFC 3394 Section 4.1: 128-bit key wrapping 128-bit key */
    const uint8_t kek[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    const uint8_t exp_plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    const uint8_t wrapped[24] = {
        0x1F, 0xA6, 0x8B, 0x0A, 0x81, 0x12, 0xB4, 0x47,
        0xAE, 0xF3, 0x4B, 0xD8, 0xFB, 0x5A, 0x7B, 0x82,
        0x9D, 0x3E, 0x86, 0x23, 0x71, 0xD2, 0xCF, 0xE5
    };

    uint8_t unwrap_buf[16] = {0};
    int ret = wlan_aes_unwrap(kek, wrapped, 24, unwrap_buf);
    assert(ret == 0);
    assert(memcmp(unwrap_buf, exp_plain, 16) == 0);

    /* Tampered wrapped key fails integrity check */
    uint8_t corrupt[24];
    memcpy(corrupt, wrapped, 24);
    corrupt[10] ^= 0x01;
    ret = wlan_aes_unwrap(kek, corrupt, 24, unwrap_buf);
    assert(ret == -1);

    printf("[TEST 5] PASS: AES Key Wrap (RFC 3394) unwrapping verified.\n");
}

/* ============================================================================
 * 6. IEEE 802.11i / RFC 3610 AES-128-CCMP Encryption & Decryption
 * ============================================================================ */

static void test_ccmp_roundtrip_and_tampering(void) {
    printf("[TEST 6] Testing AES-128-CCMP encryption, decryption, and tamper detection...\n");

    uint8_t tk[16] = {
        0xC9, 0x7C, 0x1F, 0x67, 0xCE, 0x37, 0x11, 0x85,
        0x51, 0x4A, 0x8A, 0x19, 0xF2, 0xBD, 0xD5, 0x2D
    };
    uint64_t pn = 0x000000000001ULL;

    /* Mock 802.11 AAD header (22 bytes, Duration excluded per IEEE 802.11) */
    uint8_t aad[22] = {
        0x08, 0x41,                         /* Frame Control (Data, ToDS, Protected) */
        0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80, /* A1: BSSID/AP */
        0x00, 0x0F, 0xB5, 0x32, 0x88, 0x6E, /* A2: Station MAC */
        0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80, /* A3: DA */
        0x10, 0x00                          /* Sequence Control */
    };

    /* Plaintext Ethernet IP packet (80 bytes) */
    uint8_t plain[80];
    for (int i = 0; i < 80; i++) plain[i] = (uint8_t)(i ^ 0x5A);

    uint8_t cipher[80] = {0};
    uint8_t mic[8] = {0};

    int enc_ret = wlan_ccmp_encrypt(tk, pn, aad, sizeof(aad), plain, sizeof(plain), cipher, mic);
    assert(enc_ret == 0);

    /* Assert ciphertext is not equal to plaintext */
    assert(memcmp(cipher, plain, sizeof(plain)) != 0);

    /* Decrypt with matching parameters */
    uint8_t decrypted[80] = {0};
    int dec_ret = wlan_ccmp_decrypt(tk, pn, aad, sizeof(aad), cipher, sizeof(cipher), mic, decrypted);
    assert(dec_ret == 0);
    assert(memcmp(decrypted, plain, sizeof(plain)) == 0);

    /* Adversarial Gate 1: Tampered Ciphertext bit */
    uint8_t tampered_cipher[80];
    memcpy(tampered_cipher, cipher, 80);
    tampered_cipher[20] ^= 0x01;
    memset(decrypted, 0xFF, 80);
    assert(wlan_ccmp_decrypt(tk, pn, aad, sizeof(aad), tampered_cipher, 80, mic, decrypted) == -1);

    /* Adversarial Gate 2: Tampered AAD (e.g. spoofed Station MAC) */
    uint8_t tampered_aad[22];
    memcpy(tampered_aad, aad, 22);
    tampered_aad[9] ^= 0x02;
    assert(wlan_ccmp_decrypt(tk, pn, tampered_aad, sizeof(aad), cipher, 80, mic, decrypted) == -1);

    /* Adversarial Gate 3: Tampered MIC */
    uint8_t tampered_mic[8];
    memcpy(tampered_mic, mic, 8);
    tampered_mic[7] ^= 0x80;
    assert(wlan_ccmp_decrypt(tk, pn, aad, sizeof(aad), cipher, 80, tampered_mic, decrypted) == -1);

    /* Adversarial Gate 4: Wrong TK */
    uint8_t wrong_tk[16];
    memcpy(wrong_tk, tk, 16);
    wrong_tk[0] ^= 0xFF;
    assert(wlan_ccmp_decrypt(wrong_tk, pn, aad, sizeof(aad), cipher, 80, mic, decrypted) == -1);

    printf("[TEST 6] PASS: AES-128-CCMP roundtrip and adversarial tamper rejection verified.\n");
}

/* ============================================================================
 * 7. IEEE 802.11i TKIP Group Frame Decryption (Phase 1/2 + RC4 + Michael MIC)
 * ============================================================================ */

static void test_tkip_group_decryption(void) {
    printf("[TEST 7] Testing TKIP group encryption, decryption, Michael MIC, and replay window...\n");

    uint8_t gtk[32];
    for (int i = 0; i < 32; i++) gtk[i] = (uint8_t)(0x30 + i * 3);

    uint8_t bcast_da[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t ap_bssid[6] = {0x00, 0x14, 0x6C, 0x7E, 0x40, 0x80};
    uint8_t orig_sa[6]  = {0x00, 0x22, 0x33, 0x44, 0x55, 0x66};

    uint8_t plain_frame[128];
    memset(plain_frame, 0, sizeof(plain_frame));
    /* QoS Data FromDS=1 (26-byte header) */
    plain_frame[0] = 0x88; /* QoS Data */
    plain_frame[1] = 0x02; /* FromDS */
    memcpy(plain_frame + 4, bcast_da, 6);
    memcpy(plain_frame + 10, ap_bssid, 6);
    memcpy(plain_frame + 16, orig_sa, 6);
    plain_frame[24] = 0x00; plain_frame[25] = 0x00;

    /* LLC/SNAP + ARP Broadcast Payload (36 bytes) */
    const uint8_t snap_arp[36] = {
        0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x00, 0x22, 0x33, 0x44, 0x55, 0x66, 192, 168, 1, 1,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 192, 168, 1, 42
    };
    memcpy(plain_frame + 26, snap_arp, sizeof(snap_arp));
    size_t plain_len = 26 + sizeof(snap_arp);

    uint8_t tkip_frame[256];
    size_t tkip_len = sizeof(tkip_frame);
    uint64_t tsc = 10;
    assert(wlan_tkip_encrypt_frame(gtk, tsc, 1, plain_frame, plain_len, tkip_frame, &tkip_len) == 0);
    assert(tkip_len == plain_len + 20); /* +8B ExtIV +8B Michael MIC +4B ICV */

    uint8_t dec_frame[256];
    size_t dec_len = sizeof(dec_frame);
    uint64_t last_tsc = 0;
    uint64_t bitmap = 0;
    assert(wlan_tkip_decrypt_frame(gtk, tkip_frame, tkip_len, dec_frame, &dec_len, &last_tsc, &bitmap) == 0);
    assert(dec_len == plain_len);
    assert(memcmp(dec_frame + 26, snap_arp, sizeof(snap_arp)) == 0);
    assert(last_tsc == 10);

    /* Replay rejection: same TSC=10 must be rejected */
    dec_len = sizeof(dec_frame);
    assert(wlan_tkip_decrypt_frame(gtk, tkip_frame, tkip_len, dec_frame, &dec_len, &last_tsc, &bitmap) == -2);

    /* Tampered Michael MIC / Ciphertext rejection */
    uint8_t tkip_tampered[256];
    size_t tkip_t_len = sizeof(tkip_tampered);
    assert(wlan_tkip_encrypt_frame(gtk, 11, 1, plain_frame, plain_len, tkip_tampered, &tkip_t_len) == 0);
    tkip_tampered[26 + 8 + 10] ^= 0x01; /* Flip bit in encrypted payload */
    dec_len = sizeof(dec_frame);
    assert(wlan_tkip_decrypt_frame(gtk, tkip_tampered, tkip_t_len, dec_frame, &dec_len, &last_tsc, &bitmap) == -1);

    printf("[TEST 7] PASS: TKIP group encryption/decryption, Michael MIC, ICV, and replay window verified.\n");
}

/* ============================================================================
 * 8. SHA-256, HMAC-SHA-256, and IEEE 802.11 KDF-SHA-256
 * ============================================================================ */

static void test_sha256_and_kdf256(void) {
    printf("[TEST 8] Testing SHA-256, HMAC-SHA-256, and IEEE 802.11 KDF-SHA-256...\n");

    /* FIPS 180-2 SHA-256("abc") */
    uint8_t d256[32];
    wlan_sha256("abc", 3, d256);
    const uint8_t exp_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    assert(memcmp(d256, exp_abc, 32) == 0);

    /* RFC 4231 HMAC-SHA-256 Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?" */
    uint8_t hmac256[32];
    wlan_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, hmac256);
    const uint8_t exp_hmac[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
    };
    assert(memcmp(hmac256, exp_hmac, 32) == 0);

    /* KDF-SHA-256 PTK derivation symmetry */
    uint8_t pmk[32];
    memset(pmk, 0xA5, 32);
    uint8_t mac1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac2[6] = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    uint8_t n1[32], n2[32], ptk_a[64], ptk_b[64];
    memset(n1, 0x11, 32);
    memset(n2, 0x22, 32);
    wlan_derive_ptk_sha256(pmk, mac1, mac2, n1, n2, ptk_a);
    wlan_derive_ptk_sha256(pmk, mac2, mac1, n2, n1, ptk_b);
    assert(memcmp(ptk_a, ptk_b, 64) == 0);

    printf("[TEST 8] PASS: SHA-256, HMAC-SHA-256, and KDF-SHA-256 PTK derivation verified.\n");
}

/* ============================================================================
 * 9. WPA3-Personal SAE (RFC 7664 / NIST P-256 Group 19 Dragonfly Handshake)
 * ============================================================================ */

static void test_wpa3_sae_p256(void) {
    printf("[TEST 9] Testing WPA3-Personal SAE (NIST P-256 Group 19) Commit-Confirm exchange...\n");

    uint8_t sta_mac[6] = {0x00, 0x24, 0xFD, 0x82, 0x65, 0x01};
    uint8_t ap_mac[6]  = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    const char *pass   = "DragonflyWPA3Secret!";

    wlan_sae_ctx_t sta_ctx, ap_ctx;
    assert(wlan_sae_derive_pwe(&sta_ctx, sta_mac, ap_mac, pass) == 0);
    assert(wlan_sae_derive_pwe(&ap_ctx, ap_mac, sta_mac, pass) == 0);

    /* Both peers must derive the exact same PWE point (x, y) */
    assert(memcmp(sta_ctx.pwe_x, ap_ctx.pwe_x, 32) == 0);
    assert(memcmp(sta_ctx.pwe_y, ap_ctx.pwe_y, 32) == 0);

    uint8_t sta_commit[WLAN_SAE_COMMIT_LEN];
    uint8_t ap_commit[WLAN_SAE_COMMIT_LEN];
    assert(wlan_sae_build_commit(&sta_ctx, sta_commit) == 0);
    assert(wlan_sae_build_commit(&ap_ctx, ap_commit) == 0);

    /* Reflection attack gate: feeding own commit back must be rejected */
    assert(wlan_sae_process_commit(&sta_ctx, sta_commit, sizeof(sta_commit)) == -1);

    /* Mutual commit processing */
    assert(wlan_sae_process_commit(&sta_ctx, ap_commit, sizeof(ap_commit)) == 0);
    assert(wlan_sae_process_commit(&ap_ctx, sta_commit, sizeof(sta_commit)) == 0);

    /* Both peers must derive identical KCK, PMK, and PMKID */
    assert(memcmp(sta_ctx.kck, ap_ctx.kck, 32) == 0);
    assert(memcmp(sta_ctx.pmk, ap_ctx.pmk, 32) == 0);
    assert(memcmp(sta_ctx.pmkid, ap_ctx.pmkid, 16) == 0);

    /* Mutual confirm build and verification */
    uint8_t sta_confirm[WLAN_SAE_CONFIRM_LEN];
    uint8_t ap_confirm[WLAN_SAE_CONFIRM_LEN];
    assert(wlan_sae_build_confirm(&sta_ctx, sta_confirm) == 0);
    assert(wlan_sae_build_confirm(&ap_ctx, ap_confirm) == 0);

    assert(wlan_sae_verify_confirm(&sta_ctx, ap_confirm, sizeof(ap_confirm)) == 0);
    assert(wlan_sae_verify_confirm(&ap_ctx, sta_confirm, sizeof(sta_confirm)) == 0);
    assert(sta_ctx.confirmed && ap_ctx.confirmed);

    /* Tampered confirm must fail verification */
    uint8_t bad_confirm[WLAN_SAE_CONFIRM_LEN];
    memcpy(bad_confirm, ap_confirm, sizeof(bad_confirm));
    bad_confirm[10] ^= 0x01;
    assert(wlan_sae_verify_confirm(&sta_ctx, bad_confirm, sizeof(bad_confirm)) == -1);

    printf("[TEST 9] PASS: WPA3-SAE NIST P-256 PWE, Commit-Confirm, PMK/PMKID, and anti-reflection verified.\n");
}

int main(void) {
    printf("================================================================\n");
    printf("  FortressOS Wireless Cryptographic Engine (WIFI-0) Test Suite  \n");
    printf("================================================================\n");

    test_sha1();
    test_hmac_sha1();
    test_pbkdf2_wpa2();
    test_ptk_derivation();
    test_aes_key_unwrap();
    test_ccmp_roundtrip_and_tampering();
    test_tkip_group_decryption();
    test_sha256_and_kdf256();
    test_wpa3_sae_p256();

    printf("================================================================\n");
    printf("  ALL 9 WLAN CRYPTO SUITES PASSED (100%%)                        \n");
    printf("================================================================\n");
    return 0;
}
