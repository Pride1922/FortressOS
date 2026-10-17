#ifndef FORTRESS_SSH_CIPHER_H
#define FORTRESS_SSH_CIPHER_H

#include "types.h"
#include "sshbuf.h"

#define SSH_CIPHER_KEY_LEN 64
#define SSH_CIPHER_TAG_LEN 16

typedef struct {
    uint8_t K_main[32];   /* payload encryption key + poly1305 key generator */
    uint8_t K_header[32]; /* packet length encryption key */
    uint32_t seq;         /* packet sequence number */
    bool active;          /* whether cipher is active */
} ssh_cipher_ctx;

void ssh_cipher_init(ssh_cipher_ctx *ctx, const uint8_t key[64]);
void ssh_cipher_reset_seq(ssh_cipher_ctx *ctx);
void ssh_cipher_cleanup(ssh_cipher_ctx *ctx);

int ssh_cipher_decrypt_length(ssh_cipher_ctx *ctx, const uint8_t in[4], uint32_t *out_len);
int ssh_cipher_decrypt_packet(ssh_cipher_ctx *ctx, const uint8_t enc_length[4], uint32_t packet_len,
                              const uint8_t *enc_payload, const uint8_t tag[16], uint8_t *out_payload);

int ssh_cipher_encrypt_packet(ssh_cipher_ctx *ctx, const uint8_t *plain_payload, uint32_t payload_len,
                              uint8_t enc_length[4], uint8_t *out_enc_payload, uint8_t tag[16]);

#endif /* FORTRESS_SSH_CIPHER_H */
