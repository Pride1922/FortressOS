#include "kex.h"
#include "crypto.h"
#include "ssh_packet.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <stdlib.h>
#define kex_alloc(s) malloc(s)
#define kex_free(p) free(p)
#else
#define kex_alloc(s) ssh_alloc(s)
#define kex_free(p) ssh_free(p)
#endif

void kex_init(ssh_kex_ctx *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = KEX_STATE_INIT;
}

void kex_cleanup(ssh_kex_ctx *ctx) {
    if (!ctx) return;
    crypto_wipe(ctx->server_priv, sizeof(ctx->server_priv));
    crypto_wipe(ctx->shared_secret, sizeof(ctx->shared_secret));
    crypto_wipe(ctx->key_c2s, sizeof(ctx->key_c2s));
    crypto_wipe(ctx->key_s2c, sizeof(ctx->key_s2c));
    crypto_wipe(ctx->hash, sizeof(ctx->hash));
    if (ctx->client_kexinit) {
        kex_free(ctx->client_kexinit);
        ctx->client_kexinit = NULL;
    }
    if (ctx->server_kexinit) {
        kex_free(ctx->server_kexinit);
        ctx->server_kexinit = NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
}

void kex_reset_for_rekey(ssh_kex_ctx *ctx) {
    if (!ctx) return;
    crypto_wipe(ctx->server_priv, sizeof(ctx->server_priv));
    crypto_wipe(ctx->shared_secret, sizeof(ctx->shared_secret));
    crypto_wipe(ctx->key_c2s, sizeof(ctx->key_c2s));
    crypto_wipe(ctx->key_s2c, sizeof(ctx->key_s2c));
    crypto_wipe(ctx->hash, sizeof(ctx->hash));
    crypto_wipe(ctx->client_pub, sizeof(ctx->client_pub));
    crypto_wipe(ctx->server_pub, sizeof(ctx->server_pub));
    if (ctx->client_kexinit) {
        kex_free(ctx->client_kexinit);
        ctx->client_kexinit = NULL;
    }
    if (ctx->server_kexinit) {
        kex_free(ctx->server_kexinit);
        ctx->server_kexinit = NULL;
    }
    ctx->client_kexinit_len = 0;
    ctx->server_kexinit_len = 0;
    ctx->state = KEX_STATE_INIT;
    /* ctx->session_id, ctx->session_id_set, ctx->strict_kex are retained */
}

int kex_build_kexinit(ssh_kex_ctx *ctx, const uint8_t cookie[16], sshbuf_t *out_payload) {
    if (!ctx || !cookie || !out_payload) return SSH_ERR_INVALID_ARG;

    size_t start_off = out_payload->size;

    int r = sshbuf_put_u8(out_payload, SSH_MSG_KEXINIT);
    if (r) return r;
    r = sshbuf_put_raw(out_payload, cookie, 16);
    if (r) return r;

    /* kex_algorithms */
    r = sshbuf_put_cstring(out_payload, SSH_KEX_CURVE25519_SHA256 "," SSH_KEX_STRICT_SERVER);
    if (r) return r;
    /* server_host_key_algorithms */
    r = sshbuf_put_cstring(out_payload, SSH_HOSTKEY_ED25519);
    if (r) return r;
    /* encryption_algorithms_client_to_server */
    r = sshbuf_put_cstring(out_payload, SSH_CIPHER_CHACHA20_POLY1305);
    if (r) return r;
    /* encryption_algorithms_server_to_client */
    r = sshbuf_put_cstring(out_payload, SSH_CIPHER_CHACHA20_POLY1305);
    if (r) return r;
    /* mac_algorithms_client_to_server (implicit in AEAD, send empty) */
    r = sshbuf_put_cstring(out_payload, "");
    if (r) return r;
    /* mac_algorithms_server_to_client */
    r = sshbuf_put_cstring(out_payload, "");
    if (r) return r;
    /* compression_algorithms_client_to_server */
    r = sshbuf_put_cstring(out_payload, "none");
    if (r) return r;
    /* compression_algorithms_server_to_client */
    r = sshbuf_put_cstring(out_payload, "none");
    if (r) return r;
    /* languages_client_to_server */
    r = sshbuf_put_cstring(out_payload, "");
    if (r) return r;
    /* languages_server_to_client */
    r = sshbuf_put_cstring(out_payload, "");
    if (r) return r;

    /* first_kex_packet_follows = 0 */
    r = sshbuf_put_u8(out_payload, 0);
    if (r) return r;
    /* uint32 reserved = 0 */
    r = sshbuf_put_u32(out_payload, 0);
    if (r) return r;

    /* Save a copy for exchange hash H */
    size_t kexinit_len = out_payload->size - start_off;
    if (ctx->server_kexinit) kex_free(ctx->server_kexinit);
    ctx->server_kexinit = (uint8_t *)kex_alloc(kexinit_len);
    if (!ctx->server_kexinit) return SSH_ERR_NO_MEM;
    memcpy(ctx->server_kexinit, out_payload->data + start_off, kexinit_len);
    ctx->server_kexinit_len = kexinit_len;

    ctx->state = KEX_STATE_KEXINIT_SENT;
    return SSH_ERR_SUCCESS;
}

static bool namelist_contains(const char *list, size_t list_len, const char *target) {
    if (!list || !target) return false;
    size_t target_len = strlen(target);
    size_t i = 0;
    while (i < list_len) {
        size_t next_comma = i;
        while (next_comma < list_len && list[next_comma] != ',') next_comma++;
        size_t item_len = next_comma - i;
        if (item_len == target_len && memcmp(list + i, target, item_len) == 0) {
            return true;
        }
        i = next_comma + 1;
    }
    return false;
}

int kex_parse_kexinit(ssh_kex_ctx *ctx, const uint8_t *payload, size_t len) {
    if (!ctx || !payload || len < 17) return SSH_ERR_INVALID_ARG;

    sshbuf_t b;
    sshbuf_init_from(&b, payload, len);

    uint8_t msg = 0;
    int r = sshbuf_get_u8(&b, &msg);
    if (r || msg != SSH_MSG_KEXINIT) return SSH_ERR_INVALID_FORMAT;

    uint8_t cookie[16];
    r = sshbuf_get_raw(&b, cookie, 16);
    if (r) return r;

    const uint8_t *kex_algs = NULL;
    size_t kex_algs_len = 0;
    r = sshbuf_get_string_ptr(&b, &kex_algs, &kex_algs_len);
    if (r) return r;

    const uint8_t *hostkey_algs = NULL;
    size_t hostkey_algs_len = 0;
    r = sshbuf_get_string_ptr(&b, &hostkey_algs, &hostkey_algs_len);
    if (r) return r;

    const uint8_t *ciphers_c2s = NULL;
    size_t ciphers_c2s_len = 0;
    r = sshbuf_get_string_ptr(&b, &ciphers_c2s, &ciphers_c2s_len);
    if (r) return r;

    const uint8_t *ciphers_s2c = NULL;
    size_t ciphers_s2c_len = 0;
    r = sshbuf_get_string_ptr(&b, &ciphers_s2c, &ciphers_s2c_len);
    if (r) return r;

    /* Verify algorithm intersection */
    if (!namelist_contains((const char *)kex_algs, kex_algs_len, SSH_KEX_CURVE25519_SHA256))
        return SSH_ERR_INVALID_FORMAT;
    if (!namelist_contains((const char *)hostkey_algs, hostkey_algs_len, SSH_HOSTKEY_ED25519))
        return SSH_ERR_INVALID_FORMAT;
    if (!namelist_contains((const char *)ciphers_c2s, ciphers_c2s_len, SSH_CIPHER_CHACHA20_POLY1305))
        return SSH_ERR_INVALID_FORMAT;
    if (!namelist_contains((const char *)ciphers_s2c, ciphers_s2c_len, SSH_CIPHER_CHACHA20_POLY1305))
        return SSH_ERR_INVALID_FORMAT;

    /* Check if strict KEX requested by client */
    if (namelist_contains((const char *)kex_algs, kex_algs_len, SSH_KEX_STRICT_CLIENT)) {
        ctx->strict_kex = true;
    }

    /* Save a copy of client kexinit for exchange hash H */
    if (ctx->client_kexinit) kex_free(ctx->client_kexinit);
    ctx->client_kexinit = (uint8_t *)kex_alloc(len);
    if (!ctx->client_kexinit) return SSH_ERR_NO_MEM;
    memcpy(ctx->client_kexinit, payload, len);
    ctx->client_kexinit_len = len;

    ctx->state = KEX_STATE_KEXINIT_RECV;
    return SSH_ERR_SUCCESS;
}

int kex_start_ecdh(ssh_kex_ctx *ctx) {
    if (!ctx) return SSH_ERR_INVALID_ARG;
    return crypto_x25519_keygen(ctx->server_priv, ctx->server_pub);
}

int kex_process_ecdh_init(ssh_kex_ctx *ctx, const uint8_t *payload, size_t len,
                          const uint8_t host_pubkey[32],
                          const char *client_version, const char *server_version,
                          uint8_t out_hash[32]) {
    if (!ctx || !payload || !host_pubkey || !client_version || !server_version || !out_hash)
        return SSH_ERR_INVALID_ARG;

    sshbuf_t b;
    sshbuf_init_from(&b, payload, len);

    uint8_t msg = 0;
    int r = sshbuf_get_u8(&b, &msg);
    if (r || msg != SSH_MSG_KEX_ECDH_INIT) return SSH_ERR_INVALID_FORMAT;

    const uint8_t *client_pub = NULL;
    size_t client_pub_len = 0;
    r = sshbuf_get_string_ptr(&b, &client_pub, &client_pub_len);
    if (r || client_pub_len != 32) return SSH_ERR_INVALID_FORMAT;
    memcpy(ctx->client_pub, client_pub, 32);

    /* Low-order all-zero point check */
    int all_zero = 0;
    for (int i = 0; i < 32; i++) all_zero |= ctx->client_pub[i];
    if (all_zero == 0) return SSH_ERR_INVALID_FORMAT;

    /* Compute shared secret K = X25519(d_server, Q_client) */
    r = crypto_x25519(ctx->shared_secret, ctx->server_priv, ctx->client_pub);
    if (r) return SSH_ERR_INVALID_FORMAT;

    /* Low-order shared secret refusal (RFC 7748 / RFC 8731) */
    all_zero = 0;
    for (int i = 0; i < 32; i++) all_zero |= ctx->shared_secret[i];
    if (all_zero == 0) return SSH_ERR_INVALID_FORMAT;

    /* Build host key blob Ks: string "ssh-ed25519" + string host_pubkey[32] */
    sshbuf_t ks_buf;
    sshbuf_init(&ks_buf, 64, 256);
    sshbuf_put_cstring(&ks_buf, SSH_HOSTKEY_ED25519);
    sshbuf_put_string(&ks_buf, host_pubkey, 32);

    /* Compute exchange hash H = SHA256(Vc || Vs || Ic || Is || Ks || Qc || Qs || K) */
    sshbuf_t hbuf;
    sshbuf_init(&hbuf, 4096, 65536);
    sshbuf_put_cstring(&hbuf, client_version);
    sshbuf_put_cstring(&hbuf, server_version);
    sshbuf_put_string(&hbuf, ctx->client_kexinit, ctx->client_kexinit_len);
    sshbuf_put_string(&hbuf, ctx->server_kexinit, ctx->server_kexinit_len);
    sshbuf_put_string(&hbuf, ks_buf.data, ks_buf.size);
    sshbuf_put_string(&hbuf, ctx->client_pub, 32);
    sshbuf_put_string(&hbuf, ctx->server_pub, 32);
    sshbuf_put_mpint(&hbuf, ctx->shared_secret, 32);

    crypto_sha256(hbuf.data, hbuf.size, ctx->hash);
    memcpy(out_hash, ctx->hash, 32);

    if (!ctx->session_id_set) {
        memcpy(ctx->session_id, ctx->hash, 32);
        ctx->session_id_set = true;
    }

    sshbuf_free(&ks_buf);
    sshbuf_free(&hbuf);

    ctx->state = KEX_STATE_ECDH_DONE;
    return SSH_ERR_SUCCESS;
}

int kex_build_ecdh_reply(ssh_kex_ctx *ctx, const uint8_t host_pubkey[32],
                         const uint8_t signature[64], sshbuf_t *out_payload) {
    if (!ctx || !host_pubkey || !signature || !out_payload) return SSH_ERR_INVALID_ARG;

    /* Host key blob Ks */
    sshbuf_t ks_buf;
    sshbuf_init(&ks_buf, 64, 256);
    sshbuf_put_cstring(&ks_buf, SSH_HOSTKEY_ED25519);
    sshbuf_put_string(&ks_buf, host_pubkey, 32);

    /* Signature blob: string "ssh-ed25519" + string signature[64] */
    sshbuf_t sig_buf;
    sshbuf_init(&sig_buf, 128, 256);
    sshbuf_put_cstring(&sig_buf, SSH_HOSTKEY_ED25519);
    sshbuf_put_string(&sig_buf, signature, 64);

    /* SSH_MSG_KEX_ECDH_REPLY packet */
    int r = sshbuf_put_u8(out_payload, SSH_MSG_KEX_ECDH_REPLY);
    if (!r) r = sshbuf_put_string(out_payload, ks_buf.data, ks_buf.size);
    if (!r) r = sshbuf_put_string(out_payload, ctx->server_pub, 32);
    if (!r) r = sshbuf_put_string(out_payload, sig_buf.data, sig_buf.size);

    sshbuf_free(&ks_buf);
    sshbuf_free(&sig_buf);
    return r;
}

static void derive_key_64(const uint8_t *k_mpint, size_t k_mpint_len,
                          const uint8_t hash[32], char tag,
                          const uint8_t session_id[32], uint8_t out_key[64]) {
    /* K1 = SHA256(K_mpint || H || tag || session_id) */
    crypto_sha256_ctx ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, k_mpint, k_mpint_len);
    crypto_sha256_update(&ctx, hash, 32);
    crypto_sha256_update(&ctx, &tag, 1);
    crypto_sha256_update(&ctx, session_id, 32);
    crypto_sha256_final(&ctx, out_key);

    /* K2 = SHA256(K_mpint || H || K1) */
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, k_mpint, k_mpint_len);
    crypto_sha256_update(&ctx, hash, 32);
    crypto_sha256_update(&ctx, out_key, 32);
    crypto_sha256_final(&ctx, out_key + 32);
}

int kex_derive_keys(ssh_kex_ctx *ctx) {
    if (!ctx || ctx->state < KEX_STATE_ECDH_DONE) return SSH_ERR_INVALID_ARG;

    /* Encode K as mpint */
    sshbuf_t kbuf;
    sshbuf_init(&kbuf, 64, 128);
    int r = sshbuf_put_mpint(&kbuf, ctx->shared_secret, 32);
    if (r) {
        sshbuf_free(&kbuf);
        return r;
    }

    /* Client-to-server keys: tag 'C' */
    derive_key_64(kbuf.data, kbuf.size, ctx->hash, 'C', ctx->session_id, ctx->key_c2s);

    /* Server-to-client keys: tag 'D' */
    derive_key_64(kbuf.data, kbuf.size, ctx->hash, 'D', ctx->session_id, ctx->key_s2c);

    sshbuf_free(&kbuf);
    return SSH_ERR_SUCCESS;
}
