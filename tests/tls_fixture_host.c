#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TEST_HOST 1
#include "tls.h"
#include "syscall_abi.h"
#include "user/wget_codec.h"

/* Static memory allocations adhering to Gate G1 (zero dynamic heap allocation) */
static fortress_tls_t s_tls_session;
static fortress_trust_store_t s_trust_store;

static uint64_t now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int create_connected_socket(const char *ip_str, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip_str, &sin.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* Mock Syscall State for Cases 14 and 15 */
static bool s_mock_rand_fail = false;
static bool s_mock_clock_fail = false;

static long mock_case_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f)
{
    (void)c; (void)d; (void)e; (void)f;
    if (nr == SYS_CLOCK_GETTIME) {
        if (s_mock_clock_fail) {
            /* Inject uncalibrated/ancient epoch < 2026-01-01 (e.g. 1700000000) */
            timespec_t *ts = (timespec_t *)b;
            if (ts) {
                ts->tv_sec = 1700000000LL;
                ts->tv_nsec = 0;
            }
            return 0;
        }
        struct timespec hts;
        if (clock_gettime((clockid_t)a, &hts) != 0) return -1;
        timespec_t *ts = (timespec_t *)b;
        if (ts) {
            ts->tv_sec = hts.tv_sec;
            ts->tv_nsec = hts.tv_nsec;
        }
        return 0;
    }
    if (nr == SYS_GETRANDOM) {
        if (s_mock_rand_fail) {
            /* Inject short read (16 bytes instead of 32 bytes) */
            void *buf = (void *)a;
            memset(buf, 0x42, 16);
            return 16;
        }
        void *buf = (void *)a;
        size_t len = (size_t)b;
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) return -1;
        size_t r = fread(buf, 1, len, f);
        fclose(f);
        return (long)r;
    }
    return -38; /* -ENOSYS */
}

/* 1. Valid certificate handshake */
static void run_case1_valid(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(s_trust_store.count > 0);

    int ires = fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count);
    assert(ires == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_SUCCESS);

    const char *req = "GET / HTTP/1.0\r\nHost: test.fortress.local\r\n\r\n";
    int wres = fortress_tls_write_all(&s_tls_session, req, strlen(req), deadline);
    assert(wres == TLS_SUCCESS);
    assert(fortress_tls_flush(&s_tls_session, deadline) == TLS_SUCCESS);

    char buf[256];
    int rlen = fortress_tls_read(&s_tls_session, buf, sizeof(buf) - 1, deadline);
    assert(rlen > 0);
    buf[rlen] = '\0';
    assert(strstr(buf, "200 OK") != NULL);

    int cls = fortress_tls_close(&s_tls_session, deadline);
    assert(cls == TLS_SUCCESS);

    close(fd);
    printf("  [PASS] Case 1: Valid certificate handshake\n");
}

/* 2. Expired certificate rejection */
static void run_case2_expired(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_X509_EXPIRED);

    close(fd);
    printf("  [PASS] Case 2: Expired certificate rejection\n");
}

/* 3. Not-yet-valid certificate rejection */
static void run_case3_not_yet_valid(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_X509_EXPIRED);

    close(fd);
    printf("  [PASS] Case 3: Not-yet-valid certificate rejection\n");
}

/* 4. Self-signed certificate rejection */
static void run_case4_self_signed(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_X509_NOT_TRUSTED);

    close(fd);
    printf("  [PASS] Case 4: Self-signed certificate rejection\n");
}

/* 5. Wrong hostname / SAN mismatch rejection */
static void run_case5_wrong_hostname(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    /* Request test.fortress.local, but server provides certificate for wrong.fortress.local */
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_X509_BAD_SERVER_NAME ||
           s_tls_session.br_last_error == BR_ERR_X509_NOT_TRUSTED ||
           s_tls_session.br_last_error == BR_ERR_BAD_SNI);

    close(fd);
    printf("  [PASS] Case 5: Wrong hostname / SAN mismatch rejection\n");
}

/* 6. Sub-2048-bit RSA key rejection */
static void run_case6_sub_2048_rsa(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_BAD_SIGNATURE ||
           s_tls_session.br_last_error == BR_ERR_X509_NOT_TRUSTED);

    close(fd);
    printf("  [PASS] Case 6: Sub-2048-bit RSA key rejection\n");
}

/* 7. RSA-PSS signature rejection */
static void run_case7_rsa_pss_signature(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_X509_UNSUPPORTED ||
           s_tls_session.br_last_error == BR_ERR_BAD_SIGNATURE ||
           s_tls_session.br_last_error == BR_ERR_X509_NOT_TRUSTED);

    close(fd);
    printf("  [PASS] Case 7: RSA-PSS signature rejection\n");
}

/* 8. Unsupported cipher suite negotiation (static RSA / CBC rejection) */
static void run_case8_unsupported_cipher(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_ERR_HANDSHAKE_FAILED);
    assert(s_tls_session.br_last_error == BR_ERR_BAD_CIPHER_SUITE);

    close(fd);
    printf("  [PASS] Case 8: Unsupported cipher suite negotiation rejection\n");
}

/* 9. Corrupted record authentication tag */
static void run_case9_corrupted_tag(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_SUCCESS);

    const char *req = "GET /corrupted HTTP/1.0\r\nHost: test.fortress.local\r\n\r\n";
    int wres = fortress_tls_write_all(&s_tls_session, req, strlen(req), deadline);
    assert(wres == TLS_SUCCESS);
    assert(fortress_tls_flush(&s_tls_session, deadline) == TLS_SUCCESS);

    char buf[256];
    int rlen = fortress_tls_read(&s_tls_session, buf, sizeof(buf), deadline);
    assert(rlen < 0);
    assert(s_tls_session.br_last_error == BR_ERR_BAD_MAC);

    close(fd);
    printf("  [PASS] Case 9: Corrupted record authentication tag rejection (BR_ERR_BAD_MAC)\n");
}

/* 10. Malformed chunked transfer encoding framing */
static void run_case10_malformed_chunked(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_SUCCESS);

    const char *req = "GET /chunked HTTP/1.0\r\nHost: test.fortress.local\r\n\r\n";
    assert(fortress_tls_write_all(&s_tls_session, req, strlen(req), deadline) == TLS_SUCCESS);
    assert(fortress_tls_flush(&s_tls_session, deadline) == TLS_SUCCESS);

    uint8_t raw_resp[512];
    int rlen = fortress_tls_read(&s_tls_session, raw_resp, sizeof(raw_resp) - 1, deadline);
    assert(rlen > 0);
    raw_resp[rlen] = '\0';

    size_t header_len = 0;
    assert(wget_find_header_end(raw_resp, (size_t)rlen, &header_len) == WGET_OK);

    wget_response_t resp;
    assert(wget_parse_response_headers(raw_resp, header_len, &resp) == WGET_OK);
    assert(resp.is_chunked == true);

    /* Feed remaining body bytes into chunked decoder */
    wget_chunked_decoder_t dec;
    wget_chunked_decoder_init(&dec);

    uint8_t out[256];
    size_t in_c = 0, out_w = 0;
    size_t body_len = (size_t)rlen - header_len;
    int dec_res = wget_decode_chunked(&dec, raw_resp + header_len, body_len, &in_c, out, sizeof(out), &out_w);
    assert(dec_res == WGET_ERR_MALFORMED_CHUNK);

    fortress_tls_close(&s_tls_session, deadline);
    close(fd);
    printf("  [PASS] Case 10: Malformed chunked transfer encoding framing rejected\n");
}

/* 11. Slow-loris 1-byte fragmented handshake timeout (asserting non-restarting deadline) */
static void run_case11_slow_loris_timeout(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    /* Set deadline 600 ms from now; server trickles 1 byte every 300 ms */
    uint64_t t0 = now_monotonic_ms();
    uint64_t deadline = t0 + 600;

    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    uint64_t elapsed = now_monotonic_ms() - t0;

    assert(cres == TLS_ERR_TIMEOUT);
    assert(s_tls_session.last_error == TLS_ERR_TIMEOUT);
    /* Verify timeout fired between 550ms and 1200ms without restarting on bytes received */
    assert(elapsed >= 550 && elapsed <= 1200);

    close(fd);
    printf("  [PASS] Case 11: Slow-loris 1-byte fragmented handshake timeout (non-restarting deadline)\n");
}

/* 12. Abrupt TCP drop without close_notify */
static void run_case12_abrupt_drop(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_SUCCESS);

    const char *req = "GET /drop HTTP/1.0\r\nHost: test.fortress.local\r\n\r\n";
    assert(fortress_tls_write_all(&s_tls_session, req, strlen(req), deadline) == TLS_SUCCESS);
    assert(fortress_tls_flush(&s_tls_session, deadline) == TLS_SUCCESS);

    char buf[256];
    int rlen = fortress_tls_read(&s_tls_session, buf, sizeof(buf) - 1, deadline);
    assert(rlen > 0);

    /* Server closed TCP immediately without sending close_notify alert */
    int cls = fortress_tls_close(&s_tls_session, deadline);
    assert(cls == TLS_ERR_TRUNCATED);

    close(fd);
    printf("  [PASS] Case 12: Abrupt TCP drop without close_notify (TLS_ERR_TRUNCATED)\n");
}

/* 13. Content-Length truncation */
static void run_case13_content_length_truncation(int port, const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    int fd = create_connected_socket("127.0.0.1", port);
    assert(fd >= 0);

    uint64_t deadline = now_monotonic_ms() + 5000;
    int cres = fortress_tls_connect(&s_tls_session, fd, "test.fortress.local", deadline);
    assert(cres == TLS_SUCCESS);

    const char *req = "GET /truncate HTTP/1.0\r\nHost: test.fortress.local\r\n\r\n";
    assert(fortress_tls_write_all(&s_tls_session, req, strlen(req), deadline) == TLS_SUCCESS);
    assert(fortress_tls_flush(&s_tls_session, deadline) == TLS_SUCCESS);

    uint8_t raw_resp[512];
    int rlen = fortress_tls_read(&s_tls_session, raw_resp, sizeof(raw_resp) - 1, deadline);
    assert(rlen > 0);
    raw_resp[rlen] = '\0';

    size_t header_len = 0;
    assert(wget_find_header_end(raw_resp, (size_t)rlen, &header_len) == WGET_OK);

    wget_response_t resp;
    assert(wget_parse_response_headers(raw_resp, header_len, &resp) == WGET_OK);
    assert(resp.has_content_length == true);
    assert(resp.content_length == 100);

    /* Body received is only 14 bytes while Content-Length is 100 */
    size_t body_len = (size_t)rlen - header_len;
    assert(body_len < 100);

    /* Read again to reach EOF / peer closure */
    int r2 = fortress_tls_read(&s_tls_session, raw_resp, sizeof(raw_resp), deadline);
    (void)r2;

    int val_res = wget_validate_content_length(resp.content_length, (uint64_t)body_len, true);
    assert(val_res == WGET_ERR_TRUNCATED);

    fortress_tls_close(&s_tls_session, deadline);
    close(fd);
    printf("  [PASS] Case 13: Content-Length truncation rejected\n");
}

/* 14. Entropy failure / short read injection */
static void run_case14_entropy_failure(const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    s_mock_rand_fail = true;
    fortress_tls_set_syscall_hook(mock_case_syscall);

    int cres = fortress_tls_connect(&s_tls_session, 42, "test.fortress.local", 1000);
    assert(cres == TLS_ERR_ENTROPY_FAILURE);
    assert(s_tls_session.last_error == TLS_ERR_ENTROPY_FAILURE);

    s_mock_rand_fail = false;
    fortress_tls_set_syscall_hook(NULL);
    printf("  [PASS] Case 14: Entropy failure / short read injection (TLS_ERR_ENTROPY_FAILURE)\n");
}

/* 15. Invalid / uncalibrated clock rejection (epoch < 2026-01-01) */
static void run_case15_invalid_clock(const char *anchor_path) {
    int lres = fortress_tls_load_anchors(anchor_path, &s_trust_store);
    assert(lres == TLS_SUCCESS);
    assert(fortress_tls_client_init(&s_tls_session, s_trust_store.anchors, s_trust_store.count) == TLS_SUCCESS);

    s_mock_clock_fail = true;
    fortress_tls_set_syscall_hook(mock_case_syscall);

    int cres = fortress_tls_connect(&s_tls_session, 42, "test.fortress.local", 1000);
    assert(cres == TLS_ERR_CLOCK_UNSYNCHRONIZED);
    assert(s_tls_session.last_error == TLS_ERR_CLOCK_UNSYNCHRONIZED);

    s_mock_clock_fail = false;
    fortress_tls_set_syscall_hook(NULL);
    printf("  [PASS] Case 15: Invalid / uncalibrated clock rejection (TLS_ERR_CLOCK_UNSYNCHRONIZED)\n");
}

/* 16. Missing / malformed anchors.v1 fail-closed test */
static void run_case16_missing_malformed_anchors(void) {
    /* 1. Missing file */
    int res = fortress_tls_load_anchors("/nonexistent/file/anchors.v1", &s_trust_store);
    assert(res == TLS_ERR_ANCHOR_FILE);

    /* 2. Malformed magic */
    uint8_t bad_magic[64];
    memset(bad_magic, 0, sizeof(bad_magic));
    memcpy(bad_magic, "BADMAGIC", 8);
    res = fortress_tls_parse_anchors(bad_magic, sizeof(bad_magic), &s_trust_store);
    assert(res == TLS_ERR_ANCHOR_MAGIC);

    /* 3. Anchor count = 0 */
    uint8_t zero_count[64];
    memset(zero_count, 0, sizeof(zero_count));
    memcpy(zero_count, TLS_ANCHOR_MAGIC, 8);
    uint32_t ver = 1;
    memcpy(zero_count + 8, &ver, 4);
    uint32_t cnt0 = 0;
    memcpy(zero_count + 12, &cnt0, 4);
    res = fortress_tls_parse_anchors(zero_count, sizeof(zero_count), &s_trust_store);
    assert(res == TLS_ERR_ANCHOR_BOUNDS);

    /* 4. Anchor count > 256 */
    uint32_t cnt257 = 257;
    memcpy(zero_count + 12, &cnt257, 4);
    res = fortress_tls_parse_anchors(zero_count, sizeof(zero_count), &s_trust_store);
    assert(res == TLS_ERR_ANCHOR_BOUNDS);

    /* 5. Descriptor offset out of bounds */
    uint8_t bad_offset[128];
    memset(bad_offset, 0, sizeof(bad_offset));
    memcpy(bad_offset, TLS_ANCHOR_MAGIC, 8);
    memcpy(bad_offset + 8, &ver, 4);
    uint32_t cnt1 = 1;
    memcpy(bad_offset + 12, &cnt1, 4);
    fortress_anchor_desc_t *d = (fortress_anchor_desc_t *)(bad_offset + 16);
    d->dn_offset = 50000; /* Way beyond file size */
    d->dn_len = 10;
    d->key_type = BR_KEYTYPE_RSA;
    res = fortress_tls_parse_anchors(bad_offset, sizeof(bad_offset), &s_trust_store);
    assert(res == TLS_ERR_ANCHOR_FORMAT || res == TLS_ERR_ANCHOR_BOUNDS);

    printf("  [PASS] Case 16: Missing / malformed anchors.v1 fail-closed test\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <case_name> [port] [anchor_path]\n", argv[0]);
        return 1;
    }

    const char *case_name = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : 0;
    const char *anchor_path = (argc >= 4) ? argv[3] : "test_anchors.v1";

    if (strcmp(case_name, "case1_valid") == 0) {
        run_case1_valid(port, anchor_path);
    } else if (strcmp(case_name, "case2_expired") == 0) {
        run_case2_expired(port, anchor_path);
    } else if (strcmp(case_name, "case3_not_yet_valid") == 0) {
        run_case3_not_yet_valid(port, anchor_path);
    } else if (strcmp(case_name, "case4_self_signed") == 0) {
        run_case4_self_signed(port, anchor_path);
    } else if (strcmp(case_name, "case5_wrong_hostname") == 0) {
        run_case5_wrong_hostname(port, anchor_path);
    } else if (strcmp(case_name, "case6_sub_2048_rsa") == 0) {
        run_case6_sub_2048_rsa(port, anchor_path);
    } else if (strcmp(case_name, "case7_rsa_pss") == 0) {
        run_case7_rsa_pss_signature(port, anchor_path);
    } else if (strcmp(case_name, "case8_unsupported_cipher") == 0) {
        run_case8_unsupported_cipher(port, anchor_path);
    } else if (strcmp(case_name, "case9_corrupted_tag") == 0) {
        run_case9_corrupted_tag(port, anchor_path);
    } else if (strcmp(case_name, "case10_malformed_chunked") == 0) {
        run_case10_malformed_chunked(port, anchor_path);
    } else if (strcmp(case_name, "case11_slow_loris") == 0) {
        run_case11_slow_loris_timeout(port, anchor_path);
    } else if (strcmp(case_name, "case12_abrupt_drop") == 0) {
        run_case12_abrupt_drop(port, anchor_path);
    } else if (strcmp(case_name, "case13_content_length_truncation") == 0) {
        run_case13_content_length_truncation(port, anchor_path);
    } else if (strcmp(case_name, "case14_entropy_failure") == 0) {
        run_case14_entropy_failure(anchor_path);
    } else if (strcmp(case_name, "case15_invalid_clock") == 0) {
        run_case15_invalid_clock(anchor_path);
    } else if (strcmp(case_name, "case16_missing_malformed_anchors") == 0) {
        run_case16_missing_malformed_anchors();
    } else {
        fprintf(stderr, "Unknown test case: %s\n", case_name);
        return 2;
    }

    return 0;
}
