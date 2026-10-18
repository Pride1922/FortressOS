/* Actual worker receive path with a mocked socket and real packet crypto. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static long socket_syscall(long, uintptr_t, uintptr_t, uintptr_t,
                            uintptr_t, uintptr_t, uintptr_t);
#define SSHD_SYSCALL socket_syscall
#include "user/sshd/sshd.c"

static const uint8_t *pending;
static size_t pending_len;
static unsigned reads;
static long empty_result;
static sshbuf_t sent_wire;
static size_t send_limit;
static bool fail_send;
static long send_failure;

static long socket_syscall(long nr, uintptr_t fd, uintptr_t buf, uintptr_t cap,
                            uintptr_t d, uintptr_t e, uintptr_t f) {
    (void)d; (void)e; (void)f;
    if (nr == SYS_SEND) {
        assert(fd == 7);
        if (fail_send) return send_failure;
        size_t n = send_limit && send_limit < cap ? send_limit : cap;
        assert(sshbuf_put_raw(&sent_wire, (const void *)buf, n) == 0);
        return (long)n;
    }
    assert(nr == SYS_RECV && fd == 7);
    reads++;
    if (!pending_len) return empty_result;
    size_t n = pending_len < cap ? pending_len : cap;
    memcpy((void *)buf, pending, n);
    pending += n;
    pending_len -= n;
    return (long)n;
}

int64_t crypto_test_getrandom(void *p, size_t n, uint64_t flags) {
    (void)flags;
    memset(p, 0x42, n);
    return (int64_t)n;
}

void crypto_test_wiped(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) assert(*b++ == 0);
}

static void test_transition(size_t prefix, const sshbuf_t *wire,
                            size_t newkeys_len, const uint8_t key[64]) {
    static ssh_transport_t server;
    assert(ssh_transport_init(&server) == 0);
    assert(ssh_transport_feed_input(&server, sshbuf_ptr(wire), newkeys_len + prefix) == 0);
    uint8_t type, scratch[32];
    const uint8_t *payload;
    size_t len;
    assert(ssh_transport_get_packet(&server, &type, &payload, &len) == 0);
    assert(type == SSH_MSG_NEWKEYS);
    /* The worker applies negotiated inbound keys before entering auth. */
    ssh_cipher_init(&server.in_cipher, key);
    server.initial_kex_done = true;
    pending = sshbuf_ptr(wire) + newkeys_len + prefix;
    pending_len = sshbuf_len(wire) - newkeys_len - prefix;
    reads = 0;
    assert(sshd_receive_packet(&server, 7, scratch, sizeof(scratch), &type, &payload, &len) == 0);
    assert(type == SSH_MSG_SERVICE_REQUEST);
    assert(len == 17 && memcmp(payload + 5, "ssh-userauth", 12) == 0);
    if (prefix == sshbuf_len(wire) - newkeys_len) assert(reads == 0);
    else assert(reads > 0);

    ssh_auth_ctx_t auth = {0};
    sshbuf_t reply;
    assert(sshbuf_init(&reply, 64, 256) == 0);
    assert(auth_process_service_request(&auth, payload, len, &reply) == 0);
    assert(sshbuf_ptr(&reply)[0] == SSH_MSG_SERVICE_ACCEPT);
    sshbuf_free(&reply);
    ssh_transport_cleanup(&server);
}

static void test_large_channel_reply(size_t limit) {
    static ssh_transport_t sender, receiver;
    static uint8_t data[4096];
    uint8_t key[64];
    memset(data, 'D', sizeof(data));memset(key, 0x6b, sizeof(key));
    assert(ssh_transport_init(&sender) == 0);
    assert(ssh_transport_init(&receiver) == 0);
    ssh_cipher_init(&sender.out_cipher, key);
    ssh_cipher_init(&receiver.in_cipher, key);
    sshbuf_t reply;
    assert(sshbuf_init(&reply, 512, 16384) == 0);
    assert(sshbuf_init(&sent_wire, 512, 16384) == 0);
    assert(sshbuf_put_u8(&reply, SSH_MSG_CHANNEL_DATA) == 0);
    assert(sshbuf_put_u32(&reply, 0) == 0);
    assert(sshbuf_put_string(&reply, data, sizeof(data)) == 0);
    send_limit=limit;
    assert(send_channel_replies(&sender, 7, &reply) == 0);
    assert(ssh_transport_feed_input(&receiver, sshbuf_ptr(&sent_wire), sshbuf_len(&sent_wire)) == 0);
    uint8_t type;const uint8_t *payload;size_t len;
    assert(ssh_transport_get_packet(&receiver, &type, &payload, &len) == 0);
    assert(type == SSH_MSG_CHANNEL_DATA && len == sizeof(data)+9);
    assert(memcmp(payload+9, data, sizeof(data)) == 0);
    assert(sshbuf_len(&reply) == 0);
    for (unsigned failure=0;failure<2;failure++) {
        assert(sshbuf_put_u8(&reply, SSH_MSG_CHANNEL_EOF) == 0);
        assert(sshbuf_put_u32(&reply, 0) == 0);
        fail_send=true;send_failure=failure ? -1 : 0;
        assert(send_channel_replies(&sender, 7, &reply) == SSH_ERR_DISCONNECT);
        fail_send=false;sshbuf_reset(&reply);
    }
    sshbuf_free(&reply);sshbuf_free(&sent_wire);
    ssh_transport_cleanup(&sender);ssh_transport_cleanup(&receiver);
}

int main(void) {
    test_large_channel_reply(0);
    test_large_channel_reply(137);
    puts("PASS: 4096-byte encrypted channel reply, short sends, zero progress and socket failure");
    static ssh_transport_t client, server;
    uint8_t key[64];
    memset(key, 0x5a, sizeof(key));
    sshbuf_t wire, service;
    assert(sshbuf_init(&wire, 256, 4096) == 0);
    assert(sshbuf_init(&service, 32, 256) == 0);
    assert(ssh_transport_init(&client) == 0);
    assert(ssh_transport_send_packet(&client, SSH_MSG_NEWKEYS, NULL, 0, &wire) == 0);
    size_t newkeys_len = sshbuf_len(&wire);
    ssh_cipher_init(&client.out_cipher, key);
    assert(sshbuf_put_cstring(&service, "ssh-userauth") == 0);
    assert(ssh_transport_send_packet(&client, SSH_MSG_SERVICE_REQUEST,
                                    sshbuf_ptr(&service), sshbuf_len(&service), &wire) == 0);
    /* Every split, including the complete coalesced NEWKEYS + encrypted request. */
    for (size_t prefix = 0; prefix <= sshbuf_len(&wire) - newkeys_len; prefix++)
        test_transition(prefix, &wire, newkeys_len, key);

    assert(ssh_transport_init(&server) == 0);
    uint8_t scratch[32], type;
    const uint8_t *payload;
    size_t len;
    pending_len = 0;
    reads = 0;
    empty_result = 0;
    assert(sshd_receive_packet(&server, 7, scratch, sizeof(scratch), &type, &payload, &len) == SSH_ERR_DISCONNECT);
    assert(reads == 1);
    empty_result = -1;
    assert(sshd_receive_packet(&server, 7, scratch, sizeof(scratch), &type, &payload, &len) == SSH_ERR_DISCONNECT);
    /* A buffered framing error must return immediately instead of reading forever. */
    const uint8_t bad_length[4] = {0, 0, 0, 0};
    assert(ssh_transport_feed_input(&server, bad_length, sizeof(bad_length)) == 0);
    reads = 0;
    assert(sshd_receive_packet(&server, 7, scratch, sizeof(scratch), &type, &payload, &len) == SSH_ERR_INVALID_FORMAT);
    assert(reads == 0);
    ssh_transport_cleanup(&server);
    ssh_transport_cleanup(&client);
    sshbuf_free(&wire);
    sshbuf_free(&service);
    puts("PASS: worker drains coalesced encrypted service request, every split, EOF, socket and framing errors");
    return 0;
}
