#include "transport.h"
#include "crypto.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <stdlib.h>
#endif

int ssh_transport_init(ssh_transport_t *t) {
    if (!t) return SSH_ERR_INVALID_ARG;
    memset(t, 0, sizeof(*t));
    t->state = TRANSPORT_STATE_INIT;
    kex_init(&t->kex);
    return sshbuf_init(&t->in_buf, 4096, 65536);
}

void ssh_transport_cleanup(ssh_transport_t *t) {
    if (!t) return;
    kex_cleanup(&t->kex);
    ssh_cipher_cleanup(&t->in_cipher);
    ssh_cipher_cleanup(&t->out_cipher);
    sshbuf_free(&t->in_buf);
    crypto_wipe(t->packet_buf, sizeof(t->packet_buf));
    memset(t, 0, sizeof(*t));
    t->state = TRANSPORT_STATE_INIT;
}

int ssh_transport_feed_input(ssh_transport_t *t, const uint8_t *data, size_t len) {
    if (!t || (!data && len > 0)) return SSH_ERR_INVALID_ARG;
    if (t->state == TRANSPORT_STATE_ERROR) return t->last_error;

    if (t->in_buf.off >= 4096 || t->in_buf.off == t->in_buf.size) {
        sshbuf_compact(&t->in_buf);
    }
    return sshbuf_put_raw(&t->in_buf, data, len);
}

int ssh_transport_get_packet(ssh_transport_t *t, uint8_t *out_type, const uint8_t **out_payload, size_t *out_len) {
    if (!t || !out_type || !out_payload || !out_len) return SSH_ERR_INVALID_ARG;
    if (t->state == TRANSPORT_STATE_ERROR) return t->last_error;

    size_t avail = sshbuf_len(&t->in_buf);
    if (avail < 4) return SSH_ERR_AGAIN;

    const uint8_t *in_ptr = sshbuf_ptr(&t->in_buf);

    if (!t->in_cipher.active) {
        /* Unencrypted mode */
        uint32_t packet_len = ((uint32_t)in_ptr[0] << 24) |
                              ((uint32_t)in_ptr[1] << 16) |
                              ((uint32_t)in_ptr[2] << 8)  |
                              ((uint32_t)in_ptr[3]);

        if (packet_len < SSH_MIN_PACKET_LEN || packet_len > SSH_MAX_PACKET_LEN) {
            t->state = TRANSPORT_STATE_ERROR;
            t->last_error = SSH_ERR_INVALID_FORMAT;
            t->disconnect_reason = SSH_DISCONNECT_PROTOCOL_ERROR;
            return SSH_ERR_INVALID_FORMAT;
        }

        if (avail < 4 + packet_len) {
            return SSH_ERR_AGAIN;
        }

        sshbuf_consume(&t->in_buf, 4);
        memcpy(t->packet_buf, sshbuf_ptr(&t->in_buf), packet_len);
        sshbuf_consume(&t->in_buf, packet_len);

        uint8_t pad_len = t->packet_buf[0];
        if (pad_len < 4 || packet_len < (size_t)(1 + pad_len + 1)) {
            t->state = TRANSPORT_STATE_ERROR;
            t->last_error = SSH_ERR_INVALID_FORMAT;
            t->disconnect_reason = SSH_DISCONNECT_PROTOCOL_ERROR;
            return SSH_ERR_INVALID_FORMAT;
        }

        uint8_t msg = t->packet_buf[1];

        /* Strict KEX pre-KEX verification: only KEX messages allowed before NEWKEYS */
        if (t->strict_kex && !t->initial_kex_done) {
            if (msg != SSH_MSG_KEXINIT &&
                msg != SSH_MSG_KEX_ECDH_INIT &&
                msg != SSH_MSG_KEX_ECDH_REPLY &&
                msg != SSH_MSG_NEWKEYS) {
                t->state = TRANSPORT_STATE_ERROR;
                t->last_error = SSH_ERR_PROTOCOL_VIOLATION;
                t->disconnect_reason = SSH_DISCONNECT_PROTOCOL_ERROR;
                return SSH_ERR_PROTOCOL_VIOLATION;
            }
        }

        if (msg == SSH_MSG_KEXINIT) {
            t->state = TRANSPORT_STATE_KEX;
        }

        t->in_cipher.seq++;

        t->packet_len = packet_len;
        t->packet_type = msg;
        t->payload = t->packet_buf + 1;
        t->payload_len = packet_len - 1 - pad_len;

        *out_type = t->packet_type;
        *out_payload = t->payload;
        *out_len = t->payload_len;

        if (t->in_buf.off >= 4096 || t->in_buf.off == t->in_buf.size) {
            sshbuf_compact(&t->in_buf);
        }
        return SSH_ERR_SUCCESS;
    } else {
        /* Encrypted mode (ChaCha20-Poly1305) */
        uint32_t packet_len = 0;
        int r = ssh_cipher_decrypt_length(&t->in_cipher, in_ptr, &packet_len);
        if (r != SSH_ERR_SUCCESS || packet_len < SSH_MIN_PACKET_LEN || packet_len > SSH_MAX_PACKET_LEN) {
            t->state = TRANSPORT_STATE_ERROR;
            t->last_error = SSH_ERR_INVALID_FORMAT;
            t->disconnect_reason = SSH_DISCONNECT_PROTOCOL_ERROR;
            return SSH_ERR_INVALID_FORMAT;
        }

        size_t total_frame = 4 + packet_len + 16;
        if (avail < total_frame) {
            return SSH_ERR_AGAIN;
        }

        const uint8_t *enc_length = in_ptr;
        const uint8_t *enc_payload = in_ptr + 4;
        const uint8_t *tag = in_ptr + 4 + packet_len;

        r = ssh_cipher_decrypt_packet(&t->in_cipher, enc_length, packet_len,
                                      enc_payload, tag, t->packet_buf);
        if (r != SSH_ERR_SUCCESS) {
            t->state = TRANSPORT_STATE_ERROR;
            t->last_error = SSH_ERR_INVALID_FORMAT;
            t->disconnect_reason = SSH_DISCONNECT_MAC_ERROR;
            return SSH_ERR_INVALID_FORMAT;
        }

        sshbuf_consume(&t->in_buf, total_frame);

        uint8_t pad_len = t->packet_buf[0];
        if (pad_len < 4 || packet_len < (size_t)(1 + pad_len + 1)) {
            t->state = TRANSPORT_STATE_ERROR;
            t->last_error = SSH_ERR_INVALID_FORMAT;
            t->disconnect_reason = SSH_DISCONNECT_PROTOCOL_ERROR;
            return SSH_ERR_INVALID_FORMAT;
        }

        uint8_t msg = t->packet_buf[1];

        if (msg == SSH_MSG_KEXINIT) {
            t->state = TRANSPORT_STATE_REKEY;
        }

        t->packet_len = packet_len;
        t->packet_type = msg;
        t->payload = t->packet_buf + 1;
        t->payload_len = packet_len - 1 - pad_len;

        *out_type = t->packet_type;
        *out_payload = t->payload;
        *out_len = t->payload_len;

        if (t->in_buf.off >= 4096 || t->in_buf.off == t->in_buf.size) {
            sshbuf_compact(&t->in_buf);
        }
        return SSH_ERR_SUCCESS;
    }
}

int ssh_transport_send_packet(ssh_transport_t *t, uint8_t type, const uint8_t *payload, size_t payload_len, sshbuf_t *out_wire) {
    if (!t || !out_wire || (!payload && payload_len > 0)) return SSH_ERR_INVALID_ARG;
    if (t->state == TRANSPORT_STATE_ERROR) return t->last_error;

    /* Padding calculation: RFC 4253 §6 (multiple of 8, at least 4 bytes) */
    size_t unpadded = 4 + 1 + 1 + payload_len;
    size_t pad = 8 - (unpadded % 8);
    if (pad < 4) pad += 8;

    uint32_t packet_len = (uint32_t)(1 + 1 + payload_len + pad);

    uint8_t plain[SSH_MAX_PACKET_LEN + 32];
    plain[0] = (uint8_t)pad;
    plain[1] = type;
    if (payload_len > 0) {
        memcpy(plain + 2, payload, payload_len);
    }
    for (size_t i = 0; i < pad; i++) {
        plain[2 + payload_len + i] = (uint8_t)(i + 1);
    }

    if (!t->out_cipher.active) {
        int r = sshbuf_put_u32(out_wire, packet_len);
        if (r) return r;
        r = sshbuf_put_raw(out_wire, plain, packet_len);
        if (r) return r;
        t->out_cipher.seq++;
        return SSH_ERR_SUCCESS;
    } else {
        uint8_t enc_length[4];
        uint8_t enc_body[SSH_MAX_PACKET_LEN + 32];
        uint8_t tag[16];

        int r = ssh_cipher_encrypt_packet(&t->out_cipher, plain, packet_len,
                                          enc_length, enc_body, tag);
        if (r != SSH_ERR_SUCCESS) return r;

        r = sshbuf_put_raw(out_wire, enc_length, 4);
        if (r) return r;
        r = sshbuf_put_raw(out_wire, enc_body, packet_len);
        if (r) return r;
        r = sshbuf_put_raw(out_wire, tag, 16);
        if (r) return r;

        return SSH_ERR_SUCCESS;
    }
}

int ssh_transport_apply_newkeys(ssh_transport_t *t, bool is_rekey) {
    if (!t) return SSH_ERR_INVALID_ARG;

    int r = kex_derive_keys(&t->kex);
    if (r != SSH_ERR_SUCCESS) return r;

    uint32_t saved_in_seq = t->in_cipher.seq;
    uint32_t saved_out_seq = t->out_cipher.seq;

    ssh_cipher_init(&t->in_cipher, t->kex.key_c2s);
    ssh_cipher_init(&t->out_cipher, t->kex.key_s2c);

    if (is_rekey) {
        /* RFC 4253 §9 & OpenSSH strict KEX: in re-exchange, sequence numbers MUST NOT be reset */
        t->in_cipher.seq = saved_in_seq;
        t->out_cipher.seq = saved_out_seq;
    } else {
        if (t->strict_kex) {
            ssh_cipher_reset_seq(&t->in_cipher);
            ssh_cipher_reset_seq(&t->out_cipher);
        }
        t->initial_kex_done = true;
    }

    t->state = TRANSPORT_STATE_ACTIVE;
    return SSH_ERR_SUCCESS;
}

int ssh_transport_start_rekey(ssh_transport_t *t, const uint8_t cookie[16], sshbuf_t *out_wire) {
    if (!t || !cookie || !out_wire) return SSH_ERR_INVALID_ARG;

    kex_reset_for_rekey(&t->kex);

    sshbuf_t kexinit_payload;
    int r = sshbuf_init(&kexinit_payload, 512, 4096);
    if (r) return r;

    r = kex_build_kexinit(&t->kex, cookie, &kexinit_payload);
    if (r) {
        sshbuf_free(&kexinit_payload);
        return r;
    }

    /* Send KEXINIT packet onto wire (encrypted if active) */
    /* kexinit_payload data has SSH_MSG_KEXINIT as first byte, payload follows */
    r = ssh_transport_send_packet(t, kexinit_payload.data[0],
                                  kexinit_payload.data + 1,
                                  kexinit_payload.size - 1,
                                  out_wire);

    sshbuf_free(&kexinit_payload);
    t->state = TRANSPORT_STATE_REKEY;
    return r;
}
