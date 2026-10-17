#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "user/sshd/sshbuf.h"
#include "user/sshd/ssh_packet.h"
#include "user/sshd/cipher.h"
#include "user/sshd/kex.h"
#include "crypto.h"

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL at %s:%d: %lld != %lld\n", __FILE__, __LINE__, (long long)(a), (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_MEMEQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { \
        fprintf(stderr, "FAIL at %s:%d: memory mismatch (%zu bytes)\n", __FILE__, __LINE__, (size_t)(n)); \
        exit(1); \
    } \
} while (0)

static uint64_t g_test_rng = 123456789ULL;
int64_t crypto_test_getrandom(void *p, size_t n, uint64_t flags) {
    (void)flags;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        g_test_rng = g_test_rng * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i] = (uint8_t)(g_test_rng >> 33);
    }
    return (int64_t)n;
}

void crypto_test_wiped(const void *p, size_t n) {
    const uint8_t *v = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        assert(v[i] == 0);
    }
}

static void test_sshbuf_primitives(void) {
    sshbuf_t b;
    ASSERT_EQ(sshbuf_init(&b, 16, 256), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_len(&b), 0);

    /* Test basic puts */
    ASSERT_EQ(sshbuf_put_u8(&b, 0x42), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u32(&b, 0x12345678), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u64(&b, 0x0102030405060708ULL), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&b, "hello"), SSH_ERR_SUCCESS);

    /* Test gets */
    uint8_t u8_val = 0;
    uint32_t u32_val = 0;
    uint64_t u64_val = 0;
    const uint8_t *str_ptr = NULL;
    size_t str_len = 0;

    ASSERT_EQ(sshbuf_get_u8(&b, &u8_val), SSH_ERR_SUCCESS);
    ASSERT_EQ(u8_val, 0x42);

    ASSERT_EQ(sshbuf_get_u32(&b, &u32_val), SSH_ERR_SUCCESS);
    ASSERT_EQ(u32_val, 0x12345678);

    ASSERT_EQ(sshbuf_get_u64(&b, &u64_val), SSH_ERR_SUCCESS);
    ASSERT_EQ(u64_val, 0x0102030405060708ULL);

    ASSERT_EQ(sshbuf_get_string_ptr(&b, &str_ptr, &str_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(str_len, 5);
    ASSERT_MEMEQ(str_ptr, "hello", 5);

    ASSERT_EQ(sshbuf_len(&b), 0);
    sshbuf_free(&b);

    /* Test max size limit */
    ASSERT_EQ(sshbuf_init(&b, 8, 16), SSH_ERR_SUCCESS);
    uint8_t dummy[10] = {0};
    ASSERT_EQ(sshbuf_put_raw(&b, dummy, 10), SSH_ERR_SUCCESS);
    /* Exceeds 16 bytes max */
    ASSERT_EQ(sshbuf_put_raw(&b, dummy, 10), SSH_ERR_BUFFER_TOO_SMALL);
    sshbuf_free(&b);

    /* Test fixed buffer */
    uint8_t fixed_mem[32];
    ASSERT_EQ(sshbuf_init_fixed(&b, fixed_mem, sizeof(fixed_mem)), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u32(&b, 0xdeadbeef), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_get_u32(&b, &u32_val), SSH_ERR_SUCCESS);
    ASSERT_EQ(u32_val, 0xdeadbeef);
    sshbuf_free(&b);

    printf("PASS: test_sshbuf_primitives\n");
}

static void test_sshbuf_mpint(void) {
    sshbuf_t b;
    ASSERT_EQ(sshbuf_init(&b, 64, 256), SSH_ERR_SUCCESS);

    /* Case 1: zero */
    uint8_t zero = 0;
    ASSERT_EQ(sshbuf_put_mpint(&b, &zero, 0), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_len(&b), 4);
    uint32_t len = 0;
    ASSERT_EQ(sshbuf_get_u32(&b, &len), SSH_ERR_SUCCESS);
    ASSERT_EQ(len, 0);

    /* Case 2: positive with MSB set (needs leading zero 0x00) */
    uint8_t num1[2] = {0x80, 0x01};
    ASSERT_EQ(sshbuf_put_mpint(&b, num1, 2), SSH_ERR_SUCCESS);
    uint8_t out[16] = {0};
    size_t out_len = 0;
    ASSERT_EQ(sshbuf_get_mpint(&b, out, sizeof(out), &out_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(out_len, 2);
    ASSERT_EQ(out[0], 0x80);
    ASSERT_EQ(out[1], 0x01);

    /* Case 3: positive with MSB clear (no leading zero) */
    uint8_t num2[3] = {0x00, 0x12, 0x34}; /* has leading zero byte that should be stripped by put_mpint */
    ASSERT_EQ(sshbuf_put_mpint(&b, num2, 3), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_get_mpint(&b, out, sizeof(out), &out_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(out_len, 2);
    ASSERT_EQ(out[0], 0x12);
    ASSERT_EQ(out[1], 0x34);

    /* Case 4: invalid format (unneeded leading zero) */
    sshbuf_reset(&b);
    ASSERT_EQ(sshbuf_put_u32(&b, 2), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&b, 0x00), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&b, 0x7f), SSH_ERR_SUCCESS); /* MSB of 0x7f is 0, so 0x00 is illegal! */
    ASSERT_EQ(sshbuf_get_mpint(&b, out, sizeof(out), &out_len), SSH_ERR_INVALID_FORMAT);

    /* Case 5: negative mpint rejection */
    sshbuf_reset(&b);
    ASSERT_EQ(sshbuf_put_u32(&b, 1), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&b, 0x80), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_get_mpint(&b, out, sizeof(out), &out_len), SSH_ERR_INVALID_FORMAT);

    sshbuf_free(&b);
    printf("PASS: test_sshbuf_mpint\n");
}

static void test_chacha20_poly1305_worked_example(void) {
    /* Test vector from Appendix A of draft-ietf-sshm-chacha20-poly1305 */
    static const uint8_t key[64] = {
        0x8b, 0xbf, 0xf6, 0x85, 0x5f, 0xc1, 0x02, 0x33,
        0x8c, 0x37, 0x3e, 0x73, 0xaa, 0xc0, 0xc9, 0x14,
        0xf0, 0x76, 0xa9, 0x05, 0xb2, 0x44, 0x4a, 0x32,
        0xee, 0xca, 0xff, 0xea, 0xe2, 0x2b, 0xec, 0xc5,
        0xe9, 0xb7, 0xa7, 0xa5, 0x82, 0x5a, 0x82, 0x49,
        0x34, 0x6e, 0xc1, 0xc2, 0x83, 0x01, 0xcf, 0x39,
        0x45, 0x43, 0xfc, 0x75, 0x69, 0x88, 0x7d, 0x76,
        0xe1, 0x68, 0xf3, 0x75, 0x62, 0xac, 0x07, 0x40
    };

    /* Unencrypted payload: padding_length (1) + payload (65) + padding (6) = 72 bytes */
    static const uint8_t plain_payload[72] = {
        0x06, 0x5e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x38, 0x4c, 0x6f, 0x72, 0x65, 0x6d, 0x20,
        0x69, 0x70, 0x73, 0x75, 0x6d, 0x20, 0x64, 0x6f,
        0x6c, 0x6f, 0x72, 0x20, 0x73, 0x69, 0x74, 0x20,
        0x61, 0x6d, 0x65, 0x74, 0x2c, 0x20, 0x63, 0x6f,
        0x6e, 0x73, 0x65, 0x63, 0x74, 0x65, 0x74, 0x75,
        0x72, 0x20, 0x61, 0x64, 0x69, 0x70, 0x69, 0x73,
        0x69, 0x63, 0x69, 0x6e, 0x67, 0x20, 0x65, 0x6c,
        0x69, 0x74, 0x4e, 0x43, 0xe8, 0x04, 0xdc, 0x6c
    };

    /* Expected encrypted length (4 bytes) */
    static const uint8_t expected_enc_length[4] = {
        0x2c, 0x3e, 0xcc, 0xe4
    };

    /* Expected encrypted payload (72 bytes) */
    static const uint8_t expected_enc_payload[72] = {
        0xa5, 0xbc, 0x05, 0x89, 0x5b, 0xf0, 0x7a, 0x7b,
        0xa9, 0x56, 0xb6, 0xc6, 0x88, 0x29, 0xac, 0x7c,
        0x83, 0xb7, 0x80, 0xb7, 0x00, 0x0e, 0xcd, 0xe7,
        0x45, 0xaf, 0xc7, 0x05, 0xbb, 0xc3, 0x78, 0xce,
        0x03, 0xa2, 0x80, 0x23, 0x6b, 0x87, 0xb5, 0x3b,
        0xed, 0x58, 0x39, 0x66, 0x23, 0x02, 0xb1, 0x64,
        0xb6, 0x28, 0x6a, 0x48, 0xcd, 0x1e, 0x09, 0x71,
        0x38, 0xe3, 0xcb, 0x90, 0x9b, 0x8b, 0x2b, 0x82,
        0x9d, 0xd1, 0x8d, 0x2a, 0x35, 0xff, 0x82, 0xd9
    };

    /* Expected Poly1305 MAC tag (16 bytes) */
    static const uint8_t expected_tag[16] = {
        0x95, 0x34, 0x9e, 0x85, 0x5b, 0xf0, 0x2c, 0x29,
        0x8e, 0xf7, 0x75, 0xf2, 0xd1, 0xa7, 0xe8, 0xb8
    };

    ssh_cipher_ctx ctx;
    ssh_cipher_init(&ctx, key);
    ctx.seq = 7; /* Packet sequence number in worked example is 7 */

    uint8_t enc_length[4];
    uint8_t enc_payload[72];
    uint8_t tag[16];

    int r = ssh_cipher_encrypt_packet(&ctx, plain_payload, sizeof(plain_payload),
                                      enc_length, enc_payload, tag);
    ASSERT_EQ(r, SSH_ERR_SUCCESS);
    ASSERT_MEMEQ(enc_length, expected_enc_length, 4);
    ASSERT_MEMEQ(enc_payload, expected_enc_payload, 72);
    ASSERT_MEMEQ(tag, expected_tag, 16);
    ASSERT_EQ(ctx.seq, 8);

    /* Now decrypt using a receiver context initialized with sequence number 7 */
    ssh_cipher_ctx recv_ctx;
    ssh_cipher_init(&recv_ctx, key);
    recv_ctx.seq = 7;

    uint32_t dec_len = 0;
    r = ssh_cipher_decrypt_length(&recv_ctx, enc_length, &dec_len);
    ASSERT_EQ(r, SSH_ERR_SUCCESS);
    ASSERT_EQ(dec_len, 72);

    uint8_t dec_payload[72];
    r = ssh_cipher_decrypt_packet(&recv_ctx, enc_length, dec_len, enc_payload, tag, dec_payload);
    ASSERT_EQ(r, SSH_ERR_SUCCESS);
    ASSERT_MEMEQ(dec_payload, plain_payload, 72);
    ASSERT_EQ(recv_ctx.seq, 8);

    /* Test tampering rejection: flip 1 bit in tag */
    recv_ctx.seq = 7;
    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, 16);
    bad_tag[0] ^= 0x01;
    r = ssh_cipher_decrypt_packet(&recv_ctx, enc_length, dec_len, enc_payload, bad_tag, dec_payload);
    ASSERT_EQ(r, SSH_ERR_INVALID_FORMAT);

    /* Test tampering rejection: flip 1 bit in ciphertext payload */
    recv_ctx.seq = 7;
    uint8_t bad_payload[72];
    memcpy(bad_payload, enc_payload, 72);
    bad_payload[10] ^= 0x01;
    r = ssh_cipher_decrypt_packet(&recv_ctx, enc_length, dec_len, bad_payload, tag, dec_payload);
    ASSERT_EQ(r, SSH_ERR_INVALID_FORMAT);

    /* Test tampering rejection: flip 1 bit in encrypted length */
    recv_ctx.seq = 7;
    uint8_t bad_enc_len[4];
    memcpy(bad_enc_len, enc_length, 4);
    bad_enc_len[0] ^= 0x01;
    r = ssh_cipher_decrypt_packet(&recv_ctx, bad_enc_len, dec_len, enc_payload, tag, dec_payload);
    ASSERT_EQ(r, SSH_ERR_INVALID_FORMAT);

    ssh_cipher_cleanup(&ctx);
    ssh_cipher_cleanup(&recv_ctx);
    printf("PASS: test_chacha20_poly1305_worked_example\n");
}

static void test_kex_handshake_simulation(void) {
    ssh_kex_ctx server_kex;
    kex_init(&server_kex);

    /* Generate server host identity */
    uint8_t host_pubkey[32], host_seckey[64];
    ASSERT_EQ(crypto_ed25519_keygen(host_pubkey, host_seckey), 0);

    /* 1. Server constructs KEXINIT */
    uint8_t s_cookie[16];
    for (int i = 0; i < 16; i++) s_cookie[i] = (uint8_t)(i + 1);
    sshbuf_t s_kexinit_buf;
    ASSERT_EQ(sshbuf_init(&s_kexinit_buf, 512, 4096), SSH_ERR_SUCCESS);
    ASSERT_EQ(kex_build_kexinit(&server_kex, s_cookie, &s_kexinit_buf), SSH_ERR_SUCCESS);

    /* 2. Client constructs KEXINIT */
    sshbuf_t c_kexinit_buf;
    ASSERT_EQ(sshbuf_init(&c_kexinit_buf, 512, 4096), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&c_kexinit_buf, SSH_MSG_KEXINIT), SSH_ERR_SUCCESS);
    uint8_t c_cookie[16] = {0xaa};
    ASSERT_EQ(sshbuf_put_raw(&c_kexinit_buf, c_cookie, 16), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "curve25519-sha256,kex-strict-c-v00@openssh.com"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "ssh-ed25519"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "chacha20-poly1305@openssh.com"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "chacha20-poly1305@openssh.com"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, ""), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, ""), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "none"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, "none"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, ""), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&c_kexinit_buf, ""), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&c_kexinit_buf, 0), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u32(&c_kexinit_buf, 0), SSH_ERR_SUCCESS);

    /* Server parses client KEXINIT */
    ASSERT_EQ(kex_parse_kexinit(&server_kex, c_kexinit_buf.data, c_kexinit_buf.size), SSH_ERR_SUCCESS);
    ASSERT_EQ(server_kex.strict_kex, true);

    /* 3. Client starts ECDH */
    uint8_t c_priv[32], c_pub[32];
    ASSERT_EQ(crypto_x25519_keygen(c_priv, c_pub), 0);

    sshbuf_t c_ecdh_init;
    ASSERT_EQ(sshbuf_init(&c_ecdh_init, 64, 256), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&c_ecdh_init, SSH_MSG_KEX_ECDH_INIT), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_string(&c_ecdh_init, c_pub, 32), SSH_ERR_SUCCESS);

    /* 4. Server starts ECDH and processes client init */
    ASSERT_EQ(kex_start_ecdh(&server_kex), 0);

    const char client_ver[] = "SSH-2.0-OpenSSH_9.6";
    const char server_ver[] = SSH_VERSION_STRING;

    uint8_t s_hash[32];
    ASSERT_EQ(kex_process_ecdh_init(&server_kex, c_ecdh_init.data, c_ecdh_init.size,
                                    host_pubkey, client_ver, server_ver, s_hash), SSH_ERR_SUCCESS);

    /* Server signs hash H */
    uint8_t sig[64];
    ASSERT_EQ(crypto_ed25519_sign(sig, host_seckey, s_hash, 32), 0);

    /* Server builds ECDH_REPLY */
    sshbuf_t s_reply;
    ASSERT_EQ(sshbuf_init(&s_reply, 256, 1024), SSH_ERR_SUCCESS);
    ASSERT_EQ(kex_build_ecdh_reply(&server_kex, host_pubkey, sig, &s_reply), SSH_ERR_SUCCESS);

    /* Client validates server reply */
    sshbuf_t reply_reader;
    sshbuf_init_from(&reply_reader, s_reply.data, s_reply.size);
    uint8_t reply_msg = 0;
    ASSERT_EQ(sshbuf_get_u8(&reply_reader, &reply_msg), SSH_ERR_SUCCESS);
    ASSERT_EQ(reply_msg, SSH_MSG_KEX_ECDH_REPLY);

    const uint8_t *ks_blob = NULL;
    size_t ks_len = 0;
    ASSERT_EQ(sshbuf_get_string_ptr(&reply_reader, &ks_blob, &ks_len), SSH_ERR_SUCCESS);

    const uint8_t *s_pub = NULL;
    size_t s_pub_len = 0;
    ASSERT_EQ(sshbuf_get_string_ptr(&reply_reader, &s_pub, &s_pub_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(s_pub_len, 32);

    const uint8_t *sig_blob = NULL;
    size_t sig_len = 0;
    ASSERT_EQ(sshbuf_get_string_ptr(&reply_reader, &sig_blob, &sig_len), SSH_ERR_SUCCESS);

    /* Client computes shared secret K */
    uint8_t c_shared[32];
    ASSERT_EQ(crypto_x25519(c_shared, c_priv, s_pub), 0);
    ASSERT_MEMEQ(c_shared, server_kex.shared_secret, 32);

    /* Client computes hash H */
    sshbuf_t c_hbuf;
    ASSERT_EQ(sshbuf_init(&c_hbuf, 4096, 65536), SSH_ERR_SUCCESS);
    sshbuf_put_cstring(&c_hbuf, client_ver);
    sshbuf_put_cstring(&c_hbuf, server_ver);
    sshbuf_put_string(&c_hbuf, c_kexinit_buf.data, c_kexinit_buf.size);
    sshbuf_put_string(&c_hbuf, s_kexinit_buf.data, s_kexinit_buf.size);
    sshbuf_put_string(&c_hbuf, ks_blob, ks_len);
    sshbuf_put_string(&c_hbuf, c_pub, 32);
    sshbuf_put_string(&c_hbuf, s_pub, 32);
    sshbuf_put_mpint(&c_hbuf, c_shared, 32);

    uint8_t c_hash[32];
    crypto_sha256(c_hbuf.data, c_hbuf.size, c_hash);
    ASSERT_MEMEQ(c_hash, s_hash, 32);
    sshbuf_free(&c_hbuf);

    /* Client verifies host signature over H */
    sshbuf_t sig_reader;
    sshbuf_init_from(&sig_reader, sig_blob, sig_len);
    const uint8_t *sig_alg = NULL;
    size_t sig_alg_len = 0;
    ASSERT_EQ(sshbuf_get_string_ptr(&sig_reader, &sig_alg, &sig_alg_len), SSH_ERR_SUCCESS);
    const uint8_t *raw_sig = NULL;
    size_t raw_sig_len = 0;
    ASSERT_EQ(sshbuf_get_string_ptr(&sig_reader, &raw_sig, &raw_sig_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(raw_sig_len, 64);
    ASSERT_EQ(crypto_ed25519_verify(raw_sig, host_pubkey, c_hash, 32), 0);

    /* Server derives keys */
    ASSERT_EQ(kex_derive_keys(&server_kex), SSH_ERR_SUCCESS);

    /* Initialize server ciphers */
    ssh_cipher_ctx s_cipher_in, s_cipher_out;
    ssh_cipher_init(&s_cipher_in, server_kex.key_c2s);
    ssh_cipher_init(&s_cipher_out, server_kex.key_s2c);

    /* Initialize client ciphers */
    ssh_cipher_ctx c_cipher_in, c_cipher_out;
    ssh_cipher_init(&c_cipher_in, server_kex.key_s2c);
    ssh_cipher_init(&c_cipher_out, server_kex.key_c2s);

    /* Strict KEX reset upon NEWKEYS */
    if (server_kex.strict_kex) {
        ssh_cipher_reset_seq(&s_cipher_in);
        ssh_cipher_reset_seq(&s_cipher_out);
        ssh_cipher_reset_seq(&c_cipher_in);
        ssh_cipher_reset_seq(&c_cipher_out);
    }

    /* 5. Server sends encrypted SSH_MSG_USERAUTH_BANNER packet */
    sshbuf_t banner_payload;
    ASSERT_EQ(sshbuf_init(&banner_payload, 64, 256), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_u8(&banner_payload, 5), SSH_ERR_SUCCESS); /* padding len = 5 */
    ASSERT_EQ(sshbuf_put_u8(&banner_payload, SSH_MSG_USERAUTH_BANNER), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&banner_payload, "Welcome to FortressOS\n"), SSH_ERR_SUCCESS);
    ASSERT_EQ(sshbuf_put_cstring(&banner_payload, ""), SSH_ERR_SUCCESS);
    /* 5 bytes padding */
    uint8_t pad5[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    ASSERT_EQ(sshbuf_put_raw(&banner_payload, pad5, 5), SSH_ERR_SUCCESS);

    uint8_t enc_hdr[4];
    uint8_t *enc_body = malloc(banner_payload.size);
    uint8_t out_tag[16];
    ASSERT_EQ(ssh_cipher_encrypt_packet(&s_cipher_out, banner_payload.data, banner_payload.size,
                                        enc_hdr, enc_body, out_tag), SSH_ERR_SUCCESS);

    /* Client decrypts banner packet */
    uint32_t c_dec_len = 0;
    ASSERT_EQ(ssh_cipher_decrypt_length(&c_cipher_in, enc_hdr, &c_dec_len), SSH_ERR_SUCCESS);
    ASSERT_EQ(c_dec_len, banner_payload.size);

    uint8_t *c_dec_body = malloc(c_dec_len);
    ASSERT_EQ(ssh_cipher_decrypt_packet(&c_cipher_in, enc_hdr, c_dec_len,
                                        enc_body, out_tag, c_dec_body), SSH_ERR_SUCCESS);
    ASSERT_MEMEQ(c_dec_body, banner_payload.data, c_dec_len);

    free(enc_body);
    free(c_dec_body);
    sshbuf_free(&banner_payload);
    sshbuf_free(&s_kexinit_buf);
    sshbuf_free(&c_kexinit_buf);
    sshbuf_free(&c_ecdh_init);
    sshbuf_free(&s_reply);

    kex_cleanup(&server_kex);
    ssh_cipher_cleanup(&s_cipher_in);
    ssh_cipher_cleanup(&s_cipher_out);
    ssh_cipher_cleanup(&c_cipher_in);
    ssh_cipher_cleanup(&c_cipher_out);

    printf("PASS: test_kex_handshake_simulation\n");
}

int main(void) {
    test_sshbuf_primitives();
    test_sshbuf_mpint();
    test_chacha20_poly1305_worked_example();
    test_kex_handshake_simulation();
    printf("ALL TESTS PASSED (%s)\n", __FILE__);
    return 0;
}
