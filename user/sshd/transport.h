#ifndef FORTRESS_SSH_TRANSPORT_H
#define FORTRESS_SSH_TRANSPORT_H

#include "types.h"
#include "sshbuf.h"
#include "ssh_packet.h"
#include "cipher.h"
#include "kex.h"

typedef enum {
    TRANSPORT_STATE_INIT = 0,
    TRANSPORT_STATE_KEX,
    TRANSPORT_STATE_ACTIVE,
    TRANSPORT_STATE_REKEY,
    TRANSPORT_STATE_ERROR
} transport_state_t;

typedef struct {
    transport_state_t state;
    bool strict_kex;
    bool initial_kex_done;

    /* Cryptographic and KEX contexts */
    ssh_kex_ctx kex;
    ssh_cipher_ctx in_cipher;
    ssh_cipher_ctx out_cipher;

    /* Stream receive buffer */
    sshbuf_t in_buf;

    /* Current incoming decrypted packet scratch */
    uint8_t packet_buf[SSH_MAX_PACKET_LEN + 32];
    size_t packet_len;
    uint8_t packet_type;
    const uint8_t *payload;
    size_t payload_len;

    /* Error details if in TRANSPORT_STATE_ERROR */
    int last_error;
    uint32_t disconnect_reason;
} ssh_transport_t;

int ssh_transport_init(ssh_transport_t *t);
void ssh_transport_cleanup(ssh_transport_t *t);

/* Feed raw TCP stream bytes into the transport receiver */
int ssh_transport_feed_input(ssh_transport_t *t, const uint8_t *data, size_t len);

/* Extract next packet from stream. Returns SSH_ERR_SUCCESS if packet is ready,
 * SSH_ERR_AGAIN if more data is needed, or negative error code on failure. */
int ssh_transport_get_packet(ssh_transport_t *t, uint8_t *out_type, const uint8_t **out_payload, size_t *out_len);

/* Construct and encrypt (if active) an SSH packet for sending onto wire */
int ssh_transport_send_packet(ssh_transport_t *t, uint8_t type, const uint8_t *payload, size_t payload_len, sshbuf_t *out_wire);

/* Apply newly negotiated keys after receiving/sending SSH_MSG_NEWKEYS */
int ssh_transport_apply_newkeys(ssh_transport_t *t, bool is_rekey);

/* Initiate rekeying */
int ssh_transport_start_rekey(ssh_transport_t *t, const uint8_t cookie[16], sshbuf_t *out_wire);

#endif /* FORTRESS_SSH_TRANSPORT_H */
