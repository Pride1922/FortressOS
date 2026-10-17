#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "user/sshd/channel.h"

void crypto_wipe(void *p, size_t n) {
    if (!p) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

static void test_channel_open_and_confirmation(void) {
    printf("Running test_channel_open_and_confirmation...\n");

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    /* Construct client SSH_MSG_CHANNEL_OPEN for "session" */
    sshbuf_t req, reply;
    assert(sshbuf_init(&req, 128, 512) == 0);
    assert(sshbuf_init(&reply, 128, 512) == 0);

    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "session") == 0);
    assert(sshbuf_put_u32(&req, 42) == 0);          /* sender_channel */
    assert(sshbuf_put_u32(&req, 32768) == 0);       /* initial_window */
    assert(sshbuf_put_u32(&req, 16384) == 0);       /* max_packet */

    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    /* Verify reply is SSH_MSG_CHANNEL_OPEN_CONFIRMATION */
    uint8_t type = 0;
    uint32_t r_id = 0, s_id = 0, win = 0, maxpkt = 0;
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    assert(sshbuf_get_u32(&reply, &r_id) == 0);
    assert(r_id == 42);
    assert(sshbuf_get_u32(&reply, &s_id) == 0);
    assert(s_id == 0);
    assert(sshbuf_get_u32(&reply, &win) == 0);
    assert(win == SSH_CHANNEL_DEFAULT_WINDOW);
    assert(sshbuf_get_u32(&reply, &maxpkt) == 0);
    assert(maxpkt == SSH_CHANNEL_DEFAULT_MAXPACKET);

    /* Verify channel table state */
    ssh_channel_t *c = channel_lookup_local(&mgr, 0);
    assert(c != NULL);
    assert(c->state == SSH_CHANNEL_STATE_OPEN);
    assert(c->remote_id == 42);
    assert(c->remote_window == 32768);
    assert(c->remote_maxpacket == 16384);

    sshbuf_free(&req);
    sshbuf_free(&reply);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_channel_open_and_confirmation\n");
}

static void test_channel_open_rejections(void) {
    printf("Running test_channel_open_rejections...\n");

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    sshbuf_t req, reply;
    assert(sshbuf_init(&req, 128, 512) == 0);
    assert(sshbuf_init(&reply, 128, 512) == 0);

    /* 1. Reject unknown channel type */
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "direct-tcpip") == 0);
    assert(sshbuf_put_u32(&req, 10) == 0);
    assert(sshbuf_put_u32(&req, 32768) == 0);
    assert(sshbuf_put_u32(&req, 16384) == 0);

    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    uint8_t type = 0;
    uint32_t r_id = 0, reason = 0;
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_OPEN_FAILURE);
    assert(sshbuf_get_u32(&reply, &r_id) == 0);
    assert(r_id == 10);
    assert(sshbuf_get_u32(&reply, &reason) == 0);
    assert(reason == SSH_OPEN_UNKNOWN_CHANNEL_TYPE);

    /* 2. Open channels until capacity exhausted */
    for (uint32_t i = 0; i < SSH_CHANNEL_MAX_CHANNELS; i++) {
        sshbuf_reset(&req);
        sshbuf_reset(&reply);
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
        assert(sshbuf_put_cstring(&req, "session") == 0);
        assert(sshbuf_put_u32(&req, 100 + i) == 0);
        assert(sshbuf_put_u32(&req, 32768) == 0);
        assert(sshbuf_put_u32(&req, 16384) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
        assert(sshbuf_get_u8(&reply, &type) == 0);
        assert(type == SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    }

    /* Next open must fail with RESOURCE_SHORTAGE */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "session") == 0);
    assert(sshbuf_put_u32(&req, 999) == 0);
    assert(sshbuf_put_u32(&req, 32768) == 0);
    assert(sshbuf_put_u32(&req, 16384) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_OPEN_FAILURE);
    assert(sshbuf_get_u32(&reply, &r_id) == 0);
    assert(r_id == 999);
    assert(sshbuf_get_u32(&reply, &reason) == 0);
    assert(reason == SSH_OPEN_RESOURCE_SHORTAGE);

    sshbuf_free(&req);
    sshbuf_free(&reply);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_channel_open_rejections\n");
}

static void test_channel_requests(void) {
    printf("Running test_channel_requests...\n");

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    sshbuf_t req, reply;
    assert(sshbuf_init(&req, 256, 1024) == 0);
    assert(sshbuf_init(&reply, 256, 1024) == 0);

    /* Open channel 0 (remote 55) */
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "session") == 0);
    assert(sshbuf_put_u32(&req, 55) == 0);
    assert(sshbuf_put_u32(&req, 32768) == 0);
    assert(sshbuf_put_u32(&req, 16384) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    /* 1. Request PTY */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_REQUEST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);           /* local_id */
    assert(sshbuf_put_cstring(&req, "pty-req") == 0);
    assert(sshbuf_put_u8(&req, 1) == 0);           /* want_reply = true */
    assert(sshbuf_put_cstring(&req, "xterm-256color") == 0);
    assert(sshbuf_put_u32(&req, 80) == 0);          /* cols */
    assert(sshbuf_put_u32(&req, 24) == 0);          /* rows */
    assert(sshbuf_put_u32(&req, 640) == 0);         /* width_px */
    assert(sshbuf_put_u32(&req, 480) == 0);         /* height_px */
    assert(sshbuf_put_cstring(&req, "") == 0);      /* modes */

    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_REQUEST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    uint8_t type = 0;
    uint32_t r_id = 0;
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_SUCCESS);
    assert(sshbuf_get_u32(&reply, &r_id) == 0);
    assert(r_id == 55);

    ssh_channel_t *c = channel_lookup_local(&mgr, 0);
    assert(c != NULL);
    assert(c->pty_requested == true);
    assert(strcmp(c->pty_params.term, "xterm-256color") == 0);
    assert(c->pty_params.cols == 80 && c->pty_params.rows == 24);

    /* 2. Window change */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_REQUEST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_cstring(&req, "window-change") == 0);
    assert(sshbuf_put_u8(&req, 0) == 0);           /* want_reply = false */
    assert(sshbuf_put_u32(&req, 120) == 0);         /* cols */
    assert(sshbuf_put_u32(&req, 40) == 0);          /* rows */
    assert(sshbuf_put_u32(&req, 1024) == 0);
    assert(sshbuf_put_u32(&req, 768) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_REQUEST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(sshbuf_len(&reply) == 0); /* No reply requested */
    assert(c->pty_params.cols == 120 && c->pty_params.rows == 40);

    /* 3. Exec request */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_REQUEST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_cstring(&req, "exec") == 0);
    assert(sshbuf_put_u8(&req, 1) == 0);           /* want_reply = true */
    assert(sshbuf_put_cstring(&req, "uname -a") == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_REQUEST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_SUCCESS);
    assert(c->exec_requested == true);
    assert(strcmp(c->exec_cmd, "uname -a") == 0);

    /* 4. Unsupported request with want_reply */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_REQUEST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_cstring(&req, "x11-req") == 0);
    assert(sshbuf_put_u8(&req, 1) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_REQUEST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_FAILURE);

    /* 5. Global request failure */
    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_GLOBAL_REQUEST) == 0);
    assert(sshbuf_put_cstring(&req, "tcpip-forward") == 0);
    assert(sshbuf_put_u8(&req, 1) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_GLOBAL_REQUEST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_REQUEST_FAILURE);

    sshbuf_free(&req);
    sshbuf_free(&reply);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_channel_requests\n");
}

static void test_data_transfer_and_window_credits(void) {
    printf("Running test_data_transfer_and_window_credits...\n");

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    sshbuf_t req, reply, pkt;
    assert(sshbuf_init(&req, 1024, 64 * 1024) == 0);
    assert(sshbuf_init(&reply, 1024, 64 * 1024) == 0);
    assert(sshbuf_init(&pkt, 1024, 64 * 1024) == 0);

    /* Open channel with small remote window for flow control testing */
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "session") == 0);
    assert(sshbuf_put_u32(&req, 77) == 0);          /* remote_id */
    assert(sshbuf_put_u32(&req, 2048) == 0);        /* remote initial_window = 2048 */
    assert(sshbuf_put_u32(&req, 1024) == 0);        /* remote max_packet = 1024 */
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    /* 1. Client sends data: 500 bytes */
    uint8_t sample[500];
    for (size_t i = 0; i < sizeof(sample); i++) sample[i] = (uint8_t)(i & 0xFF);

    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_DATA) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);           /* local_id */
    assert(sshbuf_put_string(&req, sample, sizeof(sample)) == 0);

    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_DATA, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    /* Window still above MIN_WINDOW_ADJUST (65536 - 500 > 16384), so no window adjust packet yet */
    assert(sshbuf_len(&reply) == 0);

    /* Verify data received into channel's inbound buffer */
    uint8_t read_buf[512];
    size_t got = channel_read_inbound(&mgr, 0, read_buf, sizeof(read_buf));
    assert(got == 500);
    assert(memcmp(read_buf, sample, 500) == 0);

    /* 2. Client sends enough data to cross window adjustment threshold (50000 bytes) */
    uint8_t big_chunk[50000];
    memset(big_chunk, 'A', sizeof(big_chunk));

    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_DATA) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_string(&req, big_chunk, sizeof(big_chunk)) == 0);

    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_DATA, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    /* Window fell to 65536 - 500 - 50000 = 15036 < 16384, so reply must contain WINDOW_ADJUST */
    uint8_t type = 0;
    uint32_t r_id = 0, adjust_bytes = 0;
    assert(sshbuf_get_u8(&reply, &type) == 0);
    assert(type == SSH_MSG_CHANNEL_WINDOW_ADJUST);
    assert(sshbuf_get_u32(&reply, &r_id) == 0);
    assert(r_id == 77);
    assert(sshbuf_get_u32(&reply, &adjust_bytes) == 0);
    assert(adjust_bytes == 50500); /* Restores local window back to 65536! */

    ssh_channel_t *c = channel_lookup_local(&mgr, 0);
    assert(c != NULL);
    assert(c->local_window == SSH_CHANNEL_DEFAULT_WINDOW);

    /* 3. Server outbound transmission and window throttling */
    uint8_t out_msg[2500];
    memset(out_msg, 'B', sizeof(out_msg));

    /* Remote window is 2048, maxpacket is 1024.
     * First send: should send maxpacket = 1024 bytes */
    sshbuf_reset(&pkt);
    int64_t sent1 = channel_send_data(&mgr, 0, out_msg, sizeof(out_msg), &pkt);
    assert(sent1 == 1024);
    assert(channel_get_write_avail(&mgr, 0) == 1024);

    /* Verify sent packet structure */
    uint8_t pkt_type = 0;
    uint32_t pkt_rid = 0;
    const uint8_t *pkt_data = NULL;
    size_t pkt_len = 0;
    assert(sshbuf_get_u8(&pkt, &pkt_type) == 0);
    assert(pkt_type == SSH_MSG_CHANNEL_DATA);
    assert(sshbuf_get_u32(&pkt, &pkt_rid) == 0);
    assert(pkt_rid == 77);
    assert(sshbuf_get_string_ptr(&pkt, &pkt_data, &pkt_len) == 0);
    assert(pkt_len == 1024);

    /* Second send: should send remaining 1024 bytes */
    sshbuf_reset(&pkt);
    int64_t sent2 = channel_send_data(&mgr, 0, out_msg + 1024, sizeof(out_msg) - 1024, &pkt);
    assert(sent2 == 1024);
    assert(channel_get_write_avail(&mgr, 0) == 0); /* Window completely exhausted! */

    /* Third send: window exhausted, returns 0 */
    sshbuf_reset(&pkt);
    int64_t sent3 = channel_send_data(&mgr, 0, out_msg + 2048, sizeof(out_msg) - 2048, &pkt);
    assert(sent3 == 0);
    assert(sshbuf_len(&pkt) == 0);

    /* 4. Remote sends WINDOW_ADJUST (+4096 bytes) */
    sshbuf_reset(&req);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_WINDOW_ADJUST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_u32(&req, 4096) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_WINDOW_ADJUST, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
    assert(channel_get_write_avail(&mgr, 0) == 4096);

    /* Now remaining 452 bytes can be sent */
    sshbuf_reset(&pkt);
    int64_t sent4 = channel_send_data(&mgr, 0, out_msg + 2048, sizeof(out_msg) - 2048, &pkt);
    assert(sent4 == 452);
    assert(channel_get_write_avail(&mgr, 0) == 4096 - 452);

    /* 5. Extended data (stderr) */
    sshbuf_reset(&pkt);
    int64_t sent_err = channel_send_stderr(&mgr, 0, (const uint8_t *)"error line\n", 11, &pkt);
    assert(sent_err == 11);
    assert(sshbuf_get_u8(&pkt, &pkt_type) == 0);
    assert(pkt_type == SSH_MSG_CHANNEL_EXTENDED_DATA);
    assert(sshbuf_get_u32(&pkt, &pkt_rid) == 0);
    assert(pkt_rid == 77);
    uint32_t ext_type = 0;
    assert(sshbuf_get_u32(&pkt, &ext_type) == 0);
    assert(ext_type == SSH_EXTENDED_DATA_STDERR);

    sshbuf_free(&req);
    sshbuf_free(&reply);
    sshbuf_free(&pkt);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_data_transfer_and_window_credits\n");
}

static void test_adversarial_channel_violations(void) {
    printf("Running test_adversarial_channel_violations...\n");

    ssh_channel_mgr_t mgr;
    channel_mgr_init(&mgr);

    sshbuf_t req, reply;
    assert(sshbuf_init(&req, 1024, 80000) == 0);
    assert(sshbuf_init(&reply, 1024, 80000) == 0);

    /* Open channel with default window 65536 */
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
    assert(sshbuf_put_cstring(&req, "session") == 0);
    assert(sshbuf_put_u32(&req, 88) == 0);
    assert(sshbuf_put_u32(&req, 32768) == 0);
    assert(sshbuf_put_u32(&req, 16384) == 0);
    assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

    /* 1. Client attempts to send more data than available local window (65537 bytes) */
    uint8_t over_data[65537];
    memset(over_data, 'Z', sizeof(over_data));

    sshbuf_reset(&req);
    sshbuf_reset(&reply);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_DATA) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_string(&req, over_data, sizeof(over_data)) == 0);

    int r = channel_process_packet(&mgr, SSH_MSG_CHANNEL_DATA, sshbuf_ptr(&req), sshbuf_len(&req), &reply);
    assert(r == SSH_ERR_PROTOCOL_VIOLATION);

    /* 2. Window adjust integer overflow */
    sshbuf_reset(&req);
    assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_WINDOW_ADJUST) == 0);
    assert(sshbuf_put_u32(&req, 0) == 0);
    assert(sshbuf_put_u32(&req, 0xFFFFFFFF) == 0); /* will overflow 32768 + 0xFFFFFFFF */

    r = channel_process_packet(&mgr, SSH_MSG_CHANNEL_WINDOW_ADJUST, sshbuf_ptr(&req), sshbuf_len(&req), &reply);
    assert(r == SSH_ERR_PROTOCOL_VIOLATION);

    sshbuf_free(&req);
    sshbuf_free(&reply);
    channel_mgr_cleanup(&mgr);
    printf("PASS: test_adversarial_channel_violations\n");
}

static void test_eof_and_close_interleavings(void) {
    printf("Running test_eof_and_close_interleavings...\n");

    /* CASE A: Server sends exit-status, then EOF, then CLOSE. Client responds with CLOSE. */
    {
        ssh_channel_mgr_t mgr;
        channel_mgr_init(&mgr);

        sshbuf_t req, reply, pkt;
        assert(sshbuf_init(&req, 128, 512) == 0);
        assert(sshbuf_init(&reply, 128, 512) == 0);
        assert(sshbuf_init(&pkt, 128, 512) == 0);

        /* Open channel */
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
        assert(sshbuf_put_cstring(&req, "session") == 0);
        assert(sshbuf_put_u32(&req, 99) == 0);
        assert(sshbuf_put_u32(&req, 32768) == 0);
        assert(sshbuf_put_u32(&req, 16384) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

        /* Server sends exit-status 0 */
        assert(channel_send_exit_status(&mgr, 0, 0, &pkt) == 0);
        uint8_t type = 0;
        uint32_t rid = 0;
        const uint8_t *req_str = NULL; size_t req_len = 0;
        uint8_t want_rep = 0;
        uint32_t exit_val = 0;
        assert(sshbuf_get_u8(&pkt, &type) == 0);
        assert(type == SSH_MSG_CHANNEL_REQUEST);
        assert(sshbuf_get_u32(&pkt, &rid) == 0);
        assert(rid == 99);
        assert(sshbuf_get_string_ptr(&pkt, &req_str, &req_len) == 0);
        assert(req_len == 11 && memcmp(req_str, "exit-status", 11) == 0);
        assert(sshbuf_get_u8(&pkt, &want_rep) == 0);
        assert(want_rep == 0);
        assert(sshbuf_get_u32(&pkt, &exit_val) == 0);
        assert(exit_val == 0);

        /* Server sends EOF */
        sshbuf_reset(&pkt);
        assert(channel_send_eof(&mgr, 0, &pkt) == 0);
        assert(sshbuf_get_u8(&pkt, &type) == 0);
        assert(type == SSH_MSG_CHANNEL_EOF);

        ssh_channel_t *c = channel_lookup_local(&mgr, 0);
        assert(c->state == SSH_CHANNEL_STATE_EOF_LOCAL);

        /* Attempt to send data after sending EOF must be rejected */
        sshbuf_reset(&pkt);
        assert(channel_send_data(&mgr, 0, (const uint8_t *)"x", 1, &pkt) == SSH_ERR_INVALID_FORMAT);

        /* Client sends EOF */
        sshbuf_reset(&req);
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_EOF) == 0);
        assert(sshbuf_put_u32(&req, 0) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_EOF, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
        assert(c->state == SSH_CHANNEL_STATE_EOF_BOTH);

        /* Server sends CLOSE */
        sshbuf_reset(&pkt);
        assert(channel_send_close(&mgr, 0, &pkt) == 0);
        assert(sshbuf_get_u8(&pkt, &type) == 0);
        assert(type == SSH_MSG_CHANNEL_CLOSE);
        assert(c->state == SSH_CHANNEL_STATE_CLOSE_SENT);

        /* Client responds with CLOSE -> channel reaped */
        sshbuf_reset(&req);
        sshbuf_reset(&reply);
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_CLOSE) == 0);
        assert(sshbuf_put_u32(&req, 0) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_CLOSE, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);
        assert(sshbuf_len(&reply) == 0); /* We already sent CLOSE, no reply needed */
        assert(channel_lookup_local(&mgr, 0) == NULL); /* Channel reaped! */

        sshbuf_free(&req);
        sshbuf_free(&reply);
        sshbuf_free(&pkt);
        channel_mgr_cleanup(&mgr);
    }

    /* CASE B: Client initiates CLOSE first. Server replies with CLOSE and reaps. */
    {
        ssh_channel_mgr_t mgr;
        channel_mgr_init(&mgr);

        sshbuf_t req, reply;
        assert(sshbuf_init(&req, 128, 512) == 0);
        assert(sshbuf_init(&reply, 128, 512) == 0);

        /* Open channel */
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_OPEN) == 0);
        assert(sshbuf_put_cstring(&req, "session") == 0);
        assert(sshbuf_put_u32(&req, 101) == 0);
        assert(sshbuf_put_u32(&req, 32768) == 0);
        assert(sshbuf_put_u32(&req, 16384) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_OPEN, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

        /* Client sends CLOSE */
        sshbuf_reset(&req);
        sshbuf_reset(&reply);
        assert(sshbuf_put_u8(&req, SSH_MSG_CHANNEL_CLOSE) == 0);
        assert(sshbuf_put_u32(&req, 0) == 0);
        assert(channel_process_packet(&mgr, SSH_MSG_CHANNEL_CLOSE, sshbuf_ptr(&req), sshbuf_len(&req), &reply) == 0);

        /* Server must have replied with SSH_MSG_CHANNEL_CLOSE to remote_id 101 */
        uint8_t type = 0;
        uint32_t rid = 0;
        assert(sshbuf_get_u8(&reply, &type) == 0);
        assert(type == SSH_MSG_CHANNEL_CLOSE);
        assert(sshbuf_get_u32(&reply, &rid) == 0);
        assert(rid == 101);

        /* Channel must be immediately reaped */
        assert(channel_lookup_local(&mgr, 0) == NULL);

        sshbuf_free(&req);
        sshbuf_free(&reply);
        channel_mgr_cleanup(&mgr);
    }

    printf("PASS: test_eof_and_close_interleavings\n");
}

int main(void) {
    test_channel_open_and_confirmation();
    test_channel_open_rejections();
    test_channel_requests();
    test_data_transfer_and_window_credits();
    test_adversarial_channel_violations();
    test_eof_and_close_interleavings();

    printf("\nALL TESTS PASSED (tests/ssh_channel_host.c)\n");
    return 0;
}
