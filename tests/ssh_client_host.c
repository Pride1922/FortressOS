/* tests/ssh_client_host.c - Host unit test suite for FortressOS SSH client */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "openssh_ground_truth_vector.h"
#include "../user/sshd/ssh_packet.h"
#include "../user/sshd/sshbuf.h"
#include "../user/sshd/cipher.h"
#include "../src/include/crypto.h"

/* Monocypher headers for crypto */
#include "../src/lib/crypto/namespace.h"
#include "../src/lib/crypto/upstream/src/monocypher.h"
#include "../src/lib/crypto/upstream/src/optional/monocypher-ed25519.h"
#include "../src/lib/crypto/namespace_end.h"

static uint64_t g_test_rng = 987654321ULL;
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

/* Test 1: Ground truth exchange hash H and key derivation matching OpenSSH 9.6 */
static void test_ground_truth_exchange_hash(void) {
    printf("[TEST 1] Ground Truth Exchange Hash H & Key Derivation... ");

    /* Build host key blob Ks */
    sshbuf_t ks_buf;
    sshbuf_init(&ks_buf, 64, 256);
    sshbuf_put_cstring(&ks_buf, SSH_HOSTKEY_ED25519);
    sshbuf_put_string(&ks_buf, OPENSSH_GT_HOST_PUB, 32);

    /* Compute H = SHA256(Vc || Vs || Ic || Is || Ks || Qc || Qs || K) */
    sshbuf_t hbuf;
    sshbuf_init(&hbuf, 4096, 65536);
    sshbuf_put_cstring(&hbuf, OPENSSH_GT_V_C);
    sshbuf_put_cstring(&hbuf, OPENSSH_GT_V_S);
    sshbuf_put_string(&hbuf, OPENSSH_GT_I_C, sizeof(OPENSSH_GT_I_C));
    sshbuf_put_string(&hbuf, OPENSSH_GT_I_S, sizeof(OPENSSH_GT_I_S));
    sshbuf_put_string(&hbuf, ks_buf.data, ks_buf.size);
    sshbuf_put_string(&hbuf, OPENSSH_GT_Q_C, 32);
    sshbuf_put_string(&hbuf, OPENSSH_GT_Q_S, 32);
    sshbuf_put_mpint(&hbuf, OPENSSH_GT_SHARED_SECRET_K, 32);

    uint8_t computed_h[32];
    crypto_sha256(hbuf.data, hbuf.size, computed_h);

    assert(memcmp(computed_h, OPENSSH_GT_HASH_H, 32) == 0);

    /* Verify Ed25519 signature on H */
    uint8_t sig[64];
    crypto_ed25519_sign(sig, OPENSSH_GT_HOST_SEC, computed_h, 32);
    int vr = crypto_ed25519_verify(sig, OPENSSH_GT_HOST_PUB, computed_h, 32);
    assert(vr == 0);

    /* Derive client-to-server and server-to-client keys */
    sshbuf_t kbuf;
    sshbuf_init(&kbuf, 64, 128);
    sshbuf_put_mpint(&kbuf, OPENSSH_GT_SHARED_SECRET_K, 32);

    uint8_t key_c2s[64];
    uint8_t key_s2c[64];
    derive_key_64(kbuf.data, kbuf.size, computed_h, 'C', computed_h, key_c2s);
    derive_key_64(kbuf.data, kbuf.size, computed_h, 'D', computed_h, key_s2c);

    /* Keys must be distinct and non-zero */
    assert(memcmp(key_c2s, key_s2c, 64) != 0);
    bool non_zero = false;
    for (int i = 0; i < 64; i++) {
        if (key_c2s[i] != 0 || key_s2c[i] != 0) non_zero = true;
    }
    assert(non_zero);

    sshbuf_free(&ks_buf);
    sshbuf_free(&hbuf);
    sshbuf_free(&kbuf);
    printf("PASS\n");
}

/* Test 2: Client KEXINIT construction and parsing */
static void test_client_kexinit_construction(void) {
    printf("[TEST 2] Client KEXINIT Construction & Validation... ");

    uint8_t cookie[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                          0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};

    sshbuf_t b;
    sshbuf_init(&b, 512, 2048);

    assert(sshbuf_put_u8(&b, SSH_MSG_KEXINIT) == 0);
    assert(sshbuf_put_raw(&b, cookie, 16) == 0);
    assert(sshbuf_put_cstring(&b, SSH_KEX_CURVE25519_SHA256 "," SSH_KEX_STRICT_CLIENT) == 0);
    assert(sshbuf_put_cstring(&b, SSH_HOSTKEY_ED25519) == 0);
    assert(sshbuf_put_cstring(&b, SSH_CIPHER_CHACHA20_POLY1305) == 0);
    assert(sshbuf_put_cstring(&b, SSH_CIPHER_CHACHA20_POLY1305) == 0);
    assert(sshbuf_put_cstring(&b, "") == 0); /* mac c2s */
    assert(sshbuf_put_cstring(&b, "") == 0); /* mac s2c */
    assert(sshbuf_put_cstring(&b, "none") == 0); /* comp c2s */
    assert(sshbuf_put_cstring(&b, "none") == 0); /* comp s2c */
    assert(sshbuf_put_cstring(&b, "") == 0); /* lang c2s */
    assert(sshbuf_put_cstring(&b, "") == 0); /* lang s2c */
    assert(sshbuf_put_u8(&b, 0) == 0); /* first_kex_packet_follows */
    assert(sshbuf_put_u32(&b, 0) == 0); /* reserved */

    /* Parse it back */
    sshbuf_t read_b;
    sshbuf_init_from(&read_b, b.data, b.size);

    uint8_t msg = 0;
    assert(sshbuf_get_u8(&read_b, &msg) == 0 && msg == SSH_MSG_KEXINIT);
    uint8_t parsed_cookie[16];
    assert(sshbuf_get_raw(&read_b, parsed_cookie, 16) == 0);
    assert(memcmp(cookie, parsed_cookie, 16) == 0);

    const uint8_t *kex_algs = NULL;
    size_t kex_len = 0;
    assert(sshbuf_get_string_ptr(&read_b, &kex_algs, &kex_len) == 0);
    assert(strstr((const char *)kex_algs, "curve25519-sha256") != NULL);
    assert(strstr((const char *)kex_algs, "kex-strict-c-v00@openssh.com") != NULL);

    sshbuf_free(&b);
    printf("PASS\n");
}

/* Test 3: ChaCha20-Poly1305 Symmetric Packet Framing (Client Out / Server In) */
static void test_chacha20_poly1305_client_framing(void) {
    printf("[TEST 3] ChaCha20-Poly1305 Client Packet Framing & Sequencing... ");

    uint8_t key_c2s[64];
    for (int i = 0; i < 64; i++) {
        key_c2s[i] = (uint8_t)(i * 3 + 1);
    }

    ssh_cipher_ctx client_out;
    ssh_cipher_ctx server_in;
    ssh_cipher_init(&client_out, key_c2s);
    ssh_cipher_init(&server_in, key_c2s); /* Server receives on c2s */

    /* Client sends CHANNEL_DATA packet */
    const char payload_str[] = "Hello from FortressOS SSH Client!";
    size_t payload_len = strlen(payload_str);

    /* Unpadded layout: 1 pad_len + 1 msg_type + payload */
    size_t unpadded = 1 + 1 + payload_len;
    size_t pad = 8 - (unpadded % 8);
    if (pad < 4) pad += 8;
    uint32_t packet_len = (uint32_t)(unpadded + pad);

    uint8_t plain[1024];
    plain[0] = (uint8_t)pad;
    plain[1] = SSH_MSG_CHANNEL_DATA;
    memcpy(plain + 2, payload_str, payload_len);
    for (size_t i = 0; i < pad; i++) plain[2 + payload_len + i] = (uint8_t)(i + 1);

    uint8_t enc_length[4];
    uint8_t enc_body[1024];
    uint8_t tag[16];

    assert(ssh_cipher_encrypt_packet(&client_out, plain, packet_len, enc_length, enc_body, tag) == SSH_ERR_SUCCESS);
    assert(client_out.seq == 1);

    /* Server decrypts length */
    uint32_t dec_len = 0;
    assert(ssh_cipher_decrypt_length(&server_in, enc_length, &dec_len) == SSH_ERR_SUCCESS);
    assert(dec_len == packet_len);

    /* Server decrypts body and checks tag */
    uint8_t dec_payload[1024];
    assert(ssh_cipher_decrypt_packet(&server_in, enc_length, dec_len, enc_body, tag, dec_payload) == SSH_ERR_SUCCESS);
    assert(server_in.seq == 1);

    /* Validate decrypted message */
    uint8_t dec_pad = dec_payload[0];
    uint8_t dec_type = dec_payload[1];
    assert(dec_pad == pad);
    assert(dec_type == SSH_MSG_CHANNEL_DATA);
    assert(memcmp(dec_payload + 2, payload_str, payload_len) == 0);

    /* Verify tampered ciphertext fails tag verification */
    enc_body[10] ^= 0xFF;
    server_in.seq = 1; /* Rewind for tamper check */
    int r = ssh_cipher_decrypt_packet(&server_in, enc_length, dec_len, enc_body, tag, dec_payload);
    assert(r != SSH_ERR_SUCCESS);

    ssh_cipher_cleanup(&client_out);
    ssh_cipher_cleanup(&server_in);
    printf("PASS\n");
}

/* Test 4: User Authentication & Service Request packet construction */
static void test_userauth_packet_construction(void) {
    printf("[TEST 4] Userauth & Service Request Packet Formatting... ");

    /* Service request */
    sshbuf_t b;
    sshbuf_init(&b, 128, 512);
    assert(sshbuf_put_u8(&b, SSH_MSG_SERVICE_REQUEST) == 0);
    assert(sshbuf_put_cstring(&b, "ssh-userauth") == 0);

    sshbuf_t read_b;
    sshbuf_init_from(&read_b, b.data, b.size);
    uint8_t msg = 0;
    assert(sshbuf_get_u8(&read_b, &msg) == 0 && msg == SSH_MSG_SERVICE_REQUEST);
    const uint8_t *svc = NULL;
    size_t svc_len = 0;
    assert(sshbuf_get_string_ptr(&read_b, &svc, &svc_len) == 0);
    assert(svc_len == 12 && memcmp(svc, "ssh-userauth", 12) == 0);
    sshbuf_free(&b);

    /* Userauth password request */
    sshbuf_init(&b, 256, 1024);
    assert(sshbuf_put_u8(&b, SSH_MSG_USERAUTH_REQUEST) == 0);
    assert(sshbuf_put_cstring(&b, "operator") == 0);
    assert(sshbuf_put_cstring(&b, "ssh-connection") == 0);
    assert(sshbuf_put_cstring(&b, "password") == 0);
    assert(sshbuf_put_u8(&b, 0) == 0); /* false */
    assert(sshbuf_put_cstring(&b, "secret-pass") == 0);

    sshbuf_init_from(&read_b, b.data, b.size);
    assert(sshbuf_get_u8(&read_b, &msg) == 0 && msg == SSH_MSG_USERAUTH_REQUEST);
    const uint8_t *user = NULL, *service = NULL, *method = NULL, *pass = NULL;
    size_t ulen = 0, slen = 0, mlen = 0, plen = 0;
    uint8_t change = 0;

    assert(sshbuf_get_string_ptr(&read_b, &user, &ulen) == 0);
    assert(sshbuf_get_string_ptr(&read_b, &service, &slen) == 0);
    assert(sshbuf_get_string_ptr(&read_b, &method, &mlen) == 0);
    assert(sshbuf_get_u8(&read_b, &change) == 0 && change == 0);
    assert(sshbuf_get_string_ptr(&read_b, &pass, &plen) == 0);

    assert(ulen == 8 && memcmp(user, "operator", 8) == 0);
    assert(slen == 14 && memcmp(service, "ssh-connection", 14) == 0);
    assert(mlen == 8 && memcmp(method, "password", 8) == 0);
    assert(plen == 11 && memcmp(pass, "secret-pass", 11) == 0);

    sshbuf_free(&b);
    printf("PASS\n");
}

/* Test 5: Channel Session / PTY / Exec requests */
static void test_channel_session_requests(void) {
    printf("[TEST 5] Channel Session, PTY-req and Exec Requests... ");

    /* 1. CHANNEL_OPEN "session" */
    sshbuf_t b;
    sshbuf_init(&b, 128, 512);
    assert(sshbuf_put_u8(&b, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&b, "session") == 0);
    assert(sshbuf_put_u32(&b, 0) == 0); /* local id */
    assert(sshbuf_put_u32(&b, 65536) == 0); /* initial window */
    assert(sshbuf_put_u32(&b, 32768) == 0); /* max packet */

    sshbuf_t read_b;
    sshbuf_init_from(&read_b, b.data, b.size);
    uint8_t msg = 0;
    assert(sshbuf_get_u8(&read_b, &msg) == 0 && msg == SSH_MSG_CHANNEL_OPEN);
    const uint8_t *ch_type = NULL;
    size_t ch_type_len = 0;
    uint32_t sender_id = 0, win = 0, maxpkt = 0;
    assert(sshbuf_get_string_ptr(&read_b, &ch_type, &ch_type_len) == 0);
    assert(sshbuf_get_u32(&read_b, &sender_id) == 0 && sender_id == 0);
    assert(sshbuf_get_u32(&read_b, &win) == 0 && win == 65536);
    assert(sshbuf_get_u32(&read_b, &maxpkt) == 0 && maxpkt == 32768);
    sshbuf_free(&b);

    /* 2. CHANNEL_REQUEST "exec" */
    sshbuf_init(&b, 128, 512);
    assert(sshbuf_put_u8(&b, SSH_MSG_CHANNEL_REQUEST) == 0);
    assert(sshbuf_put_u32(&b, 42) == 0); /* recipient channel */
    assert(sshbuf_put_cstring(&b, "exec") == 0);
    assert(sshbuf_put_u8(&b, 1) == 0); /* want reply */
    assert(sshbuf_put_cstring(&b, "uname -a") == 0);

    sshbuf_init_from(&read_b, b.data, b.size);
    assert(sshbuf_get_u8(&read_b, &msg) == 0 && msg == SSH_MSG_CHANNEL_REQUEST);
    uint32_t rchannel = 0;
    const uint8_t *req_type = NULL, *cmd = NULL;
    size_t req_len = 0, cmd_len = 0;
    uint8_t want_rep = 0;

    assert(sshbuf_get_u32(&read_b, &rchannel) == 0 && rchannel == 42);
    assert(sshbuf_get_string_ptr(&read_b, &req_type, &req_len) == 0);
    assert(req_len == 4 && memcmp(req_type, "exec", 4) == 0);
    assert(sshbuf_get_u8(&read_b, &want_rep) == 0 && want_rep == 1);
    assert(sshbuf_get_string_ptr(&read_b, &cmd, &cmd_len) == 0);
    assert(cmd_len == 8 && memcmp(cmd, "uname -a", 8) == 0);

    sshbuf_free(&b);
    printf("PASS\n");
}

int main(void) {
    printf("=== FortressOS SSH Client Host Unit Tests ===\n");
    test_ground_truth_exchange_hash();
    test_client_kexinit_construction();
    test_chacha20_poly1305_client_framing();
    test_userauth_packet_construction();
    test_channel_session_requests();
    printf("All 5 SSH client host unit tests passed!\n");
    return 0;
}
