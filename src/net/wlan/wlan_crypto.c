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
    if (!kek || !wrapped || !plain || wrapped_len < 24 || wrapped_len > 256 || (wrapped_len % 8) != 0) {
        return -1;
    }

    size_t n = (wrapped_len / 8) - 1;
    uint8_t round_keys[176];
    aes128_key_expansion(kek, round_keys);

    uint8_t a[8];
    memcpy(a, wrapped, 8);

    /* Complete bounded EAPOL Key Data can contain RSN + GTK + IGTK.
     * Match the handshake caller's 256-byte wrapped-data ceiling. */
    if (n > 31) return -1;
    uint8_t r[31][8];
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
    if (aad && aad_len >= 24) {
        nonce[0] = (uint8_t)(aad[22] & 0x0Fu);
    }
    if (aad && aad_len >= 14) {
        /* Address 2 is at offset 8 of IEEE 802.11 AAD (FC: 2B, A1: 6B, A2: 6B) */
        memcpy(nonce + 1, aad + 8, 6);
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
    if (aad && aad_len >= 24) {
        nonce[0] = (uint8_t)(aad[22] & 0x0Fu);
    }
    if (aad && aad_len >= 14) {
        /* Address 2 is at offset 8 of IEEE 802.11 AAD (FC: 2B, A1: 6B, A2: 6B) */
        memcpy(nonce + 1, aad + 8, 6);
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
    bool is_qos = ((fc >> 4) & 0x08) != 0;
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
    uint16_t fc_mask = 0x0070u | 0x0800u | 0x1000u | 0x2000u | 0x8000u;
    uint16_t masked_fc = (fc & ~fc_mask) | 0x4000u;
    aad[0] = (uint8_t)(masked_fc & 0xFF);
    aad[1] = (uint8_t)((masked_fc >> 8) & 0xFF);
    memcpy(aad + 2, hdr + 4, 18);
    uint16_t sc = (hdr[22] | ((uint16_t)hdr[23] << 8)) & 0x000Fu;
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
                            uint64_t *rx_pn,
                            uint64_t *rx_bitmap) {
    if (!tk || !cipher_frame || cipher_len < 40 || !out_frame || !out_len || !rx_pn) return -1;
    const uint8_t *hdr = cipher_frame;
    uint16_t fc = hdr[0] | ((uint16_t)hdr[1] << 8);
    if (!(fc & 0x4000)) return -1;

    bool is_qos = ((fc >> 4) & 0x08) != 0;
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

    /* 64-packet sliding replay window check */
    if (*rx_pn != 0) {
        if (pn <= *rx_pn) {
            uint64_t diff = *rx_pn - pn;
            if (diff >= 64) return -2; /* Outside 64-packet window */
            if (rx_bitmap && (*rx_bitmap & (1ULL << diff))) return -2; /* Replay packet */
        }
    }

    uint8_t aad[32];
    uint16_t fc_mask = 0x0070u | 0x0800u | 0x1000u | 0x2000u | 0x8000u;
    uint16_t masked_fc = (fc & ~fc_mask) | 0x4000u;
    aad[0] = (uint8_t)(masked_fc & 0xFF);
    aad[1] = (uint8_t)((masked_fc >> 8) & 0xFF);
    memcpy(aad + 2, hdr + 4, 18);
    uint16_t sc = (hdr[22] | ((uint16_t)hdr[23] << 8)) & 0x000Fu;
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

    /* Replay window state update after authenticated decryption */
    if (*rx_pn == 0) {
        *rx_pn = (pn == 0) ? 1 : pn;
        if (rx_bitmap) *rx_bitmap = 1ULL;
    } else if (pn > *rx_pn) {
        uint64_t diff = pn - *rx_pn;
        if (rx_bitmap) {
            if (diff < 64) *rx_bitmap = (*rx_bitmap << diff) | 1ULL;
            else *rx_bitmap = 1ULL;
        }
        *rx_pn = pn;
    } else {
        uint64_t diff = *rx_pn - pn;
        if (rx_bitmap && diff < 64) {
            *rx_bitmap |= (1ULL << diff);
        }
    }

    memcpy(out_frame, cipher_frame, hdr_len);
    out_frame[1] &= ~0x40; /* Clear PROTECTED bit */
    *out_len = hdr_len + payload_len;
    return 0;
}

/* ============================================================================
 * 7. IEEE 802.11i TKIP Group Frame Encryption & Decryption
 * ============================================================================ */

static inline uint16_t tkip_rotr16(uint16_t v, int n) {
    return (uint16_t)((v >> n) | (v << (16 - n)));
}

static inline uint32_t tkip_rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static inline uint32_t tkip_rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static inline uint16_t tkip_get_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t tkip_get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void tkip_put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* IEEE 802.11i Annex H.2 bijective 16-bit S-Box derived from AES S-Box */
static uint16_t tkip_sbox(uint16_t val) {
    uint8_t lo = (uint8_t)(val & 0xFFu);
    uint8_t hi = (uint8_t)(val >> 8);
    uint8_t s_lo = s_sbox[lo];
    uint8_t s2_lo = xtime(s_lo);
    uint8_t s3_lo = (uint8_t)(s2_lo ^ s_lo);
    uint16_t box0 = ((uint16_t)s2_lo << 8) | (uint16_t)s3_lo;

    uint8_t s_hi = s_sbox[hi];
    uint8_t s2_hi = xtime(s_hi);
    uint8_t s3_hi = (uint8_t)(s2_hi ^ s_hi);
    uint16_t box1 = ((uint16_t)s3_hi << 8) | (uint16_t)s2_hi;
    return (uint16_t)(box0 ^ box1);
}

/* TKIP Phase 1 & Phase 2 Key Mixing -> 16-byte RC4 per-packet seed */
static __attribute__((noinline)) void tkip_derive_rc4_key(const uint8_t tk[16],
                                                          const uint8_t ta[6],
                                                          uint64_t tsc,
                                                          uint8_t rc4_key[16]) {
    uint16_t p1k[5];
    uint16_t tsc16 = (uint16_t)(tsc & 0xFFFFu);
    p1k[0] = (uint16_t)((tsc >> 16) & 0xFFFFu);
    p1k[1] = (uint16_t)((tsc >> 32) & 0xFFFFu);
    p1k[2] = tkip_get_u16_le(ta + 0);
    p1k[3] = tkip_get_u16_le(ta + 2);
    p1k[4] = tkip_get_u16_le(ta + 4);

    /* Phase 1: 8 rounds */
    for (int i = 0; i < 8; i++) {
        int j = 2 * (i & 1);
        p1k[0] = (uint16_t)(p1k[0] + tkip_sbox(p1k[4] ^ tkip_get_u16_le(tk + ((0 + j) & 14))));
        p1k[1] = (uint16_t)(p1k[1] + tkip_sbox(p1k[0] ^ tkip_get_u16_le(tk + ((4 + j) & 14))));
        p1k[2] = (uint16_t)(p1k[2] + tkip_sbox(p1k[1] ^ tkip_get_u16_le(tk + ((8 + j) & 14))));
        p1k[3] = (uint16_t)(p1k[3] + tkip_sbox(p1k[2] ^ tkip_get_u16_le(tk + ((12 + j) & 14))));
        p1k[4] = (uint16_t)(p1k[4] + tkip_sbox(p1k[3] ^ tkip_get_u16_le(tk + ((0 + j) & 14))) + (uint16_t)i);
    }

    /* Phase 2: 96-bit PPK mixing */
    uint16_t ppk[6];
    for (int k = 0; k < 5; k++) ppk[k] = p1k[k];
    ppk[5] = (uint16_t)(p1k[4] + tsc16);

    ppk[0] = (uint16_t)(ppk[0] + tkip_sbox(ppk[5] ^ tkip_get_u16_le(tk + 0)));
    ppk[1] = (uint16_t)(ppk[1] + tkip_sbox(ppk[0] ^ tkip_get_u16_le(tk + 2)));
    ppk[2] = (uint16_t)(ppk[2] + tkip_sbox(ppk[1] ^ tkip_get_u16_le(tk + 4)));
    ppk[3] = (uint16_t)(ppk[3] + tkip_sbox(ppk[2] ^ tkip_get_u16_le(tk + 6)));
    ppk[4] = (uint16_t)(ppk[4] + tkip_sbox(ppk[3] ^ tkip_get_u16_le(tk + 8)));
    ppk[5] = (uint16_t)(ppk[5] + tkip_sbox(ppk[4] ^ tkip_get_u16_le(tk + 10)));

    ppk[0] = (uint16_t)(ppk[0] + tkip_rotr16(ppk[5] ^ tkip_get_u16_le(tk + 12), 1));
    ppk[1] = (uint16_t)(ppk[1] + tkip_rotr16(ppk[0] ^ tkip_get_u16_le(tk + 14), 1));
    ppk[2] = (uint16_t)(ppk[2] + tkip_rotr16(ppk[1], 1));
    ppk[3] = (uint16_t)(ppk[3] + tkip_rotr16(ppk[2], 1));
    ppk[4] = (uint16_t)(ppk[4] + tkip_rotr16(ppk[3], 1));
    ppk[5] = (uint16_t)(ppk[5] + tkip_rotr16(ppk[4], 1));

    rc4_key[0] = (uint8_t)((tsc16 >> 8) & 0xFFu);
    rc4_key[1] = (uint8_t)((((tsc16 >> 8) & 0xFFu) | 0x20u) & 0x7Fu);
    rc4_key[2] = (uint8_t)(tsc16 & 0xFFu);
    rc4_key[3] = (uint8_t)(((ppk[5] ^ tkip_get_u16_le(tk + 0)) >> 1) & 0xFFu);
    for (int k = 0; k < 6; k++) {
        rc4_key[4 + 2 * k] = (uint8_t)(ppk[k] & 0xFFu);
        rc4_key[5 + 2 * k] = (uint8_t)(ppk[k] >> 8);
    }
}

/* Isolated noinline RC4 stream cipher so S[256] stays in a ~288B leaf frame */
static __attribute__((noinline)) void tkip_rc4_crypt(const uint8_t key[16],
                                                     const uint8_t *in,
                                                     size_t len,
                                                     uint8_t *out) {
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + s[i] + key[i & 15]);
        uint8_t tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
    }
    uint8_t idx_i = 0, idx_j = 0;
    for (size_t k = 0; k < len; k++) {
        idx_i = (uint8_t)(idx_i + 1u);
        idx_j = (uint8_t)(idx_j + s[idx_i]);
        uint8_t tmp = s[idx_i];
        s[idx_i] = s[idx_j];
        s[idx_j] = tmp;
        uint8_t kstream = s[(uint8_t)(s[idx_i] + s[idx_j])];
        out[k] = (uint8_t)(in[k] ^ kstream);
    }
}

/* IEEE 802.11 CRC-32 for TKIP ICV */
static uint32_t tkip_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
    }
    return ~crc;
}

static inline void michael_block(uint32_t *l, uint32_t *r) {
    *r ^= tkip_rotl32(*l, 17);
    *l += *r;
    *r ^= (((*l & 0xFF00FF00u) >> 8) | ((*l & 0x00FF00FFu) << 8));
    *l += *r;
    *r ^= tkip_rotl32(*l, 3);
    *l += *r;
    *r ^= tkip_rotr32(*l, 2);
    *l += *r;
}

/* IEEE 802.11i Clause 12.5.2.2 Michael MIC over DA || SA || Priority || 0*3 || MSDU */
static __attribute__((noinline)) void tkip_michael_mic(const uint8_t mic_key[8],
                                                       const uint8_t da[6],
                                                       const uint8_t sa[6],
                                                       uint8_t priority,
                                                       const uint8_t *msdu,
                                                       size_t msdu_len,
                                                       uint8_t out_mic[8]) {
    uint32_t l = tkip_get_u32_le(mic_key + 0);
    uint32_t r = tkip_get_u32_le(mic_key + 4);

    uint8_t hdr[16] = {0};
    memcpy(hdr + 0, da, 6);
    memcpy(hdr + 6, sa, 6);
    hdr[12] = (uint8_t)(priority & 0x0Fu);

    for (size_t off = 0; off < 16; off += 4) {
        l ^= tkip_get_u32_le(hdr + off);
        michael_block(&l, &r);
    }

    size_t full_words = msdu_len / 4;
    for (size_t w = 0; w < full_words; w++) {
        l ^= tkip_get_u32_le(msdu + w * 4);
        michael_block(&l, &r);
    }

    /* Final padded word (0x5A followed by 4..7 zero bytes) */
    uint32_t last = 0;
    size_t rem = msdu_len & 3u;
    const uint8_t *tail = msdu + full_words * 4;
    for (size_t i = 0; i < rem; i++) {
        last |= ((uint32_t)tail[i]) << (i * 8);
    }
    last |= 0x5Au << (rem * 8);
    l ^= last;
    michael_block(&l, &r);

    /* Trailing zero word */
    l ^= 0u;
    michael_block(&l, &r);

    tkip_put_u32_le(out_mic + 0, l);
    tkip_put_u32_le(out_mic + 4, r);
}

int wlan_tkip_encrypt_frame(const uint8_t gtk[WLAN_GTK_TKIP_LEN],
                            uint64_t tsc,
                            uint8_t key_id,
                            const uint8_t *plain_frame, size_t plain_len,
                            uint8_t *out_frame, size_t *out_len) {
    if (!gtk || !plain_frame || plain_len < 24 || !out_frame || !out_len) return -1;
    uint16_t fc = (uint16_t)(plain_frame[0] | ((uint16_t)plain_frame[1] << 8));
    bool is_qos = ((fc >> 4) & 0x08u) != 0;
    size_t hdr_len = is_qos ? 26 : 24;
    if (plain_len < hdr_len) return -1;
    size_t payload_len = plain_len - hdr_len;
    if (*out_len < hdr_len + 8 + payload_len + 8 + 4) return -1;

    /* Extract DA, SA, TA, and Priority */
    const uint8_t *da = plain_frame + 4;
    const uint8_t *ta = plain_frame + 10;
    const uint8_t *sa = (fc & 0x0200u) ? (plain_frame + 16) :
                        ((fc & 0x0100u) ? (plain_frame + 10) : (plain_frame + 10));
    if ((fc & 0x0300u) == 0x0100u) {
        da = plain_frame + 16;
    }
    uint8_t prio = is_qos ? (uint8_t)(plain_frame[24] & 0x0Fu) : 0u;

    memcpy(out_frame, plain_frame, hdr_len);
    out_frame[1] |= 0x40u; /* Set Protected bit */

    /* Build 8-byte TKIP Extended IV */
    uint8_t *iv = out_frame + hdr_len;
    uint8_t tsc0 = (uint8_t)(tsc & 0xFFu);
    uint8_t tsc1 = (uint8_t)((tsc >> 8) & 0xFFu);
    iv[0] = tsc1;
    iv[1] = (uint8_t)((tsc1 | 0x20u) & 0x7Fu);
    iv[2] = tsc0;
    iv[3] = (uint8_t)(((key_id & 0x03u) << 6) | 0x20u); /* ExtIV = 1 */
    iv[4] = (uint8_t)((tsc >> 16) & 0xFFu);
    iv[5] = (uint8_t)((tsc >> 24) & 0xFFu);
    iv[6] = (uint8_t)((tsc >> 32) & 0xFFu);
    iv[7] = (uint8_t)((tsc >> 40) & 0xFFu);

    /* Stage plaintext + 8B Michael MIC + 4B CRC-32 ICV in out_frame + hdr_len + 8 */
    uint8_t *body = out_frame + hdr_len + 8;
    memcpy(body, plain_frame + hdr_len, payload_len);
    tkip_michael_mic(gtk + 16, da, sa, prio, body, payload_len, body + payload_len);
    uint32_t icv = tkip_crc32(body, payload_len + 8);
    tkip_put_u32_le(body + payload_len + 8, icv);

    uint8_t rc4_key[16];
    tkip_derive_rc4_key(gtk, ta, tsc, rc4_key);
    tkip_rc4_crypt(rc4_key, body, payload_len + 12, body);

    *out_len = hdr_len + 8 + payload_len + 12;
    return 0;
}

int wlan_tkip_decrypt_frame(const uint8_t gtk[WLAN_GTK_TKIP_LEN],
                            const uint8_t *cipher_frame, size_t cipher_len,
                            uint8_t *out_frame, size_t *out_len,
                            uint64_t *rx_tsc,
                            uint64_t *rx_bitmap) {
    if (!gtk || !cipher_frame || cipher_len < 44 || !out_frame || !out_len || !rx_tsc) return -1;
    uint16_t fc = (uint16_t)(cipher_frame[0] | ((uint16_t)cipher_frame[1] << 8));
    if (!(fc & 0x4000u)) return -1;

    bool is_qos = ((fc >> 4) & 0x08u) != 0;
    size_t hdr_len = is_qos ? 26 : 24;
    /* Require 8B TKIP ExtIV + at least 1B payload + 8B Michael MIC + 4B ICV = hdr_len + 20 */
    if (cipher_len < hdr_len + 20) return -1;
    size_t payload_len = cipher_len - hdr_len - 20;
    if (*out_len < hdr_len + payload_len + 12) return -1;

    const uint8_t *iv = cipher_frame + hdr_len;
    if (!(iv[3] & 0x20u)) return -1; /* ExtIV bit must be set */
    /* Verify WEPSeed invariant: iv[1] == ((iv[0] | 0x20) & 0x7F) */
    if (iv[1] != (uint8_t)((iv[0] | 0x20u) & 0x7Fu)) return -1;

    uint64_t tsc = (uint64_t)iv[2] |
                   ((uint64_t)iv[0] << 8) |
                   ((uint64_t)iv[4] << 16) |
                   ((uint64_t)iv[5] << 24) |
                   ((uint64_t)iv[6] << 32) |
                   ((uint64_t)iv[7] << 40);

    /* 64-packet sliding replay window check */
    if (*rx_tsc != 0) {
        if (tsc <= *rx_tsc) {
            uint64_t diff = *rx_tsc - tsc;
            if (diff >= 64) return -2;
            if (rx_bitmap && (*rx_bitmap & (1ULL << diff))) return -2;
        }
    }

    const uint8_t *ta = cipher_frame + 10;
    uint8_t rc4_key[16];
    tkip_derive_rc4_key(gtk, ta, tsc, rc4_key);

    /* Decrypt body (payload + 8B MIC + 4B ICV) into out_frame + hdr_len */
    uint8_t *plain_body = out_frame + hdr_len;
    tkip_rc4_crypt(rc4_key, cipher_frame + hdr_len + 8, payload_len + 12, plain_body);

    /* 1. Verify CRC-32 ICV */
    uint32_t expected_icv = tkip_get_u32_le(plain_body + payload_len + 8);
    uint32_t calc_icv = tkip_crc32(plain_body, payload_len + 8);
    if (expected_icv != calc_icv) {
        memset(out_frame, 0, hdr_len + payload_len + 12);
        return -1;
    }

    /* 2. Verify Michael MIC (AP-to-STA group frames use gtk[16..23]) */
    const uint8_t *da = cipher_frame + 4;
    const uint8_t *sa = (fc & 0x0200u) ? (cipher_frame + 16) :
                        ((fc & 0x0100u) ? (cipher_frame + 10) : (cipher_frame + 10));
    if ((fc & 0x0300u) == 0x0100u) {
        da = cipher_frame + 16;
    }
    uint8_t prio = is_qos ? (uint8_t)(cipher_frame[24] & 0x0Fu) : 0u;
    uint8_t calc_mic[8];
    tkip_michael_mic(gtk + 16, da, sa, prio, plain_body, payload_len, calc_mic);

    uint8_t diff_mic = 0;
    for (int i = 0; i < 8; i++) {
        diff_mic |= (uint8_t)(calc_mic[i] ^ plain_body[payload_len + i]);
    }
    if (diff_mic != 0) {
        memset(out_frame, 0, hdr_len + payload_len + 12);
        return -1;
    }

    /* Commit replay window state after both ICV and Michael MIC pass */
    if (*rx_tsc == 0) {
        *rx_tsc = (tsc == 0) ? 1 : tsc;
        if (rx_bitmap) *rx_bitmap = 1ULL;
    } else if (tsc > *rx_tsc) {
        uint64_t diff = tsc - *rx_tsc;
        if (rx_bitmap) {
            if (diff < 64) *rx_bitmap = (*rx_bitmap << diff) | 1ULL;
            else *rx_bitmap = 1ULL;
        }
        *rx_tsc = tsc;
    } else {
        uint64_t diff = *rx_tsc - tsc;
        if (rx_bitmap && diff < 64) {
            *rx_bitmap |= (1ULL << diff);
        }
    }

    memcpy(out_frame, cipher_frame, hdr_len);
    out_frame[1] &= ~0x40u; /* Clear Protected bit */
    memset(plain_body + payload_len, 0, 12); /* Wipe trailing MIC+ICV scratch */
    *out_len = hdr_len + payload_len;
    return 0;
}

/* ============================================================================
 * 8. SHA-256, HMAC-SHA-256 & IEEE 802.11 KDF-SHA-256 (FIPS 180-4)
 * ============================================================================ */

static const uint32_t s_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcU, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

void wlan_sha256_init(wlan_sha256_ctx_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static __attribute__((noinline)) void sha256_transform(uint32_t state[8], const uint8_t buffer[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4 + 0] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8)  |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = tkip_rotr32(w[i - 15], 7) ^ tkip_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = tkip_rotr32(w[i - 2], 17) ^ tkip_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = tkip_rotr32(e, 6) ^ tkip_rotr32(e, 11) ^ tkip_rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + s_sha256_k[i] + w[i];
        uint32_t S0 = tkip_rotr32(a, 2) ^ tkip_rotr32(a, 13) ^ tkip_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void wlan_sha256_update(wlan_sha256_ctx_t *ctx, const void *data, size_t len) {
    if (!ctx || !data || !len) return;
    const uint8_t *src = (const uint8_t *)data;
    size_t buf_idx = (size_t)(ctx->count & 0x3Fu);
    ctx->count += len;

    while (len > 0) {
        size_t take = 64 - buf_idx;
        if (take > len) take = len;
        memcpy(&ctx->buffer[buf_idx], src, take);
        src += take;
        len -= take;
        buf_idx += take;

        if (buf_idx == 64) {
            sha256_transform(ctx->state, ctx->buffer);
            buf_idx = 0;
        }
    }
}

void wlan_sha256_final(wlan_sha256_ctx_t *ctx, uint8_t digest[WLAN_SHA256_DIGEST_SIZE]) {
    if (!ctx || !digest) return;
    uint64_t total_bits = ctx->count * 8;
    uint8_t pad = 0x80;
    wlan_sha256_update(ctx, &pad, 1);

    uint8_t zero = 0x00;
    while ((ctx->count & 0x3Fu) != 56) {
        wlan_sha256_update(ctx, &zero, 1);
    }

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    }
    wlan_sha256_update(ctx, len_bytes, 8);

    for (int i = 0; i < 8; i++) {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void wlan_sha256(const void *data, size_t len, uint8_t digest[WLAN_SHA256_DIGEST_SIZE]) {
    wlan_sha256_ctx_t ctx;
    wlan_sha256_init(&ctx);
    wlan_sha256_update(&ctx, data, len);
    wlan_sha256_final(&ctx, digest);
}

__attribute__((noinline)) void wlan_hmac_sha256(const void *key, size_t key_len,
                                                const void *data, size_t data_len,
                                                uint8_t mac[WLAN_SHA256_DIGEST_SIZE]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        wlan_sha256(key, key_len, k);
    } else if (key && key_len > 0) {
        memcpy(k, key, key_len);
    }

    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36u);

    uint8_t inner_hash[WLAN_SHA256_DIGEST_SIZE];
    wlan_sha256_ctx_t ctx;
    wlan_sha256_init(&ctx);
    wlan_sha256_update(&ctx, pad, 64);
    if (data && data_len > 0) {
        wlan_sha256_update(&ctx, data, data_len);
    }
    wlan_sha256_final(&ctx, inner_hash);

    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    wlan_sha256_init(&ctx);
    wlan_sha256_update(&ctx, pad, 64);
    wlan_sha256_update(&ctx, inner_hash, WLAN_SHA256_DIGEST_SIZE);
    wlan_sha256_final(&ctx, mac);
}

/* IEEE 802.11-2020 12.7.1.7.2 KDF-Hash-Length (little-endian 16-bit counter i and Length) */
__attribute__((noinline)) void wlan_kdf_sha256(const uint8_t *key, size_t key_len,
                                               const char *label,
                                               const uint8_t *context, size_t context_len,
                                               uint8_t *out, size_t out_bits) {
    if (!key || !label || !out || out_bits == 0) return;
    size_t out_bytes = (out_bits + 7u) / 8u;
    size_t label_len = strlen(label);
    size_t iterations = (out_bits + 255u) / 256u;
    uint16_t len_le = (uint16_t)out_bits;
    uint8_t len_buf[2] = { (uint8_t)(len_le & 0xFFu), (uint8_t)(len_le >> 8) };

    uint8_t k[64] = {0};
    if (key_len > 64) {
        wlan_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    size_t offset = 0;
    for (uint16_t i = 1; i <= (uint16_t)iterations; i++) {
        uint8_t ctr_buf[2] = { (uint8_t)(i & 0xFFu), (uint8_t)(i >> 8) };
        uint8_t pad[64];
        uint8_t inner[WLAN_SHA256_DIGEST_SIZE];
        uint8_t block[WLAN_SHA256_DIGEST_SIZE];
        wlan_sha256_ctx_t ctx;

        for (int b = 0; b < 64; b++) pad[b] = (uint8_t)(k[b] ^ 0x36u);
        wlan_sha256_init(&ctx);
        wlan_sha256_update(&ctx, pad, 64);
        wlan_sha256_update(&ctx, ctr_buf, 2);
        wlan_sha256_update(&ctx, label, label_len);
        if (context && context_len > 0) {
            wlan_sha256_update(&ctx, context, context_len);
        }
        wlan_sha256_update(&ctx, len_buf, 2);
        wlan_sha256_final(&ctx, inner);

        for (int b = 0; b < 64; b++) pad[b] = (uint8_t)(k[b] ^ 0x5cu);
        wlan_sha256_init(&ctx);
        wlan_sha256_update(&ctx, pad, 64);
        wlan_sha256_update(&ctx, inner, WLAN_SHA256_DIGEST_SIZE);
        wlan_sha256_final(&ctx, block);

        size_t take = out_bytes - offset;
        if (take > WLAN_SHA256_DIGEST_SIZE) take = WLAN_SHA256_DIGEST_SIZE;
        memcpy(out + offset, block, take);
        offset += take;
    }
}

void wlan_derive_ptk_sha256(const uint8_t pmk[WLAN_PMK_LEN],
                            const uint8_t addr1[6], const uint8_t addr2[6],
                            const uint8_t nonce1[32], const uint8_t nonce2[32],
                            uint8_t ptk[WLAN_PTK_LEN]) {
    if (!pmk || !addr1 || !addr2 || !nonce1 || !nonce2 || !ptk) return;
    uint8_t data[76];
    if (memcmp(addr1, addr2, 6) < 0) {
        memcpy(data + 0, addr1, 6);
        memcpy(data + 6, addr2, 6);
    } else {
        memcpy(data + 0, addr2, 6);
        memcpy(data + 6, addr1, 6);
    }
    if (memcmp(nonce1, nonce2, 32) < 0) {
        memcpy(data + 12, nonce1, 32);
        memcpy(data + 44, nonce2, 32);
    } else {
        memcpy(data + 12, nonce2, 32);
        memcpy(data + 44, nonce1, 32);
    }
    /* WPA3-SAE AKM 00-0F-AC:8 uses KDF-SHA-256-384 (or 512) with "Pairwise key expansion" */
    wlan_kdf_sha256(pmk, WLAN_PMK_LEN, "Pairwise key expansion", data, 76, ptk, 384);
    memset(ptk + 48, 0, 16);
}

/* ============================================================================
 * 9. WPA3-Personal Simultaneous Authentication of Equals (SAE / NIST P-256)
 * ============================================================================ */

/* NIST P-256 (secp256r1, IEEE 802.11 Group 19) Constants in little-endian uint32_t[8] */
static const uint32_t s_p256_p[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};

static const uint32_t s_p256_r[8] = {
    0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
};

static const uint32_t s_p256_b[8] = {
    0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
    0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
};

/* (p + 1) / 4 for square root mod p */
static const uint32_t s_p256_sqrt_exp[8] = {
    0x00000000u, 0x00000000u, 0x40000000u, 0x00000000u,
    0x00000000u, 0x40000000u, 0xC0000000u, 0x3FFFFFFFu
};

static int u256_cmp(const uint32_t a[8], const uint32_t b[8]) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

static bool u256_is_zero(const uint32_t a[8]) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= a[i];
    return v == 0;
}

static void u256_from_be(uint32_t out[8], const uint8_t be[32]) {
    for (int i = 0; i < 8; i++) {
        int off = (7 - i) * 4;
        out[i] = ((uint32_t)be[off + 0] << 24) |
                 ((uint32_t)be[off + 1] << 16) |
                 ((uint32_t)be[off + 2] << 8)  |
                 ((uint32_t)be[off + 3]);
    }
}

static void u256_to_be(uint8_t be[32], const uint32_t in[8]) {
    for (int i = 0; i < 8; i++) {
        int off = (7 - i) * 4;
        be[off + 0] = (uint8_t)(in[i] >> 24);
        be[off + 1] = (uint8_t)(in[i] >> 16);
        be[off + 2] = (uint8_t)(in[i] >> 8);
        be[off + 3] = (uint8_t)(in[i]);
    }
}

static uint32_t u256_add_raw(uint32_t out[8], const uint32_t a[8], const uint32_t b[8]) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)a[i] + (uint64_t)b[i] + carry;
        out[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    return (uint32_t)carry;
}

static uint32_t u256_sub_raw(uint32_t out[8], const uint32_t a[8], const uint32_t b[8]) {
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t diff = (uint64_t)a[i] - (uint64_t)b[i] - borrow;
        out[i] = (uint32_t)diff;
        borrow = (diff >> 63) & 1u;
    }
    return (uint32_t)borrow;
}

static void u256_rshift1(uint32_t a[8], uint32_t top_carry) {
    for (int i = 0; i < 7; i++) {
        a[i] = (a[i] >> 1) | (a[i + 1] << 31);
    }
    a[7] = (a[7] >> 1) | (top_carry << 31);
}

static void u256_mod_add(uint32_t out[8], const uint32_t a[8], const uint32_t b[8], const uint32_t mod[8]) {
    uint32_t tmp[8];
    uint32_t carry = u256_add_raw(tmp, a, b);
    if (carry || u256_cmp(tmp, mod) >= 0) {
        u256_sub_raw(tmp, tmp, mod);
    }
    memcpy(out, tmp, 32);
}

static void u256_mod_sub(uint32_t out[8], const uint32_t a[8], const uint32_t b[8], const uint32_t mod[8]) {
    uint32_t tmp[8];
    uint32_t borrow = u256_sub_raw(tmp, a, b);
    if (borrow) {
        u256_add_raw(tmp, tmp, mod);
    }
    memcpy(out, tmp, 32);
}

static __attribute__((noinline)) void u256_mod_mul(uint32_t out[8],
                                                   const uint32_t a[8],
                                                   const uint32_t b[8],
                                                   const uint32_t mod[8]) {
    uint32_t acc[8] = {0};
    uint32_t b_mod[8];
    memcpy(b_mod, b, 32);
    if (u256_cmp(b_mod, mod) >= 0) {
        u256_sub_raw(b_mod, b_mod, mod);
    }
    for (int i = 255; i >= 0; i--) {
        u256_mod_add(acc, acc, acc, mod);
        if ((a[i >> 5] >> (i & 31)) & 1u) {
            u256_mod_add(acc, acc, b_mod, mod);
        }
    }
    memcpy(out, acc, 32);
}

/* Binary Extended GCD modular inverse mod prime (mod = s_p256_p or s_p256_r) */
static __attribute__((noinline)) void u256_mod_inv(uint32_t out[8],
                                                   const uint32_t a[8],
                                                   const uint32_t mod[8]) {
    if (u256_is_zero(a)) {
        memset(out, 0, 32);
        return;
    }
    uint32_t u[8], v[8], x1[8] = {1, 0, 0, 0, 0, 0, 0, 0}, x2[8] = {0};
    memcpy(u, a, 32);
    memcpy(v, mod, 32);

    while (!u256_is_zero(u) && !u256_is_zero(v)) {
        while ((u[0] & 1u) == 0) {
            u256_rshift1(u, 0);
            if ((x1[0] & 1u) == 0) {
                u256_rshift1(x1, 0);
            } else {
                uint32_t c = u256_add_raw(x1, x1, mod);
                u256_rshift1(x1, c);
            }
        }
        while ((v[0] & 1u) == 0) {
            u256_rshift1(v, 0);
            if ((x2[0] & 1u) == 0) {
                u256_rshift1(x2, 0);
            } else {
                uint32_t c = u256_add_raw(x2, x2, mod);
                u256_rshift1(x2, c);
            }
        }
        if (u256_cmp(u, v) >= 0) {
            u256_sub_raw(u, u, v);
            u256_mod_sub(x1, x1, x2, mod);
        } else {
            u256_sub_raw(v, v, u);
            u256_mod_sub(x2, x2, x1, mod);
        }
    }
    if (u256_is_zero(u)) {
        memcpy(out, x2, 32);
    } else {
        memcpy(out, x1, 32);
    }
}

static __attribute__((noinline)) void u256_mod_exp(uint32_t out[8],
                                                   const uint32_t base[8],
                                                   const uint32_t exp[8],
                                                   const uint32_t mod[8]) {
    uint32_t res[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 255; i >= 0; i--) {
        u256_mod_mul(res, res, res, mod);
        if ((exp[i >> 5] >> (i & 31)) & 1u) {
            u256_mod_mul(res, res, base, mod);
        }
    }
    memcpy(out, res, 32);
}

/* Compute rhs = x^3 - 3x + b mod p */
static __attribute__((noinline)) void p256_curve_rhs(uint32_t rhs[8], const uint32_t x[8]) {
    uint32_t x2[8], x3[8], three_x[8];
    u256_mod_mul(x2, x, x, s_p256_p);
    u256_mod_mul(x3, x2, x, s_p256_p);
    u256_mod_add(three_x, x, x, s_p256_p);
    u256_mod_add(three_x, three_x, x, s_p256_p);
    u256_mod_sub(rhs, x3, three_x, s_p256_p);
    u256_mod_add(rhs, rhs, s_p256_b, s_p256_p);
}

static __attribute__((noinline)) bool p256_is_on_curve(const uint32_t x[8], const uint32_t y[8]) {
    if (u256_cmp(x, s_p256_p) >= 0 || u256_cmp(y, s_p256_p) >= 0) return false;
    uint32_t lhs[8], rhs[8];
    u256_mod_mul(lhs, y, y, s_p256_p);
    p256_curve_rhs(rhs, x);
    return u256_cmp(lhs, rhs) == 0;
}

/* Affine point addition on NIST P-256: R = P + Q (returns false if result is point at infinity) */
static __attribute__((noinline)) bool p256_point_add(uint32_t rx[8], uint32_t ry[8],
                                                     const uint32_t px[8], const uint32_t py[8], bool p_inf,
                                                     const uint32_t qx[8], const uint32_t qy[8], bool q_inf) {
    if (p_inf) {
        if (q_inf) return false;
        memcpy(rx, qx, 32);
        memcpy(ry, qy, 32);
        return true;
    }
    if (q_inf) {
        memcpy(rx, px, 32);
        memcpy(ry, py, 32);
        return true;
    }

    uint32_t lambda[8], num[8], den[8], inv_den[8];
    if (u256_cmp(px, qx) == 0) {
        if (u256_cmp(py, qy) != 0 || u256_is_zero(py)) {
            return false; /* Point at infinity */
        }
        /* Point doubling: lambda = (3 * px^2 - 3) / (2 * py) mod p */
        u256_mod_mul(num, px, px, s_p256_p);
        uint32_t three_x2[8];
        u256_mod_add(three_x2, num, num, s_p256_p);
        u256_mod_add(three_x2, three_x2, num, s_p256_p);
        uint32_t three[8] = {3, 0, 0, 0, 0, 0, 0, 0};
        u256_mod_sub(num, three_x2, three, s_p256_p);
        u256_mod_add(den, py, py, s_p256_p);
    } else {
        /* Distinct points: lambda = (qy - py) / (qx - px) mod p */
        u256_mod_sub(num, qy, py, s_p256_p);
        u256_mod_sub(den, qx, px, s_p256_p);
    }

    u256_mod_inv(inv_den, den, s_p256_p);
    u256_mod_mul(lambda, num, inv_den, s_p256_p);

    /* x3 = lambda^2 - px - qx mod p */
    uint32_t x3[8], y3[8];
    u256_mod_mul(x3, lambda, lambda, s_p256_p);
    u256_mod_sub(x3, x3, px, s_p256_p);
    u256_mod_sub(x3, x3, qx, s_p256_p);

    /* y3 = lambda * (px - x3) - py mod p */
    u256_mod_sub(y3, px, x3, s_p256_p);
    u256_mod_mul(y3, lambda, y3, s_p256_p);
    u256_mod_sub(y3, y3, py, s_p256_p);

    memcpy(rx, x3, 32);
    memcpy(ry, ry ? y3 : x3, 32);
    return true;
}

/* Scalar multiplication R = scalar * P on NIST P-256 */
static __attribute__((noinline)) bool p256_point_mul(uint32_t rx[8], uint32_t ry[8],
                                                     const uint32_t scalar[8],
                                                     const uint32_t px[8], const uint32_t py[8]) {
    uint32_t ax[8] = {0}, ay[8] = {0};
    bool a_inf = true;

    for (int i = 255; i >= 0; i--) {
        if (!a_inf) {
            a_inf = !p256_point_add(ax, ay, ax, ay, false, ax, ay, false);
        }
        if ((scalar[i >> 5] >> (i & 31)) & 1u) {
            a_inf = !p256_point_add(ax, ay, ax, ay, a_inf, px, py, false);
        }
    }
    if (a_inf) return false;
    memcpy(rx, ax, 32);
    memcpy(ry, ay, 32);
    return true;
}

/* IEEE 802.11-2020 Clause 12.4.4.2.2 Hunting-and-Pecking PWE Derivation for ECC Group 19 */
__attribute__((noinline)) int wlan_sae_derive_pwe(wlan_sae_ctx_t *ctx,
                                                  const uint8_t sta_mac[6],
                                                  const uint8_t bssid[6],
                                                  const char *passphrase) {
    if (!ctx || !sta_mac || !bssid || !passphrase) return -1;
    size_t pass_len = strlen(passphrase);
    if (pass_len == 0) return -1;

    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->sta_mac, sta_mac, 6);
    memcpy(ctx->bssid, bssid, 6);

    /* Salt is max(STA_MAC, BSSID) || min(STA_MAC, BSSID) (12 bytes) */
    uint8_t salt[12];
    if (memcmp(sta_mac, bssid, 6) > 0) {
        memcpy(salt + 0, sta_mac, 6);
        memcpy(salt + 6, bssid, 6);
    } else {
        memcpy(salt + 0, bssid, 6);
        memcpy(salt + 6, sta_mac, 6);
    }

    uint32_t found_x[8] = {0}, found_y[8] = {0};
    bool found = false;

    /* Counter 1..40 hunting-and-pecking loop */
    for (uint8_t counter = 1; counter <= 40; counter++) {
        uint8_t pwd_seed[WLAN_SHA256_DIGEST_SIZE];
        uint8_t pwd_value[WLAN_SHA256_DIGEST_SIZE];

        /* pwd-seed = HMAC-SHA256(salt, passphrase || counter) */
        uint8_t msg[72];
        if (pass_len > 64) pass_len = 64;
        memcpy(msg, passphrase, pass_len);
        msg[pass_len] = counter;
        wlan_hmac_sha256(salt, sizeof(salt), msg, pass_len + 1, pwd_seed);

        /* pwd-value = KDF-SHA-256(pwd-seed, "SAE Hunting and Pecking", p_be, 256) */
        uint8_t p_be[32];
        u256_to_be(p_be, s_p256_p);
        wlan_kdf_sha256(pwd_seed, sizeof(pwd_seed), "SAE Hunting and Pecking", p_be, 32, pwd_value, 256);

        uint32_t cand_x[8];
        u256_from_be(cand_x, pwd_value);
        if (u256_cmp(cand_x, s_p256_p) >= 0 || u256_is_zero(cand_x)) {
            continue;
        }

        uint32_t rhs[8], cand_y[8], check[8];
        p256_curve_rhs(rhs, cand_x);
        u256_mod_exp(cand_y, rhs, s_p256_sqrt_exp, s_p256_p);
        u256_mod_mul(check, cand_y, cand_y, s_p256_p);
        if (u256_cmp(check, rhs) == 0 && !u256_is_zero(cand_y)) {
            /* Match LSB of cand_y with LSB of pwd-seed */
            if ((cand_y[0] & 1u) != (uint32_t)(pwd_seed[31] & 1u)) {
                u256_sub_raw(cand_y, s_p256_p, cand_y);
            }
            if (!found) {
                memcpy(found_x, cand_x, 32);
                memcpy(found_y, cand_y, 32);
                found = true;
                break;
            }
        }
    }

    if (!found) return -1;
    u256_to_be(ctx->pwe_x, found_x);
    u256_to_be(ctx->pwe_y, found_y);
    ctx->pwe_valid = true;
    return 0;
}

static void sae_prng_scalar(uint32_t out[8], uint32_t salt_tag, const uint8_t seed_material[32]) {
    static uint32_t s_sae_nonce_ctr = 1;
    uint8_t input[40];
    memcpy(input, seed_material, 32);
    tkip_put_u32_le(input + 32, salt_tag);
    tkip_put_u32_le(input + 36, s_sae_nonce_ctr++);

    uint8_t digest[32];
    wlan_sha256(input, sizeof(input), digest);
    u256_from_be(out, digest);
    /* Ensure 1 < out < r */
    if (u256_cmp(out, s_p256_r) >= 0) {
        u256_sub_raw(out, out, s_p256_r);
    }
    if (out[0] < 2 && out[1] == 0 && out[2] == 0) {
        out[0] = 2;
    }
}

__attribute__((noinline)) int wlan_sae_build_commit(wlan_sae_ctx_t *ctx,
                                                    uint8_t out_body[WLAN_SAE_COMMIT_LEN]) {
    if (!ctx || !ctx->pwe_valid || !out_body) return -1;

    uint32_t pwe_x[8], pwe_y[8], rand_v[8], mask_v[8], scalar[8], inv_mask[8], elem_x[8], elem_y[8];
    u256_from_be(pwe_x, ctx->pwe_x);
    u256_from_be(pwe_y, ctx->pwe_y);

    sae_prng_scalar(rand_v, 0x53414531u, ctx->pwe_x);
    sae_prng_scalar(mask_v, 0x53414532u, ctx->pwe_y);

    /* own_scalar = (rand + mask) mod r */
    u256_mod_add(scalar, rand_v, mask_v, s_p256_r);
    if (u256_is_zero(scalar) || (scalar[0] == 1 && scalar[1] == 0 && scalar[2] == 0)) {
        scalar[0] = 3;
    }

    /* own_element = (-mask mod r) * PWE */
    u256_sub_raw(inv_mask, s_p256_r, mask_v);
    if (!p256_point_mul(elem_x, elem_y, inv_mask, pwe_x, pwe_y)) {
        return -1;
    }

    u256_to_be(ctx->rand_val, rand_v);
    u256_to_be(ctx->mask_val, mask_v);
    u256_to_be(ctx->own_scalar, scalar);
    u256_to_be(ctx->own_elem_x, elem_x);
    u256_to_be(ctx->own_elem_y, elem_y);
    ctx->commit_ready = true;

    /* Group ID = 19 (little-endian 0x0013), followed by 32B scalar + 64B element (X || Y) */
    out_body[0] = (uint8_t)(WLAN_SAE_GROUP_P256 & 0xFFu);
    out_body[1] = (uint8_t)(WLAN_SAE_GROUP_P256 >> 8);
    memcpy(out_body + 2, ctx->own_scalar, 32);
    memcpy(out_body + 34, ctx->own_elem_x, 32);
    memcpy(out_body + 66, ctx->own_elem_y, 32);
    return 0;
}

__attribute__((noinline)) int wlan_sae_process_commit(wlan_sae_ctx_t *ctx,
                                                      const uint8_t *peer_body, size_t peer_len) {
    if (!ctx || !ctx->commit_ready || !peer_body || peer_len < WLAN_SAE_COMMIT_LEN) return -1;
    uint16_t group_id = (uint16_t)peer_body[0] | ((uint16_t)peer_body[1] << 8);
    if (group_id != WLAN_SAE_GROUP_P256) return -1;

    uint32_t peer_s[8], peer_ex[8], peer_ey[8], own_s[8];
    u256_from_be(peer_s, peer_body + 2);
    u256_from_be(peer_ex, peer_body + 34);
    u256_from_be(peer_ey, peer_body + 66);
    u256_from_be(own_s, ctx->own_scalar);

    /* Validate peer scalar in [2, r-1] and not a reflection of our own commit */
    if (u256_is_zero(peer_s) || (peer_s[0] == 1 && peer_s[1] == 0 && peer_s[2] == 0)) return -1;
    if (u256_cmp(peer_s, s_p256_r) >= 0) return -1;
    if (memcmp(peer_body + 2, ctx->own_scalar, 32) == 0 &&
        memcmp(peer_body + 34, ctx->own_elem_x, 32) == 0 &&
        memcmp(peer_body + 66, ctx->own_elem_y, 32) == 0) {
        return -1; /* Reflection attack */
    }

    /* Validate peer element is on NIST P-256 curve */
    if (!p256_is_on_curve(peer_ex, peer_ey)) return -1;

    /* Shared secret point K = rand * (peer_scalar * PWE + Peer_Element) */
    uint32_t pwe_x[8], pwe_y[8], sp_x[8], sp_y[8], sum_x[8], sum_y[8], rand_v[8], kx[8], ky[8];
    u256_from_be(pwe_x, ctx->pwe_x);
    u256_from_be(pwe_y, ctx->pwe_y);
    u256_from_be(rand_v, ctx->rand_val);

    if (!p256_point_mul(sp_x, sp_y, peer_s, pwe_x, pwe_y)) return -1;
    if (!p256_point_add(sum_x, sum_y, sp_x, sp_y, false, peer_ex, peer_ey, false)) return -1;
    if (!p256_point_mul(kx, ky, rand_v, sum_x, sum_y)) return -1;

    uint8_t k_be[32];
    u256_to_be(k_be, kx);

    /* keyseed = HMAC-SHA256(0^32, k_be) */
    uint8_t zero_key[32] = {0};
    uint8_t keyseed[32];
    wlan_hmac_sha256(zero_key, 32, k_be, 32, keyseed);

    /* context = (own_scalar + peer_scalar) mod r in 32-byte big-endian */
    uint32_t total_s[8];
    u256_mod_add(total_s, own_s, peer_s, s_p256_r);
    uint8_t ctx_be[32];
    u256_to_be(ctx_be, total_s);

    /* KCK || PMK = KDF-SHA-256(keyseed, "SAE KCK and PMK", ctx_be, 512) */
    uint8_t kck_pmk[64];
    wlan_kdf_sha256(keyseed, 32, "SAE KCK and PMK", ctx_be, 32, kck_pmk, 512);
    memcpy(ctx->kck, kck_pmk + 0, 32);
    memcpy(ctx->pmk, kck_pmk + 32, 32);
    /* PMKID = first 16 bytes of ctx_be = L((own_scalar + peer_scalar) mod r, 0, 128) */
    memcpy(ctx->pmkid, ctx_be, WLAN_PMKID_LEN);

    memcpy(ctx->peer_scalar, peer_body + 2, 32);
    memcpy(ctx->peer_elem_x, peer_body + 34, 32);
    memcpy(ctx->peer_elem_y, peer_body + 66, 32);
    ctx->peer_committed = true;
    return 0;
}

/* Compute SAE confirm token = HMAC-SHA256(kck, send_confirm_le16 || s1 || e1 || s2 || e2) */
static __attribute__((noinline)) void sae_compute_confirm_token(const uint8_t kck[32],
                                                                uint16_t send_confirm,
                                                                const uint8_t s1[32],
                                                                const uint8_t e1_x[32],
                                                                const uint8_t e1_y[32],
                                                                const uint8_t s2[32],
                                                                const uint8_t e2_x[32],
                                                                const uint8_t e2_y[32],
                                                                uint8_t out_token[32]) {
    uint8_t msg[2 + 32 + 64 + 32 + 64];
    msg[0] = (uint8_t)(send_confirm & 0xFFu);
    msg[1] = (uint8_t)(send_confirm >> 8);
    memcpy(msg + 2,   s1,   32);
    memcpy(msg + 34,  e1_x, 32);
    memcpy(msg + 66,  e1_y, 32);
    memcpy(msg + 98,  s2,   32);
    memcpy(msg + 130, e2_x, 32);
    memcpy(msg + 162, e2_y, 32);
    wlan_hmac_sha256(kck, 32, msg, sizeof(msg), out_token);
}

int wlan_sae_build_confirm(wlan_sae_ctx_t *ctx,
                           uint8_t out_body[WLAN_SAE_CONFIRM_LEN]) {
    if (!ctx || !ctx->peer_committed || !out_body) return -1;
    if (ctx->send_confirm < 0xFFFFu) ctx->send_confirm++;
    out_body[0] = (uint8_t)(ctx->send_confirm & 0xFFu);
    out_body[1] = (uint8_t)(ctx->send_confirm >> 8);
    sae_compute_confirm_token(ctx->kck, ctx->send_confirm,
                              ctx->own_scalar, ctx->own_elem_x, ctx->own_elem_y,
                              ctx->peer_scalar, ctx->peer_elem_x, ctx->peer_elem_y,
                              out_body + 2);
    return 0;
}

int wlan_sae_verify_confirm(wlan_sae_ctx_t *ctx,
                            const uint8_t *peer_body, size_t peer_len) {
    if (!ctx || !ctx->peer_committed || !peer_body || peer_len < WLAN_SAE_CONFIRM_LEN) return -1;
    uint16_t peer_sc = (uint16_t)peer_body[0] | ((uint16_t)peer_body[1] << 8);
    if (peer_sc == 0) return -1;

    uint8_t expected[32];
    sae_compute_confirm_token(ctx->kck, peer_sc,
                              ctx->peer_scalar, ctx->peer_elem_x, ctx->peer_elem_y,
                              ctx->own_scalar, ctx->own_elem_x, ctx->own_elem_y,
                              expected);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(expected[i] ^ peer_body[2 + i]);
    }
    if (diff != 0) return -1;
    ctx->confirmed = true;
    return 0;
}
