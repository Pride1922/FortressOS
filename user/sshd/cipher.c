#include "cipher.h"
#include "crypto.h"
#include "ssh_packet.h"
#include "string.h"

#include "../../src/lib/crypto/namespace.h"
#include "../../src/lib/crypto/upstream/src/monocypher.h"
#include "../../src/lib/crypto/namespace_end.h"

static void make_nonce(uint8_t nonce[8], uint32_t seq) {
    nonce[0] = 0;
    nonce[1] = 0;
    nonce[2] = 0;
    nonce[3] = 0;
    nonce[4] = (uint8_t)(seq >> 24);
    nonce[5] = (uint8_t)(seq >> 16);
    nonce[6] = (uint8_t)(seq >> 8);
    nonce[7] = (uint8_t)seq;
}

void ssh_cipher_init(ssh_cipher_ctx *ctx, const uint8_t key[64]) {
    if (!ctx || !key) return;
    memcpy(ctx->K_main, key, 32);
    memcpy(ctx->K_header, key + 32, 32);
    ctx->seq = 0;
    ctx->active = true;
}

void ssh_cipher_reset_seq(ssh_cipher_ctx *ctx) {
    if (ctx) ctx->seq = 0;
}

void ssh_cipher_cleanup(ssh_cipher_ctx *ctx) {
    if (ctx) {
        crypto_wipe(ctx, sizeof(*ctx));
    }
}

int ssh_cipher_decrypt_length(ssh_cipher_ctx *ctx, const uint8_t in[4], uint32_t *out_len) {
    if (!ctx || !in || !out_len) return SSH_ERR_INVALID_ARG;
    if (!ctx->active) {
        *out_len = ((uint32_t)in[0] << 24) |
                   ((uint32_t)in[1] << 16) |
                   ((uint32_t)in[2] << 8)  |
                   ((uint32_t)in[3]);
        if (*out_len < SSH_MIN_PACKET_LEN || *out_len > SSH_MAX_PACKET_LEN)
            return SSH_ERR_INVALID_FORMAT;
        return SSH_ERR_SUCCESS;
    }

    uint8_t nonce[8];
    make_nonce(nonce, ctx->seq);

    uint8_t dec_len[4];
    mono_crypto_chacha20_djb(dec_len, in, 4, ctx->K_header, nonce, 0);

    *out_len = ((uint32_t)dec_len[0] << 24) |
               ((uint32_t)dec_len[1] << 16) |
               ((uint32_t)dec_len[2] << 8)  |
               ((uint32_t)dec_len[3]);

    if (*out_len < SSH_MIN_PACKET_LEN || *out_len > SSH_MAX_PACKET_LEN)
        return SSH_ERR_INVALID_FORMAT;

    return SSH_ERR_SUCCESS;
}

int ssh_cipher_decrypt_packet(ssh_cipher_ctx *ctx, const uint8_t enc_length[4], uint32_t packet_len,
                              const uint8_t *enc_payload, const uint8_t tag[16], uint8_t *out_payload) {
    if (!ctx || !enc_length || !enc_payload || !out_payload) return SSH_ERR_INVALID_ARG;
    if (packet_len < SSH_MIN_PACKET_LEN || packet_len > SSH_MAX_PACKET_LEN)
        return SSH_ERR_INVALID_FORMAT;

    if (!ctx->active) {
        memcpy(out_payload, enc_payload, packet_len);
        ctx->seq++;
        return SSH_ERR_SUCCESS;
    }

    if (!tag) return SSH_ERR_INVALID_ARG;

    uint8_t nonce[8];
    make_nonce(nonce, ctx->seq);

    /* Generate Poly1305 key with counter = 0 */
    uint8_t poly_block[64];
    mono_crypto_chacha20_djb(poly_block, NULL, 64, ctx->K_main, nonce, 0);

    uint8_t poly_key[32];
    memcpy(poly_key, poly_block, 32);
    crypto_wipe(poly_block, sizeof(poly_block));

    /* Compute MAC over encrypted length (4 bytes) + encrypted payload (packet_len bytes) */
    mono_crypto_poly1305_ctx pctx;
    mono_crypto_poly1305_init(&pctx, poly_key);
    mono_crypto_poly1305_update(&pctx, enc_length, 4);
    mono_crypto_poly1305_update(&pctx, enc_payload, packet_len);

    uint8_t computed_tag[16];
    mono_crypto_poly1305_final(&pctx, computed_tag);
    crypto_wipe(poly_key, sizeof(poly_key));
    crypto_wipe(&pctx, sizeof(pctx));

    /* Constant-time tag comparison */
    int diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= (computed_tag[i] ^ tag[i]);
    }
    crypto_wipe(computed_tag, sizeof(computed_tag));

    if (diff != 0) {
        return SSH_ERR_INVALID_FORMAT; /* MAC verification failure */
    }

    /* Decrypt payload with counter = 1 */
    mono_crypto_chacha20_djb(out_payload, enc_payload, packet_len, ctx->K_main, nonce, 1);

    ctx->seq++;
    return SSH_ERR_SUCCESS;
}

int ssh_cipher_encrypt_packet(ssh_cipher_ctx *ctx, const uint8_t *plain_payload, uint32_t payload_len,
                              uint8_t enc_length[4], uint8_t *out_enc_payload, uint8_t tag[16]) {
    if (!ctx || !plain_payload || !enc_length || !out_enc_payload) return SSH_ERR_INVALID_ARG;
    if (payload_len < SSH_MIN_PACKET_LEN || payload_len > SSH_MAX_PACKET_LEN)
        return SSH_ERR_INVALID_FORMAT;

    if (!ctx->active) {
        enc_length[0] = (uint8_t)(payload_len >> 24);
        enc_length[1] = (uint8_t)(payload_len >> 16);
        enc_length[2] = (uint8_t)(payload_len >> 8);
        enc_length[3] = (uint8_t)payload_len;
        memcpy(out_enc_payload, plain_payload, payload_len);
        ctx->seq++;
        return SSH_ERR_SUCCESS;
    }

    if (!tag) return SSH_ERR_INVALID_ARG;

    uint8_t nonce[8];
    make_nonce(nonce, ctx->seq);

    /* Encrypt 4-byte length with counter = 0 and K_header */
    uint8_t raw_len[4];
    raw_len[0] = (uint8_t)(payload_len >> 24);
    raw_len[1] = (uint8_t)(payload_len >> 16);
    raw_len[2] = (uint8_t)(payload_len >> 8);
    raw_len[3] = (uint8_t)payload_len;
    mono_crypto_chacha20_djb(enc_length, raw_len, 4, ctx->K_header, nonce, 0);

    /* Encrypt payload with counter = 1 and K_main */
    mono_crypto_chacha20_djb(out_enc_payload, plain_payload, payload_len, ctx->K_main, nonce, 1);

    /* Generate Poly1305 key with counter = 0 and K_main */
    uint8_t poly_block[64];
    mono_crypto_chacha20_djb(poly_block, NULL, 64, ctx->K_main, nonce, 0);

    uint8_t poly_key[32];
    memcpy(poly_key, poly_block, 32);
    crypto_wipe(poly_block, sizeof(poly_block));

    /* Compute MAC over encrypted length + encrypted payload */
    mono_crypto_poly1305_ctx pctx;
    mono_crypto_poly1305_init(&pctx, poly_key);
    mono_crypto_poly1305_update(&pctx, enc_length, 4);
    mono_crypto_poly1305_update(&pctx, out_enc_payload, payload_len);
    mono_crypto_poly1305_final(&pctx, tag);
    crypto_wipe(poly_key, sizeof(poly_key));
    crypto_wipe(&pctx, sizeof(pctx));

    ctx->seq++;
    return SSH_ERR_SUCCESS;
}
