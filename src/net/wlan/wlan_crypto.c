#include "wlan_crypto.h"
#include <string.h>

/* ============================================================================
 * 1. SHA-1 Implementation (RFC 3174)
 * ============================================================================ */

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void wlan_sha1_init(wlan_sha1_ctx_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4 + 0] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8)  |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    /* 80 rounds */
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = SHA1_ROTL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void wlan_sha1_update(wlan_sha1_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx || !data || !len) return;
    const uint8_t *src = (const uint8_t *)data;
    size_t buf_idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    while (len > 0) {
        size_t take = 64 - buf_idx;
        if (take > len) take = len;
        memcpy(&ctx->buffer[buf_idx], src, take);
        src += take;
        len -= take;
        buf_idx += take;

        if (buf_idx == 64) {
            sha1_transform(ctx->state, ctx->buffer);
            buf_idx = 0;
        }
    }
}

void wlan_sha1_final(wlan_sha1_ctx_t *ctx, uint8_t digest[WLAN_SHA1_DIGEST_SIZE]) {
    if (!ctx || !digest) return;
    uint64_t total_bits = ctx->count * 8;
    uint8_t pad = 0x80;
    wlan_sha1_update(ctx, &pad, 1);

    uint8_t zero = 0x00;
    while ((ctx->count & 0x3F) != 56) {
        wlan_sha1_update(ctx, &zero, 1);
    }

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    }
    wlan_sha1_update(ctx, len_bytes, 8);

    for (int i = 0; i < 5; i++) {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void wlan_sha1(const void *data, size_t len, uint8_t digest[WLAN_SHA1_DIGEST_SIZE]) {
    wlan_sha1_ctx_t ctx;
    wlan_sha1_init(&ctx);
    wlan_sha1_update(&ctx, data, len);
    wlan_sha1_final(&ctx, digest);
}

/* ============================================================================
 * 2. HMAC-SHA-1 Implementation (RFC 2104)
 * ============================================================================ */

void wlan_hmac_sha1(const void *key, size_t key_len,
                    const void *data, size_t data_len,
                    uint8_t mac[WLAN_SHA1_DIGEST_SIZE]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        wlan_sha1(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner_hash[WLAN_SHA1_DIGEST_SIZE];
    wlan_sha1_ctx_t ctx;

    wlan_sha1_init(&ctx);
    wlan_sha1_update(&ctx, ipad, 64);
    if (data && data_len) {
        wlan_sha1_update(&ctx, data, data_len);
    }
    wlan_sha1_final(&ctx, inner_hash);

    wlan_sha1_init(&ctx);
    wlan_sha1_update(&ctx, opad, 64);
    wlan_sha1_update(&ctx, inner_hash, WLAN_SHA1_DIGEST_SIZE);
    wlan_sha1_final(&ctx, mac);
}

/* ============================================================================
 * 3. PBKDF2-HMAC-SHA1 Implementation (RFC 2898 / RFC 8018)
 * ============================================================================ */

int wlan_pbkdf2_sha1(const char *passphrase,
                     const uint8_t *ssid, size_t ssid_len,
                     uint32_t iterations,
                     uint8_t *out_key, size_t out_len) {
    if (!passphrase || !ssid || !out_key || !iterations || !out_len) return -1;
    size_t pass_len = strlen(passphrase);

    uint32_t block_count = (uint32_t)((out_len + WLAN_SHA1_DIGEST_SIZE - 1) / WLAN_SHA1_DIGEST_SIZE);
    uint8_t u[WLAN_SHA1_DIGEST_SIZE];
    uint8_t t[WLAN_SHA1_DIGEST_SIZE];

    /* Buffer for salt + BE32(block_idx) */
    uint8_t salt_block[68];
    if (ssid_len > 64) return -1;
    memcpy(salt_block, ssid, ssid_len);

    size_t offset = 0;
    for (uint32_t i = 1; i <= block_count; i++) {
        salt_block[ssid_len + 0] = (uint8_t)(i >> 24);
        salt_block[ssid_len + 1] = (uint8_t)(i >> 16);
        salt_block[ssid_len + 2] = (uint8_t)(i >> 8);
        salt_block[ssid_len + 3] = (uint8_t)(i);

        wlan_hmac_sha1(passphrase, pass_len, salt_block, ssid_len + 4, u);
        memcpy(t, u, WLAN_SHA1_DIGEST_SIZE);

        for (uint32_t j = 1; j < iterations; j++) {
            wlan_hmac_sha1(passphrase, pass_len, u, WLAN_SHA1_DIGEST_SIZE, u);
            for (int k = 0; k < WLAN_SHA1_DIGEST_SIZE; k++) {
                t[k] ^= u[k];
            }
        }

        size_t copy_bytes = out_len - offset;
        if (copy_bytes > WLAN_SHA1_DIGEST_SIZE) copy_bytes = WLAN_SHA1_DIGEST_SIZE;
        memcpy(out_key + offset, t, copy_bytes);
        offset += copy_bytes;
    }

    return 0;
}

/* ============================================================================
 * 4. IEEE 802.11i PRF-512 & 4-Way Handshake PTK Derivation
 * ============================================================================ */

void wlan_prf_512(const uint8_t pmk[WLAN_PMK_LEN],
                  const char *label,
                  const uint8_t *data, size_t data_len,
                  uint8_t ptk[WLAN_PTK_LEN]) {
    if (!pmk || !label || !ptk) return;
    size_t label_len = strlen(label);

    /* Construct input buffer: label || 0x00 || data || counter */
    uint8_t buf[256];
    size_t prefix_len = label_len + 1 + data_len;
    if (prefix_len + 1 > sizeof(buf)) return;

    memcpy(buf, label, label_len);
    buf[label_len] = 0x00;
    if (data && data_len) {
        memcpy(buf + label_len + 1, data, data_len);
    }

    size_t offset = 0;
    uint8_t hash[WLAN_SHA1_DIGEST_SIZE];

    for (uint8_t count = 0; offset < WLAN_PTK_LEN; count++) {
        buf[prefix_len] = count;
        wlan_hmac_sha1(pmk, WLAN_PMK_LEN, buf, prefix_len + 1, hash);

        size_t take = WLAN_PTK_LEN - offset;
        if (take > WLAN_SHA1_DIGEST_SIZE) take = WLAN_SHA1_DIGEST_SIZE;
        memcpy(ptk + offset, hash, take);
        offset += take;
    }
}

void wlan_derive_ptk(const uint8_t pmk[WLAN_PMK_LEN],
                     const uint8_t addr1[6], const uint8_t addr2[6],
                     const uint8_t nonce1[32], const uint8_t nonce2[32],
                     uint8_t ptk[WLAN_PTK_LEN]) {
    if (!pmk || !addr1 || !addr2 || !nonce1 || !nonce2 || !ptk) return;

    uint8_t data[76];
    /* Order MAC addresses */
    if (memcmp(addr1, addr2, 6) < 0) {
        memcpy(data + 0, addr1, 6);
        memcpy(data + 6, addr2, 6);
    } else {
        memcpy(data + 0, addr2, 6);
        memcpy(data + 6, addr1, 6);
    }

    /* Order Nonces */
    if (memcmp(nonce1, nonce2, 32) < 0) {
        memcpy(data + 12, nonce1, 32);
        memcpy(data + 44, nonce2, 32);
    } else {
        memcpy(data + 12, nonce2, 32);
        memcpy(data + 44, nonce1, 32);
    }

    wlan_prf_512(pmk, "Pairwise key expansion", data, 76, ptk);
}

/* ============================================================================
 * 5. Constant-Time AES-128 Primitive & Key Wrap (RFC 3394)
 * ============================================================================ */

/* Forward S-Box */
static const uint8_t s_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/* Inverse S-Box */
static const uint8_t s_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint8_t s_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

/* Multiply by 2 in GF(2^8) */
static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t res = 0;
    while (b) {
        if (b & 1) res ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return res;
}

static void aes128_key_expansion(const uint8_t key[16], uint8_t round_keys[176]) {
    memcpy(round_keys, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t temp[4];
        memcpy(temp, &round_keys[(i - 1) * 4], 4);

        if (i % 4 == 0) {
            uint8_t rot = temp[0];
            temp[0] = s_sbox[temp[1]] ^ s_rcon[i / 4];
            temp[1] = s_sbox[temp[2]];
            temp[2] = s_sbox[temp[3]];
            temp[3] = s_sbox[rot];
        }

        for (int j = 0; j < 4; j++) {
            round_keys[i * 4 + j] = round_keys[(i - 4) * 4 + j] ^ temp[j];
        }
    }
}

static void aes128_encrypt_block(const uint8_t round_keys[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) state[i] = in[i] ^ round_keys[i];

    for (int round = 1; round <= 10; round++) {
        /* SubBytes */
        for (int i = 0; i < 16; i++) state[i] = s_sbox[state[i]];

        /* ShiftRows */
        uint8_t t;
        t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
        t = state[2]; state[2] = state[10]; state[10] = t;
        t = state[6]; state[6] = state[14]; state[14] = t;
        t = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = t;

        /* MixColumns (rounds 1..9) */
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = state[c * 4 + 0], a1 = state[c * 4 + 1];
                uint8_t a2 = state[c * 4 + 2], a3 = state[c * 4 + 3];
                state[c * 4 + 0] = xtime(a0 ^ a1) ^ a1 ^ a2 ^ a3;
                state[c * 4 + 1] = xtime(a1 ^ a2) ^ a2 ^ a3 ^ a0;
                state[c * 4 + 2] = xtime(a2 ^ a3) ^ a3 ^ a0 ^ a1;
                state[c * 4 + 3] = xtime(a3 ^ a0) ^ a0 ^ a1 ^ a2;
            }
        }

        /* AddRoundKey */
        const uint8_t *rk = &round_keys[round * 16];
        for (int i = 0; i < 16; i++) state[i] ^= rk[i];
    }
    memcpy(out, state, 16);
}

static void aes128_decrypt_block(const uint8_t round_keys[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) state[i] = in[i] ^ round_keys[160 + i];

    for (int round = 9; round >= 0; round--) {
        /* InvShiftRows */
        uint8_t t;
        t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
        t = state[2]; state[2] = state[10]; state[10] = t;
        t = state[6]; state[6] = state[14]; state[14] = t;
        t = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = t;

        /* InvSubBytes */
        for (int i = 0; i < 16; i++) state[i] = s_inv_sbox[state[i]];

        /* AddRoundKey */
        const uint8_t *rk = &round_keys[round * 16];
        for (int i = 0; i < 16; i++) state[i] ^= rk[i];

        /* InvMixColumns (rounds 9..1) */
        if (round > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t s0 = state[c * 4 + 0], s1 = state[c * 4 + 1];
                uint8_t s2 = state[c * 4 + 2], s3 = state[c * 4 + 3];
                state[c * 4 + 0] = gf_mul(s0, 0x0e) ^ gf_mul(s1, 0x0b) ^ gf_mul(s2, 0x0d) ^ gf_mul(s3, 0x09);
                state[c * 4 + 1] = gf_mul(s0, 0x09) ^ gf_mul(s1, 0x0e) ^ gf_mul(s2, 0x0b) ^ gf_mul(s3, 0x0d);
                state[c * 4 + 2] = gf_mul(s0, 0x0d) ^ gf_mul(s1, 0x09) ^ gf_mul(s2, 0x0e) ^ gf_mul(s3, 0x0b);
                state[c * 4 + 3] = gf_mul(s0, 0x0b) ^ gf_mul(s1, 0x0d) ^ gf_mul(s2, 0x09) ^ gf_mul(s3, 0x0e);
            }
        }
    }
    memcpy(out, state, 16);
}

int wlan_aes_unwrap(const uint8_t kek[16],
                    const uint8_t *wrapped, size_t wrapped_len,
                    uint8_t *plain) {
    if (!kek || !wrapped || !plain || wrapped_len < 16 || (wrapped_len % 8) != 0) {
        return -1;
    }

    size_t n = (wrapped_len / 8) - 1;
    uint8_t round_keys[176];
    aes128_key_expansion(kek, round_keys);

    uint8_t a[8];
    memcpy(a, wrapped, 8);

    /* Allocate R array on stack (max 64 bytes for Wi-Fi GTK/IGTK) */
    if (n > 8) return -1;
    uint8_t r[8][8];
    for (size_t i = 1; i <= n; i++) {
        memcpy(r[i - 1], wrapped + i * 8, 8);
    }

    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n; i >= 1; i--) {
            uint64_t t = (uint64_t)(n * j + i);
            uint8_t b[16];

            /* B = AES_Decrypt( (A ^ t) || R[i] ) */
            for (int k = 0; k < 8; k++) {
                uint8_t t_byte = (uint8_t)(t >> ((7 - k) * 8));
                b[k] = a[k] ^ t_byte;
            }
            memcpy(b + 8, r[i - 1], 8);

            uint8_t dec[16];
            aes128_decrypt_block(round_keys, b, dec);

            memcpy(a, dec, 8);
            memcpy(r[i - 1], dec + 8, 8);
        }
    }

    /* Verify IV: 0xA6A6A6A6A6A6A6A6 */
    for (int k = 0; k < 8; k++) {
        if (a[k] != 0xA6) return -1;
    }

    /* Copy recovered plaintext */
    for (size_t i = 1; i <= n; i++) {
        memcpy(plain + (i - 1) * 8, r[i - 1], 8);
    }
    return 0;
}

/* ============================================================================
 * 6. IEEE 802.11i / RFC 3610 AES-128-CCMP Implementation
 * ============================================================================ */

int wlan_ccmp_encrypt(const uint8_t tk[16],
                      uint64_t pn,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *plain, size_t plain_len,
                      uint8_t *cipher,
                      uint8_t mic[WLAN_MIC_LEN]) {
    if (!tk || !cipher || !mic) return -1;

    uint8_t round_keys[176];
    aes128_key_expansion(tk, round_keys);

    /* Construct Nonce (13 bytes): Priority (1 byte) || A2 (6 bytes) || PN (6 bytes) */
    uint8_t nonce[13] = {0};
    if (aad && aad_len >= 16) {
        /* Address 2 is at offset 10 of 802.11 header */
        memcpy(nonce + 1, aad + 10, 6);
    }
    nonce[7]  = (uint8_t)(pn >> 40);
    nonce[8]  = (uint8_t)(pn >> 32);
    nonce[9]  = (uint8_t)(pn >> 24);
    nonce[10] = (uint8_t)(pn >> 16);
    nonce[11] = (uint8_t)(pn >> 8);
    nonce[12] = (uint8_t)(pn);

    /* 1. CBC-MAC (Authentication) */
    uint8_t b0[16] = {0};
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | (3 << 3) /* M=8 */ | 1 /* L=2 */;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)(plain_len >> 8);
    b0[15] = (uint8_t)(plain_len & 0xFF);

    uint8_t x[16];
    aes128_encrypt_block(round_keys, b0, x);

    /* Process AAD */
    if (aad_len > 0) {
        uint8_t aad_buf[16] = {0};
        aad_buf[0] = (uint8_t)(aad_len >> 8);
        aad_buf[1] = (uint8_t)(aad_len & 0xFF);

        size_t initial = 14;
        if (initial > aad_len) initial = aad_len;
        memcpy(aad_buf + 2, aad, initial);

        for (int k = 0; k < 16; k++) x[k] ^= aad_buf[k];
        aes128_encrypt_block(round_keys, x, x);

        size_t rem = aad_len - initial;
        const uint8_t *cur = aad + initial;
        while (rem > 0) {
            memset(aad_buf, 0, 16);
            size_t take = (rem > 16) ? 16 : rem;
            memcpy(aad_buf, cur, take);
            for (int k = 0; k < 16; k++) x[k] ^= aad_buf[k];
            aes128_encrypt_block(round_keys, x, x);
            cur += take;
            rem -= take;
        }
    }

    /* Process Plaintext in CBC-MAC */
    size_t rem_plain = plain_len;
    const uint8_t *cur_plain = plain;
    while (rem_plain > 0) {
        uint8_t block[16] = {0};
        size_t take = (rem_plain > 16) ? 16 : rem_plain;
        memcpy(block, cur_plain, take);
        for (int k = 0; k < 16; k++) x[k] ^= block[k];
        aes128_encrypt_block(round_keys, x, x);
        cur_plain += take;
        rem_plain -= take;
    }

    /* Tag T is first 8 bytes of X */
    uint8_t t[8];
    memcpy(t, x, 8);

    /* 2. Counter Mode (CTR Encryption) */
    uint8_t a0[16] = {0};
    a0[0] = 1; /* L=2 */
    memcpy(a0 + 1, nonce, 13);
    a0[14] = 0;
    a0[15] = 0;

    uint8_t s0[16];
    aes128_encrypt_block(round_keys, a0, s0);
    for (int k = 0; k < 8; k++) {
        mic[k] = t[k] ^ s0[k];
    }

    uint16_t ctr = 1;
    rem_plain = plain_len;
    cur_plain = plain;
    uint8_t *cur_cipher = cipher;

    while (rem_plain > 0) {
        uint8_t ai[16] = {0};
        ai[0] = 1;
        memcpy(ai + 1, nonce, 13);
        ai[14] = (uint8_t)(ctr >> 8);
        ai[15] = (uint8_t)(ctr & 0xFF);
        ctr++;

        uint8_t si[16];
        aes128_encrypt_block(round_keys, ai, si);

        size_t take = (rem_plain > 16) ? 16 : rem_plain;
        for (size_t k = 0; k < take; k++) {
            cur_cipher[k] = cur_plain[k] ^ si[k];
        }
        cur_plain += take;
        cur_cipher += take;
        rem_plain -= take;
    }

    return 0;
}

int wlan_ccmp_decrypt(const uint8_t tk[16],
                      uint64_t pn,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *cipher, size_t cipher_len,
                      const uint8_t mic[WLAN_MIC_LEN],
                      uint8_t *plain) {
    if (!tk || !cipher || !mic || !plain) return -1;

    uint8_t round_keys[176];
    aes128_key_expansion(tk, round_keys);

    /* Construct Nonce */
    uint8_t nonce[13] = {0};
    if (aad && aad_len >= 16) {
        memcpy(nonce + 1, aad + 10, 6);
    }
    nonce[7]  = (uint8_t)(pn >> 40);
    nonce[8]  = (uint8_t)(pn >> 32);
    nonce[9]  = (uint8_t)(pn >> 24);
    nonce[10] = (uint8_t)(pn >> 16);
    nonce[11] = (uint8_t)(pn >> 8);
    nonce[12] = (uint8_t)(pn);

    /* 1. Decrypt Plaintext using CTR mode */
    uint16_t ctr = 1;
    size_t rem_cipher = cipher_len;
    const uint8_t *cur_cipher = cipher;
    uint8_t *cur_plain = plain;

    while (rem_cipher > 0) {
        uint8_t ai[16] = {0};
        ai[0] = 1;
        memcpy(ai + 1, nonce, 13);
        ai[14] = (uint8_t)(ctr >> 8);
        ai[15] = (uint8_t)(ctr & 0xFF);
        ctr++;

        uint8_t si[16];
        aes128_encrypt_block(round_keys, ai, si);

        size_t take = (rem_cipher > 16) ? 16 : rem_cipher;
        for (size_t k = 0; k < take; k++) {
            cur_plain[k] = cur_cipher[k] ^ si[k];
        }
        cur_cipher += take;
        cur_plain += take;
        rem_cipher -= take;
    }

    /* Decrypt MIC */
    uint8_t a0[16] = {0};
    a0[0] = 1;
    memcpy(a0 + 1, nonce, 13);
    a0[14] = 0;
    a0[15] = 0;

    uint8_t s0[16];
    aes128_encrypt_block(round_keys, a0, s0);
    uint8_t expected_t[8];
    for (int k = 0; k < 8; k++) {
        expected_t[k] = mic[k] ^ s0[k];
    }

    /* 2. Recompute CBC-MAC over decrypted plaintext and AAD */
    uint8_t b0[16] = {0};
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | (3 << 3) | 1;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)(cipher_len >> 8);
    b0[15] = (uint8_t)(cipher_len & 0xFF);

    uint8_t x[16];
    aes128_encrypt_block(round_keys, b0, x);

    if (aad_len > 0) {
        uint8_t aad_buf[16] = {0};
        aad_buf[0] = (uint8_t)(aad_len >> 8);
        aad_buf[1] = (uint8_t)(aad_len & 0xFF);

        size_t initial = 14;
        if (initial > aad_len) initial = aad_len;
        memcpy(aad_buf + 2, aad, initial);

        for (int k = 0; k < 16; k++) x[k] ^= aad_buf[k];
        aes128_encrypt_block(round_keys, x, x);

        size_t rem = aad_len - initial;
        const uint8_t *cur = aad + initial;
        while (rem > 0) {
            memset(aad_buf, 0, 16);
            size_t take = (rem > 16) ? 16 : rem;
            memcpy(aad_buf, cur, take);
            for (int k = 0; k < 16; k++) x[k] ^= aad_buf[k];
            aes128_encrypt_block(round_keys, x, x);
            cur += take;
            rem -= take;
        }
    }

    size_t rem_plain = cipher_len;
    cur_plain = plain;
    while (rem_plain > 0) {
        uint8_t block[16] = {0};
        size_t take = (rem_plain > 16) ? 16 : rem_plain;
        memcpy(block, cur_plain, take);
        for (int k = 0; k < 16; k++) x[k] ^= block[k];
        aes128_encrypt_block(round_keys, x, x);
        cur_plain += take;
        rem_plain -= take;
    }

    /* Constant-time comparison of computed tag vs decrypted tag */
    uint8_t diff = 0;
    for (int k = 0; k < 8; k++) {
        diff |= (x[k] ^ expected_t[k]);
    }

    if (diff != 0) {
        /* MIC failure: wipe decrypted plaintext to avoid exposing unauthenticated data */
        memset(plain, 0, cipher_len);
        return -1;
    }

    return 0;
}

int wlan_ccmp_encrypt_frame(const uint8_t tk[16],
                            uint64_t pn,
                            const uint8_t *plain_frame, size_t plain_len,
                            uint8_t *out_frame, size_t *out_len) {
    if (!tk || !plain_frame || plain_len < 24 || !out_frame || !out_len) return -1;
    const uint8_t *hdr = plain_frame;
    uint16_t fc = hdr[0] | ((uint16_t)hdr[1] << 8);
    bool is_qos = ((fc >> 4) & 0xF) == 0x08;
    size_t hdr_len = is_qos ? 26 : 24;
    if (plain_len < hdr_len) return -1;
    if (*out_len < plain_len + 16) return -1;

    size_t payload_len = plain_len - hdr_len;
    const uint8_t *payload = plain_frame + hdr_len;

    memcpy(out_frame, plain_frame, hdr_len);
    out_frame[1] |= 0x40; /* IEEE80211_FCTL_PROTECTED is bit 14 (high byte bit 6: 0x40) */

    uint8_t *ccmp_hdr = out_frame + hdr_len;
    ccmp_hdr[0] = (uint8_t)(pn & 0xFF);
    ccmp_hdr[1] = (uint8_t)((pn >> 8) & 0xFF);
    ccmp_hdr[2] = 0;
    ccmp_hdr[3] = 0x20;
    ccmp_hdr[4] = (uint8_t)((pn >> 16) & 0xFF);
    ccmp_hdr[5] = (uint8_t)((pn >> 24) & 0xFF);
    ccmp_hdr[6] = (uint8_t)((pn >> 32) & 0xFF);
    ccmp_hdr[7] = (uint8_t)((pn >> 40) & 0xFF);

    uint8_t aad[32];
    uint16_t masked_fc = (fc & ~(0x0800 | 0x1000 | 0x2000)) | 0x4000;
    aad[0] = (uint8_t)(masked_fc & 0xFF);
    aad[1] = (uint8_t)((masked_fc >> 8) & 0xFF);
    memcpy(aad + 2, hdr + 4, 18);
    uint16_t sc = (hdr[22] | ((uint16_t)hdr[23] << 8)) & 0xFFF0;
    aad[20] = (uint8_t)(sc & 0xFF);
    aad[21] = (uint8_t)((sc >> 8) & 0xFF);
    size_t aad_len = 22;
    if (is_qos) {
        uint16_t qc = (hdr[24] | ((uint16_t)hdr[25] << 8)) & 0x000F;
        aad[22] = (uint8_t)(qc & 0xFF);
        aad[23] = (uint8_t)((qc >> 8) & 0xFF);
        aad_len = 24;
    }

    uint8_t *cipher = out_frame + hdr_len + 8;
    uint8_t mic[8];
    int rc = wlan_ccmp_encrypt(tk, pn, aad, aad_len, payload, payload_len, cipher, mic);
    if (rc != 0) return rc;

    memcpy(cipher + payload_len, mic, 8);
    *out_len = hdr_len + 8 + payload_len + 8;
    return 0;
}

int wlan_ccmp_decrypt_frame(const uint8_t tk[16],
                            const uint8_t *cipher_frame, size_t cipher_len,
                            uint8_t *out_frame, size_t *out_len,
                            uint64_t *rx_pn) {
    if (!tk || !cipher_frame || cipher_len < 40 || !out_frame || !out_len || !rx_pn) return -1;
    const uint8_t *hdr = cipher_frame;
    uint16_t fc = hdr[0] | ((uint16_t)hdr[1] << 8);
    if (!(fc & 0x4000)) return -1;

    bool is_qos = ((fc >> 4) & 0xF) == 0x08;
    size_t hdr_len = is_qos ? 26 : 24;
    if (cipher_len < hdr_len + 16) return -1;

    size_t payload_len = cipher_len - hdr_len - 16;
    const uint8_t *ccmp_hdr = cipher_frame + hdr_len;
    const uint8_t *cipher = ccmp_hdr + 8;
    const uint8_t *mic = cipher + payload_len;

    uint64_t pn = (uint64_t)ccmp_hdr[0] |
                  ((uint64_t)ccmp_hdr[1] << 8) |
                  ((uint64_t)ccmp_hdr[4] << 16) |
                  ((uint64_t)ccmp_hdr[5] << 24) |
                  ((uint64_t)ccmp_hdr[6] << 32) |
                  ((uint64_t)ccmp_hdr[7] << 40);

    if (pn <= *rx_pn) return -2;

    uint8_t aad[32];
    uint16_t masked_fc = (fc & ~(0x0800 | 0x1000 | 0x2000)) | 0x4000;
    aad[0] = (uint8_t)(masked_fc & 0xFF);
    aad[1] = (uint8_t)((masked_fc >> 8) & 0xFF);
    memcpy(aad + 2, hdr + 4, 18);
    uint16_t sc = (hdr[22] | ((uint16_t)hdr[23] << 8)) & 0xFFF0;
    aad[20] = (uint8_t)(sc & 0xFF);
    aad[21] = (uint8_t)((sc >> 8) & 0xFF);
    size_t aad_len = 22;
    if (is_qos) {
        uint16_t qc = (hdr[24] | ((uint16_t)hdr[25] << 8)) & 0x000F;
        aad[22] = (uint8_t)(qc & 0xFF);
        aad[23] = (uint8_t)((qc >> 8) & 0xFF);
        aad_len = 24;
    }

    if (*out_len < hdr_len + payload_len) return -1;
    uint8_t *out_payload = out_frame + hdr_len;
    int rc = wlan_ccmp_decrypt(tk, pn, aad, aad_len, cipher, payload_len, mic, out_payload);
    if (rc != 0) return rc;

    *rx_pn = pn;
    memcpy(out_frame, cipher_frame, hdr_len);
    out_frame[1] &= ~0x40; /* Clear PROTECTED bit */
    *out_len = hdr_len + payload_len;
    return 0;
}
