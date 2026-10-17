#ifndef FORTRESS_SSH_KEX_H
#define FORTRESS_SSH_KEX_H

#include "types.h"
#include "sshbuf.h"
#include "cipher.h"

typedef enum {
    KEX_STATE_INIT = 0,
    KEX_STATE_KEXINIT_SENT,
    KEX_STATE_KEXINIT_RECV,
    KEX_STATE_ECDH_DONE,
    KEX_STATE_NEWKEYS_SENT,
    KEX_STATE_ACTIVE
} kex_state_t;

typedef struct {
    kex_state_t state;
    bool strict_kex;
    uint8_t session_id[32];
    bool session_id_set;

    /* Ephemeral Curve25519 keypair */
    uint8_t server_priv[32];
    uint8_t server_pub[32];
    uint8_t client_pub[32];
    uint8_t shared_secret[32]; /* K */

    /* Exchange hash H */
    uint8_t hash[32];

    /* Saved KEXINIT payloads */
    uint8_t *client_kexinit;
    size_t client_kexinit_len;
    uint8_t *server_kexinit;
    size_t server_kexinit_len;

    /* Derived keys (64 bytes each for chacha20-poly1305) */
    uint8_t key_c2s[64];
    uint8_t key_s2c[64];
} ssh_kex_ctx;

void kex_init(ssh_kex_ctx *ctx);
void kex_cleanup(ssh_kex_ctx *ctx);
void kex_reset_for_rekey(ssh_kex_ctx *ctx);

int kex_build_kexinit(ssh_kex_ctx *ctx, const uint8_t cookie[16], sshbuf_t *out_payload);
int kex_parse_kexinit(ssh_kex_ctx *ctx, const uint8_t *payload, size_t len);

int kex_start_ecdh(ssh_kex_ctx *ctx);
int kex_process_ecdh_init(ssh_kex_ctx *ctx, const uint8_t *payload, size_t len,
                          const uint8_t host_pubkey[32],
                          const char *client_version, const char *server_version,
                          uint8_t out_hash[32]);

int kex_build_ecdh_reply(ssh_kex_ctx *ctx, const uint8_t host_pubkey[32],
                         const uint8_t signature[64], sshbuf_t *out_payload);

int kex_derive_keys(ssh_kex_ctx *ctx);

#endif /* FORTRESS_SSH_KEX_H */
