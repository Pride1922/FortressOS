/*
 * user/ssh.c - FortressOS Standalone SSH Client (/bin/ssh)
 *
 * Implements RFC 4251, RFC 4252, RFC 4253, RFC 4254, and RFC 8731:
 * - curve25519-sha256 ECDH Key Exchange with OpenSSH strict KEX
 * - ssh-ed25519 host key authentication
 * - chacha20-poly1305@openssh.com AEAD symmetric transport encryption
 * - RFC 4252 password authentication with terminal echo suppression
 * - RFC 4254 session channels with pty-req/shell or exec command execution
 * - Non-blocking I/O multiplexing via SYS_POLL (Syscall 88)
 */

#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "terminal.h"
#include "poll.h"
#include "udp_common.h"
#include "string.h"
#include "crypto.h"

#include "sshd/ssh_packet.h"
#include "sshd/sshbuf.h"
#include "sshd/cipher.h"
#include "dns.h"
#include "netconf.h"

#define SSH_CLIENT_VERSION_STRING "SSH-2.0-FortressSSH_Client_0.1"

#define DEFAULT_SSH_PORT 22
#define DEFAULT_USER     "operator"

#define CLIENT_WINDOW_SIZE    65536
#define CLIENT_MAX_PACKET     32768
#define CLIENT_WINDOW_REFILL  16384

/* Syscall wrapper for freestanding userland */
static inline long ssh_call(long num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                            uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    long ret;
    register uintptr_t r10 __asm__("r10") = a4;
    register uintptr_t r8  __asm__("r8")  = a5;
    register uintptr_t r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long ssh_call4(long num, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    return ssh_call(num, a1, a2, a3, a4, 0, 0);
}

static void ssh_write_fd(int fd, const char *msg) {
    if (!msg) return;
    size_t len = strlen(msg);
    size_t at = 0;
    while (at < len) {
        long w = ssh_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(msg + at), len - at, 0, 0, 0);
        if (w <= 0) break;
        at += (size_t)w;
    }
}

static void ssh_write_bytes(int fd, const void *data, size_t len) {
    if (!data || len == 0) return;
    size_t at = 0;
    while (at < len) {
        long w = ssh_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)((const uint8_t *)data + at), len - at, 0, 0, 0);
        if (w <= 0) break;
        at += (size_t)w;
    }
}

/* Static context to preserve Gate G1 stack discipline */
typedef struct {
    int sock_fd;
    bool interactive;
    bool strict_kex;
    bool encrypted;

    /* Channel state */
    uint32_t local_channel_id;
    uint32_t remote_channel_id;
    uint32_t local_window;
    uint32_t remote_window;
    uint32_t remote_maxpacket;

    /* Cryptographic contexts */
    ssh_cipher_ctx in_cipher;
    ssh_cipher_ctx out_cipher;

    /* Ephemeral Curve25519 & Handshake keys */
    uint8_t client_priv[32];
    uint8_t client_pub[32];
    uint8_t server_pub[32];
    uint8_t shared_secret[32];
    uint8_t hash[32];
    uint8_t session_id[32];
    uint8_t host_pubkey[32];
    uint8_t key_c2s[64];
    uint8_t key_s2c[64];

    /* Saved KEXINIT payloads for exchange hash */
    uint8_t client_kexinit[512];
    size_t client_kexinit_len;
    uint8_t server_kexinit[1024];
    size_t server_kexinit_len;

    /* Stream reception buffers */
    uint8_t rx_stream[65536];
    size_t rx_stream_len;
    size_t rx_stream_off;

    /* Incoming packet buffer */
    uint8_t packet_payload[36000];
    size_t packet_payload_len;
    uint8_t packet_type;

    /* Terminal settings */
    bool term_saved;
    terminal_attrs_t orig_term;

    /* Process exit status */
    uint32_t exit_status;
    bool exit_status_received;

    /* DNS resolution */
    dns_context_t dns_ctx;
    dns_result_t dns_res;
} ssh_client_ctx_t;

static ssh_client_ctx_t s_ctx;

/* Key derivation helper (RFC 4253 §7.2) */
static void derive_key_64(const uint8_t *k_mpint, size_t k_mpint_len,
                          const uint8_t hash[32], char tag,
                          const uint8_t session_id[32], uint8_t out_key[64]) {
    crypto_sha256_ctx ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, k_mpint, k_mpint_len);
    crypto_sha256_update(&ctx, hash, 32);
    crypto_sha256_update(&ctx, &tag, 1);
    crypto_sha256_update(&ctx, session_id, 32);
    crypto_sha256_final(&ctx, out_key);

    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, k_mpint, k_mpint_len);
    crypto_sha256_update(&ctx, hash, 32);
    crypto_sha256_update(&ctx, out_key, 32);
    crypto_sha256_final(&ctx, out_key + 32);
}

/* Send a formatted packet onto the wire */
static int client_send_packet(uint8_t type, const uint8_t *payload, size_t payload_len) {
    size_t unpadded = (s_ctx.encrypted ? 0 : 4) + 1 + 1 + payload_len;
    size_t pad = 8 - (unpadded % 8);
    if (pad < 4) pad += 8;

    uint32_t packet_len = (uint32_t)(1 + 1 + payload_len + pad);

    static uint8_t plain[36000];
    plain[0] = (uint8_t)pad;
    plain[1] = type;
    if (payload_len > 0 && payload) {
        memcpy(plain + 2, payload, payload_len);
    }
    for (size_t i = 0; i < pad; i++) {
        plain[2 + payload_len + i] = (uint8_t)(i + 1);
    }

    if (!s_ctx.encrypted) {
        uint8_t wire_hdr[4];
        wire_hdr[0] = (uint8_t)(packet_len >> 24);
        wire_hdr[1] = (uint8_t)(packet_len >> 16);
        wire_hdr[2] = (uint8_t)(packet_len >> 8);
        wire_hdr[3] = (uint8_t)packet_len;

        ssh_write_bytes(s_ctx.sock_fd, wire_hdr, 4);
        ssh_write_bytes(s_ctx.sock_fd, plain, packet_len);
        return 0;
    } else {
        uint8_t enc_length[4];
        static uint8_t enc_body[36000];
        uint8_t tag[16];

        int r = ssh_cipher_encrypt_packet(&s_ctx.out_cipher, plain, packet_len,
                                          enc_length, enc_body, tag);
        if (r != SSH_ERR_SUCCESS) return r;

        ssh_write_bytes(s_ctx.sock_fd, enc_length, 4);
        ssh_write_bytes(s_ctx.sock_fd, enc_body, packet_len);
        ssh_write_bytes(s_ctx.sock_fd, tag, 16);
        return 0;
    }
}

/* Ensure at least 'needed' bytes are in rx_stream */
static int client_fill_rx(size_t needed) {
    if (s_ctx.rx_stream_off > 0) {
        size_t rem = s_ctx.rx_stream_len - s_ctx.rx_stream_off;
        if (rem > 0) {
            memmove(s_ctx.rx_stream, s_ctx.rx_stream + s_ctx.rx_stream_off, rem);
        }
        s_ctx.rx_stream_len = rem;
        s_ctx.rx_stream_off = 0;
    }

    while (s_ctx.rx_stream_len < needed) {
        long r = ssh_call(SYS_READ, (uintptr_t)s_ctx.sock_fd,
                          (uintptr_t)(s_ctx.rx_stream + s_ctx.rx_stream_len),
                          sizeof(s_ctx.rx_stream) - s_ctx.rx_stream_len, 0, 0, 0);
        if (r <= 0) return -1;
        s_ctx.rx_stream_len += (size_t)r;
    }
    return 0;
}

/* Read and decrypt the next packet from the TCP stream */
static int client_read_packet(uint8_t *out_type, const uint8_t **out_payload, size_t *out_len) {
    if (!s_ctx.encrypted) {
        if (client_fill_rx(4) < 0) return -1;
        const uint8_t *hdr = s_ctx.rx_stream + s_ctx.rx_stream_off;
        uint32_t packet_len = ((uint32_t)hdr[0] << 24) |
                              ((uint32_t)hdr[1] << 16) |
                              ((uint32_t)hdr[2] << 8)  |
                              ((uint32_t)hdr[3]);
        if (packet_len < SSH_MIN_PACKET_LEN || packet_len > SSH_MAX_PACKET_LEN) return -1;

        if (client_fill_rx(4 + packet_len) < 0) return -1;
        const uint8_t *body = s_ctx.rx_stream + s_ctx.rx_stream_off + 4;
        uint8_t pad_len = body[0];
        if (pad_len < 4 || (2U + pad_len) > packet_len) return -1;

        *out_type = body[1];
        size_t plen = packet_len - 2 - pad_len;
        if (plen > 0) {
            memcpy(s_ctx.packet_payload, body + 2, plen);
            *out_payload = s_ctx.packet_payload;
        } else {
            *out_payload = NULL;
        }
        *out_len = plen;

        s_ctx.rx_stream_off += (4 + packet_len);
        return 0;
    } else {
        if (client_fill_rx(4) < 0) return -1;
        const uint8_t *enc_len_hdr = s_ctx.rx_stream + s_ctx.rx_stream_off;
        uint32_t packet_len = 0;
        int r = ssh_cipher_decrypt_length(&s_ctx.in_cipher, enc_len_hdr, &packet_len);
        if (r != SSH_ERR_SUCCESS) return -1;

        if (client_fill_rx(4 + packet_len + 16) < 0) return -1;
        const uint8_t *enc_body = s_ctx.rx_stream + s_ctx.rx_stream_off + 4;
        const uint8_t *tag = enc_body + packet_len;

        static uint8_t dec_body[36000];
        r = ssh_cipher_decrypt_packet(&s_ctx.in_cipher, enc_len_hdr, packet_len,
                                      enc_body, tag, dec_body);
        if (r != SSH_ERR_SUCCESS) return -1;

        uint8_t pad_len = dec_body[0];
        if (pad_len < 4 || (2U + pad_len) > packet_len) return -1;

        *out_type = dec_body[1];
        size_t plen = packet_len - 2 - pad_len;
        if (plen > 0) {
            memcpy(s_ctx.packet_payload, dec_body + 2, plen);
            *out_payload = s_ctx.packet_payload;
        } else {
            *out_payload = NULL;
        }
        *out_len = plen;

        s_ctx.rx_stream_off += (4 + packet_len + 16);
        return 0;
    }
}

/* Print usage instructions */
static void print_usage(void) {
    ssh_write_fd(2,
        "usage: ssh [options] [user@]host [command ...]\n"
        "\n"
        "Options:\n"
        "  -p <port>      Connect to port on remote host (default: 22)\n"
        "  -l <user>      Log in as user (default: operator)\n"
        "  -P <password>  Supply password (overrides interactive prompt)\n"
        "  -s <dns-ip>    Override DNS server IPv4 address for host resolution\n"
        "  -h, --help     Display this help message\n"
        "\n"
        "If a command is supplied, executes non-interactively via exec.\n"
        "Otherwise, allocates a pseudoterminal and launches an interactive shell.\n");
}

/* Prompt for password with echo suppression */
static bool get_password(char *out_pass, size_t max_len, const char *user, const char *host) {
    ssh_write_fd(2, user);
    ssh_write_fd(2, "@");
    ssh_write_fd(2, host);
    ssh_write_fd(2, "'s password: ");

    terminal_attrs_t saved, hidden;
    bool term_ok = false;
    if (ssh_call4(SYS_TERMATTR, 0, TERM_GET, (uintptr_t)&saved, sizeof(saved)) == 0) {
        hidden = saved;
        hidden.input_flags &= ~TERM_ECHO;
        if (ssh_call4(SYS_TERMATTR, 0, TERM_SET, (uintptr_t)&hidden, sizeof(hidden)) == 0) {
            term_ok = true;
        }
    }

    size_t pos = 0;
    while (pos + 1 < max_len) {
        char ch = 0;
        long r = ssh_call(SYS_READ, 0, (uintptr_t)&ch, 1, 0, 0, 0);
        if (r <= 0 || ch == '\n' || ch == '\r') break;
        out_pass[pos++] = ch;
    }
    out_pass[pos] = '\0';

    if (term_ok) {
        (void)ssh_call4(SYS_TERMATTR, 0, TERM_SET, (uintptr_t)&saved, sizeof(saved));
    }
    ssh_write_fd(2, "\n");
    return true;
}

/* Find character in string */
static const char *ssh_strchr(const char *s, char c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == c) return s;
        s++;
    }
    return NULL;
}

/* Full SSH handshake: version exchange, KEXINIT, ECDH, NEWKEYS, USERAUTH, CHANNEL_OPEN */
static int client_handshake(const char *user, const char *pass, const char *cmd) {
    sshbuf_t sb;
    sshbuf_t sb_parse;

    /* 1. Send client version banner */
    const char c_banner[] = SSH_CLIENT_VERSION_STRING "\r\n";
    ssh_write_bytes(s_ctx.sock_fd, c_banner, sizeof(c_banner) - 1);

    /* Read server version banner line */
    size_t vpos = 0;
    static char s_banner[256];
    while (vpos + 1 < sizeof(s_banner)) {
        char ch = 0;
        long r = ssh_call(SYS_READ, (uintptr_t)s_ctx.sock_fd, (uintptr_t)&ch, 1, 0, 0, 0);
        if (r <= 0) {
            ssh_write_fd(2, "ssh: connection closed while reading server banner\n");
            return -1;
        }
        if (ch == '\n') break;
        if (ch != '\r') s_banner[vpos++] = ch;
    }
    s_banner[vpos] = '\0';

    if (strncmp(s_banner, "SSH-2.0-", 8) != 0 && strncmp(s_banner, "SSH-1.99-", 9) != 0) {
        ssh_write_fd(2, "ssh: unsupported protocol version from server: ");
        ssh_write_fd(2, s_banner);
        ssh_write_fd(2, "\n");
        return -1;
    }

    /* 2. Client KEXINIT */
    uint8_t cookie[16];
    if (ssh_call(SYS_GETRANDOM, (uintptr_t)cookie, 16, 0, 0, 0, 0) != 16) {
        ssh_write_fd(2, "ssh: strong randomness unavailable\n");
        return -1;
    }

    sshbuf_init(&sb, 512, 2048);
    sshbuf_put_u8(&sb, SSH_MSG_KEXINIT);
    sshbuf_put_raw(&sb, cookie, 16);
    sshbuf_put_cstring(&sb, SSH_KEX_CURVE25519_SHA256 "," SSH_KEX_STRICT_CLIENT);
    sshbuf_put_cstring(&sb, SSH_HOSTKEY_ED25519);
    sshbuf_put_cstring(&sb, SSH_CIPHER_CHACHA20_POLY1305);
    sshbuf_put_cstring(&sb, SSH_CIPHER_CHACHA20_POLY1305);
    sshbuf_put_cstring(&sb, "");
    sshbuf_put_cstring(&sb, "");
    sshbuf_put_cstring(&sb, "none");
    sshbuf_put_cstring(&sb, "none");
    sshbuf_put_cstring(&sb, "");
    sshbuf_put_cstring(&sb, "");
    sshbuf_put_u8(&sb, 0);
    sshbuf_put_u32(&sb, 0);

    /* Save copy of client KEXINIT for exchange hash H */
    s_ctx.client_kexinit_len = sb.size;
    memcpy(s_ctx.client_kexinit, sb.data, sb.size);

    /* Send KEXINIT packet */
    client_send_packet(SSH_MSG_KEXINIT, sb.data + 1, sb.size - 1);
    sshbuf_free(&sb);

    /* 3. Receive server KEXINIT */
    uint8_t msg_type = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_KEXINIT) {
        ssh_write_fd(2, "ssh: failed to receive KEXINIT from server\n");
        return -1;
    }

    /* Save server KEXINIT payload for exchange hash H (include msg type 20) */
    s_ctx.server_kexinit[0] = SSH_MSG_KEXINIT;
    memcpy(s_ctx.server_kexinit + 1, payload, payload_len);
    s_ctx.server_kexinit_len = payload_len + 1;

    /* Parse server KEXINIT to check for strict KEX */
    sshbuf_init_from(&sb_parse, payload, payload_len);
    uint8_t s_cookie[16];
    sshbuf_get_raw(&sb_parse, s_cookie, 16);
    const uint8_t *kex_algs = NULL;
    size_t kex_len = 0;
    sshbuf_get_string_ptr(&sb_parse, &kex_algs, &kex_len);
    if (kex_algs && kex_len > 0) {
        if (strstr((const char *)kex_algs, SSH_KEX_STRICT_SERVER) != NULL) {
            s_ctx.strict_kex = true;
        }
    }

    /* 4. Generate client Curve25519 keypair and send KEX_ECDH_INIT */
    if (crypto_x25519_keygen(s_ctx.client_priv, s_ctx.client_pub) != 0) {
        ssh_write_fd(2, "ssh: X25519 keygen failed\n");
        return -1;
    }

    sshbuf_init(&sb, 64, 256);
    sshbuf_put_string(&sb, s_ctx.client_pub, 32);
    client_send_packet(SSH_MSG_KEX_ECDH_INIT, sb.data, sb.size);
    sshbuf_free(&sb);

    /* 5. Receive KEX_ECDH_REPLY */
    if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_KEX_ECDH_REPLY) {
        ssh_write_fd(2, "ssh: failed to receive KEX_ECDH_REPLY\n");
        return -1;
    }

    sshbuf_init_from(&sb_parse, payload, payload_len);

    const uint8_t *ks_blob = NULL, *server_pub = NULL, *sig_blob = NULL;
    size_t ks_len = 0, spub_len = 0, sig_blob_len = 0;

    if (sshbuf_get_string_ptr(&sb_parse, &ks_blob, &ks_len) != 0 ||
        sshbuf_get_string_ptr(&sb_parse, &server_pub, &spub_len) != 0 || spub_len != 32 ||
        sshbuf_get_string_ptr(&sb_parse, &sig_blob, &sig_blob_len) != 0) {
        ssh_write_fd(2, "ssh: invalid KEX_ECDH_REPLY formatting\n");
        return -1;
    }
    memcpy(s_ctx.server_pub, server_pub, 32);

    /* Parse host key blob */
    sshbuf_t ks_parse;
    sshbuf_init_from(&ks_parse, ks_blob, ks_len);
    const uint8_t *hk_name = NULL, *hk_bytes = NULL;
    size_t hk_name_len = 0, hk_bytes_len = 0;
    if (sshbuf_get_string_ptr(&ks_parse, &hk_name, &hk_name_len) != 0 ||
        sshbuf_get_string_ptr(&ks_parse, &hk_bytes, &hk_bytes_len) != 0 || hk_bytes_len != 32) {
        ssh_write_fd(2, "ssh: invalid host key blob in ECDH reply\n");
        return -1;
    }
    memcpy(s_ctx.host_pubkey, hk_bytes, 32);

    /* Parse signature blob */
    sshbuf_t sig_parse;
    sshbuf_init_from(&sig_parse, sig_blob, sig_blob_len);
    const uint8_t *sig_name = NULL, *sig_bytes = NULL;
    size_t sig_name_len = 0, sig_bytes_len = 0;
    if (sshbuf_get_string_ptr(&sig_parse, &sig_name, &sig_name_len) != 0 ||
        sshbuf_get_string_ptr(&sig_parse, &sig_bytes, &sig_bytes_len) != 0 || sig_bytes_len != 64) {
        ssh_write_fd(2, "ssh: invalid signature blob in ECDH reply\n");
        return -1;
    }

    /* Compute shared secret K = X25519(d_client, Q_server) */
    if (crypto_x25519(s_ctx.shared_secret, s_ctx.client_priv, s_ctx.server_pub) != 0) {
        ssh_write_fd(2, "ssh: X25519 shared secret computation failed\n");
        return -1;
    }

    /* Compute exchange hash H = SHA256(Vc || Vs || Ic || Is || Ks || Qc || Qs || K) */
    sshbuf_init(&sb, 4096, 65536);
    sshbuf_put_cstring(&sb, SSH_CLIENT_VERSION_STRING);
    sshbuf_put_cstring(&sb, s_banner);
    sshbuf_put_string(&sb, s_ctx.client_kexinit, s_ctx.client_kexinit_len);
    sshbuf_put_string(&sb, s_ctx.server_kexinit, s_ctx.server_kexinit_len);
    sshbuf_put_string(&sb, ks_blob, ks_len);
    sshbuf_put_string(&sb, s_ctx.client_pub, 32);
    sshbuf_put_string(&sb, s_ctx.server_pub, 32);
    sshbuf_put_mpint(&sb, s_ctx.shared_secret, 32);

    crypto_sha256(sb.data, sb.size, s_ctx.hash);
    memcpy(s_ctx.session_id, s_ctx.hash, 32);
    sshbuf_free(&sb);

    /* Verify Ed25519 signature on exchange hash H */
    if (crypto_ed25519_verify(sig_bytes, s_ctx.host_pubkey, s_ctx.hash, 32) != 0) {
        ssh_write_fd(2, "ssh: host key signature verification failed!\n");
        return -1;
    }

    /* Derive encryption keys */
    sshbuf_init(&sb, 64, 128);
    sshbuf_put_mpint(&sb, s_ctx.shared_secret, 32);

    derive_key_64(sb.data, sb.size, s_ctx.hash, 'C', s_ctx.session_id, s_ctx.key_c2s);
    derive_key_64(sb.data, sb.size, s_ctx.hash, 'D', s_ctx.session_id, s_ctx.key_s2c);
    sshbuf_free(&sb);

    /* 6. Send and Receive SSH_MSG_NEWKEYS */
    client_send_packet(SSH_MSG_NEWKEYS, NULL, 0);

    if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_NEWKEYS) {
        ssh_write_fd(2, "ssh: failed to receive SSH_MSG_NEWKEYS\n");
        return -1;
    }

    /* Activate ciphers */
    ssh_cipher_init(&s_ctx.out_cipher, s_ctx.key_c2s); /* Client sends on c2s */
    ssh_cipher_init(&s_ctx.in_cipher, s_ctx.key_s2c);  /* Client receives on s2c */
    s_ctx.encrypted = true;

    /* 7. Request ssh-userauth service */
    sshbuf_init(&sb, 64, 128);
    sshbuf_put_cstring(&sb, "ssh-userauth");
    client_send_packet(SSH_MSG_SERVICE_REQUEST, sb.data, sb.size);
    sshbuf_free(&sb);

    if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_SERVICE_ACCEPT) {
        ssh_write_fd(2, "ssh: ssh-userauth service rejected by server\n");
        return -1;
    }

    /* 8. Send password authentication request */
    sshbuf_init(&sb, 256, 1024);
    sshbuf_put_cstring(&sb, user);
    sshbuf_put_cstring(&sb, "ssh-connection");
    sshbuf_put_cstring(&sb, "password");
    sshbuf_put_u8(&sb, 0); /* false: don't change password */
    sshbuf_put_cstring(&sb, pass);
    client_send_packet(SSH_MSG_USERAUTH_REQUEST, sb.data, sb.size);
    sshbuf_free(&sb);

    /* Await authentication response, handling any banners */
    while (1) {
        if (client_read_packet(&msg_type, &payload, &payload_len) < 0) {
            ssh_write_fd(2, "ssh: connection lost during user authentication\n");
            return -1;
        }
        if (msg_type == SSH_MSG_USERAUTH_BANNER) {
            sshbuf_init_from(&sb_parse, payload, payload_len);
            const uint8_t *b_msg = NULL;
            size_t b_len = 0;
            if (sshbuf_get_string_ptr(&sb_parse, &b_msg, &b_len) == 0 && b_msg) {
                ssh_write_bytes(2, b_msg, b_len);
            }
            continue;
        } else if (msg_type == SSH_MSG_USERAUTH_SUCCESS) {
            break;
        } else if (msg_type == SSH_MSG_USERAUTH_FAILURE) {
            ssh_write_fd(2, "ssh: Permission denied (password).\n");
            return -1;
        } else {
            ssh_write_fd(2, "ssh: unexpected authentication response\n");
            return -1;
        }
    }

    /* 9. Open session channel */
    s_ctx.local_channel_id = 0;
    s_ctx.local_window = CLIENT_WINDOW_SIZE;

    sshbuf_init(&sb, 128, 256);
    sshbuf_put_cstring(&sb, "session");
    sshbuf_put_u32(&sb, s_ctx.local_channel_id);
    sshbuf_put_u32(&sb, CLIENT_WINDOW_SIZE);
    sshbuf_put_u32(&sb, CLIENT_MAX_PACKET);
    client_send_packet(SSH_MSG_CHANNEL_OPEN, sb.data, sb.size);
    sshbuf_free(&sb);

    if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
        ssh_write_fd(2, "ssh: channel open failed\n");
        return -1;
    }

    sshbuf_init_from(&sb_parse, payload, payload_len);
    uint32_t rec_id = 0;
    sshbuf_get_u32(&sb_parse, &rec_id);
    sshbuf_get_u32(&sb_parse, &s_ctx.remote_channel_id);
    sshbuf_get_u32(&sb_parse, &s_ctx.remote_window);
    sshbuf_get_u32(&sb_parse, &s_ctx.remote_maxpacket);

    /* 10. Channel request: PTY + shell OR exec */
    if (cmd && cmd[0] != '\0') {
        /* Non-interactive exec */
        sshbuf_init(&sb, 128, 1024);
        sshbuf_put_u32(&sb, s_ctx.remote_channel_id);
        sshbuf_put_cstring(&sb, "exec");
        sshbuf_put_u8(&sb, 1); /* want reply */
        sshbuf_put_cstring(&sb, cmd);
        client_send_packet(SSH_MSG_CHANNEL_REQUEST, sb.data, sb.size);
        sshbuf_free(&sb);

        if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_CHANNEL_SUCCESS) {
            ssh_write_fd(2, "ssh: exec request failed\n");
            return -1;
        }
    } else {
        /* Interactive PTY and shell */
        s_ctx.interactive = true;

        /* Save terminal state and put terminal in raw mode for transparent interactive session */
        if (ssh_call4(SYS_TERMATTR, 0, TERM_GET, (uintptr_t)&s_ctx.orig_term, sizeof(s_ctx.orig_term)) == 0) {
            s_ctx.term_saved = true;
            terminal_attrs_t raw = s_ctx.orig_term;
            raw.input_flags &= ~(TERM_ECHO | TERM_ICANON);
            (void)ssh_call4(SYS_TERMATTR, 0, TERM_SET, (uintptr_t)&raw, sizeof(raw));
        }

        /* Send pty-req */
        sshbuf_init(&sb, 128, 256);
        sshbuf_put_u32(&sb, s_ctx.remote_channel_id);
        sshbuf_put_cstring(&sb, "pty-req");
        sshbuf_put_u8(&sb, 1); /* want reply */
        sshbuf_put_cstring(&sb, "xterm");
        sshbuf_put_u32(&sb, 80); /* cols */
        sshbuf_put_u32(&sb, 25); /* rows */
        sshbuf_put_u32(&sb, 0);  /* width_px */
        sshbuf_put_u32(&sb, 0);  /* height_px */
        sshbuf_put_cstring(&sb, ""); /* modes */
        client_send_packet(SSH_MSG_CHANNEL_REQUEST, sb.data, sb.size);
        sshbuf_free(&sb);

        if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_CHANNEL_SUCCESS) {
            ssh_write_fd(2, "ssh: pty-req request failed\n");
            return -1;
        }

        /* Send shell request */
        sshbuf_init(&sb, 64, 128);
        sshbuf_put_u32(&sb, s_ctx.remote_channel_id);
        sshbuf_put_cstring(&sb, "shell");
        sshbuf_put_u8(&sb, 1); /* want reply */
        client_send_packet(SSH_MSG_CHANNEL_REQUEST, sb.data, sb.size);
        sshbuf_free(&sb);

        if (client_read_packet(&msg_type, &payload, &payload_len) < 0 || msg_type != SSH_MSG_CHANNEL_SUCCESS) {
            ssh_write_fd(2, "ssh: shell request failed\n");
            return -1;
        }
    }

    return 0;
}

static bool client_has_buffered_rx(void) {
    return s_ctx.rx_stream_len > s_ctx.rx_stream_off;
}

/* Run the bidirectional event loop using SYS_POLL */
static int client_event_loop(void) {
    struct pollfd pfds[2];
    pfds[0].fd = 0;               /* stdin */
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    pfds[1].fd = s_ctx.sock_fd;   /* socket */
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    bool stdin_eof = false;

    while (1) {
        long pr = 0;
        if (!client_has_buffered_rx()) {
            pr = ssh_call(SYS_POLL, (uintptr_t)pfds, 2, 5000, 0, 0, 0);
            if (pr < 0) {
                if (pr == -4) continue; /* EINTR */
                break;
            }
        }

        /* 1. Remote socket readable or internal buffer has pending packets */
        if (client_has_buffered_rx() || (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t type = 0;
            const uint8_t *payload = NULL;
            size_t payload_len = 0;

            int rr = client_read_packet(&type, &payload, &payload_len);
            if (rr < 0) {
                /* Connection terminated */
                break;
            }

            if (type == SSH_MSG_CHANNEL_DATA) {
                sshbuf_t d_buf;
                sshbuf_init_from(&d_buf, payload, payload_len);
                uint32_t ch_id = 0;
                const uint8_t *data = NULL;
                size_t d_len = 0;
                if (sshbuf_get_u32(&d_buf, &ch_id) == 0 &&
                    sshbuf_get_string_ptr(&d_buf, &data, &d_len) == 0 && data) {
                    ssh_write_bytes(1, data, d_len);
                    if (s_ctx.local_window >= d_len) {
                        s_ctx.local_window -= (uint32_t)d_len;
                    } else {
                        s_ctx.local_window = 0;
                    }
                    /* Replenish window if below threshold */
                    if (s_ctx.local_window < CLIENT_WINDOW_REFILL) {
                        uint32_t add = CLIENT_WINDOW_SIZE - s_ctx.local_window;
                        sshbuf_t adj;
                        sshbuf_init(&adj, 64, 128);
                        sshbuf_put_u32(&adj, s_ctx.remote_channel_id);
                        sshbuf_put_u32(&adj, add);
                        client_send_packet(SSH_MSG_CHANNEL_WINDOW_ADJUST, adj.data, adj.size);
                        sshbuf_free(&adj);
                        s_ctx.local_window += add;
                    }
                }
            } else if (type == SSH_MSG_CHANNEL_EXTENDED_DATA) {
                sshbuf_t ed_buf;
                sshbuf_init_from(&ed_buf, payload, payload_len);
                uint32_t ch_id = 0, data_type = 0;
                const uint8_t *data = NULL;
                size_t d_len = 0;
                if (sshbuf_get_u32(&ed_buf, &ch_id) == 0 &&
                    sshbuf_get_u32(&ed_buf, &data_type) == 0 &&
                    sshbuf_get_string_ptr(&ed_buf, &data, &d_len) == 0 && data) {
                    ssh_write_bytes(2, data, d_len);
                    if (s_ctx.local_window >= d_len) {
                        s_ctx.local_window -= (uint32_t)d_len;
                    } else {
                        s_ctx.local_window = 0;
                    }
                    if (s_ctx.local_window < CLIENT_WINDOW_REFILL) {
                        uint32_t add = CLIENT_WINDOW_SIZE - s_ctx.local_window;
                        sshbuf_t adj;
                        sshbuf_init(&adj, 64, 128);
                        sshbuf_put_u32(&adj, s_ctx.remote_channel_id);
                        sshbuf_put_u32(&adj, add);
                        client_send_packet(SSH_MSG_CHANNEL_WINDOW_ADJUST, adj.data, adj.size);
                        sshbuf_free(&adj);
                        s_ctx.local_window += add;
                    }
                }
            } else if (type == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                sshbuf_t w_buf;
                sshbuf_init_from(&w_buf, payload, payload_len);
                uint32_t ch_id = 0, add_bytes = 0;
                if (sshbuf_get_u32(&w_buf, &ch_id) == 0 &&
                    sshbuf_get_u32(&w_buf, &add_bytes) == 0) {
                    s_ctx.remote_window += add_bytes;
                }
            } else if (type == SSH_MSG_CHANNEL_REQUEST) {
                sshbuf_t r_buf;
                sshbuf_init_from(&r_buf, payload, payload_len);
                uint32_t ch_id = 0;
                const uint8_t *req_type = NULL;
                size_t req_len = 0;
                uint8_t want_rep = 0;
                if (sshbuf_get_u32(&r_buf, &ch_id) == 0 &&
                    sshbuf_get_string_ptr(&r_buf, &req_type, &req_len) == 0 &&
                    sshbuf_get_u8(&r_buf, &want_rep) == 0) {
                    if (req_len == 11 && memcmp(req_type, "exit-status", 11) == 0) {
                        uint32_t status = 0;
                        if (sshbuf_get_u32(&r_buf, &status) == 0) {
                            s_ctx.exit_status = status;
                            s_ctx.exit_status_received = true;
                        }
                    }
                    if (want_rep) {
                        sshbuf_t rep;
                        sshbuf_init(&rep, 32, 64);
                        sshbuf_put_u32(&rep, s_ctx.remote_channel_id);
                        client_send_packet(SSH_MSG_CHANNEL_SUCCESS, rep.data, rep.size);
                        sshbuf_free(&rep);
                    }
                }
            } else if (type == SSH_MSG_CHANNEL_EOF) {
                /* Remote EOF received */
            } else if (type == SSH_MSG_CHANNEL_CLOSE) {
                /* Remote channel closed */
                sshbuf_t cls;
                sshbuf_init(&cls, 32, 64);
                sshbuf_put_u32(&cls, s_ctx.remote_channel_id);
                client_send_packet(SSH_MSG_CHANNEL_CLOSE, cls.data, cls.size);
                sshbuf_free(&cls);
                break;
            } else if (type == SSH_MSG_DISCONNECT) {
                sshbuf_t disc;
                sshbuf_init_from(&disc, payload, payload_len);
                uint32_t reason = 0;
                const uint8_t *desc = NULL;
                size_t dlen = 0;
                sshbuf_get_u32(&disc, &reason);
                sshbuf_get_string_ptr(&disc, &desc, &dlen);
                ssh_write_fd(2, "ssh: disconnected by remote host");
                if (desc && dlen > 0) {
                    ssh_write_fd(2, ": ");
                    ssh_write_bytes(2, desc, dlen);
                }
                ssh_write_fd(2, "\n");
                break;
            }
        }

        /* 2. Stdin readable */
        if (!stdin_eof && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            static uint8_t in_chunk[4096];
            size_t max_read = sizeof(in_chunk);
            if (s_ctx.remote_window > 0 && s_ctx.remote_window < max_read) {
                max_read = s_ctx.remote_window;
            }
            if (max_read > 0) {
                long r = ssh_call(SYS_READ, 0, (uintptr_t)in_chunk, max_read, 0, 0, 0);
                if (r <= 0) {
                    stdin_eof = true;
                    pfds[0].fd = -1;
                    /* Send channel EOF */
                    sshbuf_t eof_buf;
                    sshbuf_init(&eof_buf, 32, 64);
                    sshbuf_put_u32(&eof_buf, s_ctx.remote_channel_id);
                    client_send_packet(SSH_MSG_CHANNEL_EOF, eof_buf.data, eof_buf.size);
                    sshbuf_free(&eof_buf);
                } else {
                    sshbuf_t data_pkt;
                    sshbuf_init(&data_pkt, 64 + (size_t)r, 128 + (size_t)r);
                    sshbuf_put_u32(&data_pkt, s_ctx.remote_channel_id);
                    sshbuf_put_string(&data_pkt, in_chunk, (size_t)r);
                    client_send_packet(SSH_MSG_CHANNEL_DATA, data_pkt.data, data_pkt.size);
                    sshbuf_free(&data_pkt);

                    if (s_ctx.remote_window >= (size_t)r) {
                        s_ctx.remote_window -= (uint32_t)r;
                    } else {
                        s_ctx.remote_window = 0;
                    }
                }
            }
        }
    }

    return (int)s_ctx.exit_status;
}

int ssh_main(int argc, char **argv, const char *const *envp) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.sock_fd = -1;

    const char *target_host = NULL;
    const char *user = NULL;
    const char *port_str = NULL;
    const char *pass = NULL;
    uint32_t dns_server = 0;
    bool has_dns_server = false;
    uint16_t port = DEFAULT_SSH_PORT;

    static char cmd_buf[2048];
    cmd_buf[0] = '\0';
    size_t cmd_len = 0;

    /* Parse CLI options */
    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (udp_equal(arg, "-h") || udp_equal(arg, "--help")) {
            print_usage();
            return 0;
        } else if (udp_equal(arg, "-p")) {
            if (i + 1 >= argc) {
                ssh_write_fd(2, "ssh: option -p requires a port argument\n");
                return 1;
            }
            port_str = argv[++i];
        } else if (udp_equal(arg, "-l")) {
            if (i + 1 >= argc) {
                ssh_write_fd(2, "ssh: option -l requires a username argument\n");
                return 1;
            }
            user = argv[++i];
        } else if (udp_equal(arg, "-P")) {
            if (i + 1 >= argc) {
                ssh_write_fd(2, "ssh: option -P requires a password argument\n");
                return 1;
            }
            pass = argv[++i];
        } else if (udp_equal(arg, "-s")) {
            if (i + 1 >= argc) {
                ssh_write_fd(2, "ssh: option -s requires an IPv4 address\n");
                return 1;
            }
            if (!udp_ip(argv[++i], &dns_server)) {
                ssh_write_fd(2, "ssh: invalid DNS server IPv4 address\n");
                return 1;
            }
            has_dns_server = true;
        } else if (arg[0] == '-') {
            ssh_write_fd(2, "ssh: unknown option '");
            ssh_write_fd(2, arg);
            ssh_write_fd(2, "'\n");
            print_usage();
            return 1;
        } else {
            if (!target_host) {
                target_host = arg;
            } else {
                /* Append to command string */
                size_t alen = strlen(arg);
                if (cmd_len > 0 && cmd_len + 1 < sizeof(cmd_buf)) {
                    cmd_buf[cmd_len++] = ' ';
                }
                if (cmd_len + alen < sizeof(cmd_buf)) {
                    memcpy(cmd_buf + cmd_len, arg, alen);
                    cmd_len += alen;
                    cmd_buf[cmd_len] = '\0';
                }
            }
        }
        i++;
    }

    if (!target_host) {
        print_usage();
        return 1;
    }

    /* Check for user@host syntax */
    static char host_part[128];
    static char user_part[64];
    const char *at = ssh_strchr(target_host, '@');
    if (at) {
        size_t ulen = (size_t)(at - target_host);
        if (ulen > 0 && ulen < sizeof(user_part)) {
            memcpy(user_part, target_host, ulen);
            user_part[ulen] = '\0';
            if (!user) user = user_part;
        }
        size_t hlen = strlen(at + 1);
        if (hlen > 0 && hlen < sizeof(host_part)) {
            memcpy(host_part, at + 1, hlen);
            host_part[hlen] = '\0';
            target_host = host_part;
        }
    }

    if (!user) user = DEFAULT_USER;

    if (port_str) {
        unsigned pval = 0;
        const char *p = port_str;
        if (!udp_number(&p, 65535, &pval) || *p != '\0' || pval == 0) {
            ssh_write_fd(2, "ssh: invalid port number '");
            ssh_write_fd(2, port_str);
            ssh_write_fd(2, "'\n");
            return 1;
        }
        port = (uint16_t)pval;
    }

    /* Check SSH_PASSWORD environment variable if -P not given */
    if (!pass && envp) {
        for (int e = 0; envp[e]; e++) {
            if (strncmp(envp[e], "SSH_PASSWORD=", 13) == 0) {
                pass = envp[e] + 13;
                break;
            }
        }
    }

    /* Resolve host */
    uint32_t target_ip = 0;
    if (!udp_ip(target_host, &target_ip)) {
        size_t hlen = strlen(target_host);
        if (hlen > 254) {
            ssh_write_fd(2, "ssh: hostname too long\n");
            return 1;
        }

        dns_options_t dopt = {
            .server_ipv4 = has_dns_server ? dns_server : 0
        };
        dns_context_init(&s_ctx.dns_ctx);

        int status = dns_resolve_ipv4(&s_ctx.dns_ctx, &dopt, target_host, hlen, &s_ctx.dns_res);
        if (status != 0) {
            ssh_write_fd(2, "ssh: cannot resolve '");
            ssh_write_fd(2, target_host);
            ssh_write_fd(2, "': ");
            ssh_write_fd(2, dns_status_name(status));
            ssh_write_fd(2, "\n");
            return 1;
        }
        target_ip = s_ctx.dns_res.addresses[0];
    }

    /* Create TCP socket */
    long sock = ssh_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_STREAM | NET_SOCK_CLOEXEC, 6, 0, 0, 0);
    if (sock < 0) {
        ssh_write_fd(2, "ssh: failed to create TCP socket\n");
        return 1;
    }
    s_ctx.sock_fd = (int)sock;

    net_sockaddr_in_t addr = {
        .family = NET_AF_INET,
        .port = __builtin_bswap16(port),
        .address = target_ip
    };

    long conn = ssh_call(SYS_CONNECT, (uintptr_t)sock, (uintptr_t)&addr, sizeof(addr), 0, 0, 0);
    if (conn < 0) {
        ssh_write_fd(2, "ssh: connect to host ");
        ssh_write_fd(2, target_host);
        ssh_write_fd(2, " port ");
        if (port_str) ssh_write_fd(2, port_str); else ssh_write_fd(2, "22");
        ssh_write_fd(2, ": Connection refused\n");
        ssh_call(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
        return 1;
    }

    /* Interactive password prompt if not provided */
    static char entered_pass[128];
    if (!pass) {
        if (!get_password(entered_pass, sizeof(entered_pass), user, target_host)) {
            ssh_call(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
            return 1;
        }
        pass = entered_pass;
    }

    /* Perform SSH protocol handshake */
    int ret = client_handshake(user, pass, cmd_len > 0 ? cmd_buf : NULL);
    if (ret != 0) {
        ssh_call(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
        return 1;
    }

    /* Run multiplexing event loop */
    int exit_code = client_event_loop();

    /* Restore terminal settings if altered */
    if (s_ctx.term_saved) {
        (void)ssh_call4(SYS_TERMATTR, 0, TERM_SET, (uintptr_t)&s_ctx.orig_term, sizeof(s_ctx.orig_term));
    }

    /* Wipe cryptographic context and credentials */
    crypto_wipe(&s_ctx, sizeof(s_ctx));
    crypto_wipe(entered_pass, sizeof(entered_pass));

    ssh_call(SYS_CLOSE, (uintptr_t)sock, 0, 0, 0, 0, 0);
    return exit_code;
}
