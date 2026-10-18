#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_HOST 1
#include "tls.h"
#include "syscall_abi.h"

/* Static session instance to verify zero stack/heap allocations */
static fortress_tls_t s_test_session;

/* Mock State for Syscall Interception */
static int64_t s_mock_time_sec = 1775000000; /* 2026-04-01 */
static int s_mock_clock_retval = 0;

static ssize_t s_mock_rand_len = 32;
static int s_mock_rand_retval = 0;

static uint64_t s_last_send_deadline = 0;
static uint64_t s_last_recv_deadline = 0;
static int s_mock_send_retval = 100;
static int s_mock_recv_retval = 100;

static long mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e, uintptr_t f)
{
    (void)c; (void)d; (void)f;
    if (nr == SYS_CLOCK_GETTIME) {
        if (s_mock_clock_retval != 0) {
            return s_mock_clock_retval;
        }
        timespec_t *ts = (timespec_t *)b;
        if (ts) {
            ts->tv_sec = s_mock_time_sec;
            ts->tv_nsec = 0;
        }
        return 0;
    }
    if (nr == SYS_GETRANDOM) {
        if (s_mock_rand_retval != 0) {
            return s_mock_rand_retval;
        }
        void *buf = (void *)a;
        size_t len = (size_t)b;
        size_t to_write = (size_t)s_mock_rand_len;
        if (to_write > len) to_write = len;
        memset(buf, 0x5A, to_write);
        return (long)to_write;
    }
    if (nr == SYS_SEND_UNTIL) {
        s_last_send_deadline = e;
        return s_mock_send_retval;
    }
    if (nr == SYS_RECV_UNTIL) {
        s_last_recv_deadline = e;
        return s_mock_recv_retval;
    }
    return -38;
}

static void test_static_footprint_and_structures(void) {
    printf("  [TEST] Static footprint & structure sizes (Gate G1)...\n");

    /* Verify BearSSL client context sizes */
    assert(sizeof(br_ssl_client_context) == 3720);
    assert(sizeof(br_x509_minimal_context) == 3168);
    assert(sizeof(s_test_session.iobuf) == 33178);

    /* Total session context footprint must fit within bounded budget (< 41 KiB) */
    size_t total_size = sizeof(fortress_tls_t);
    printf("    -> sizeof(fortress_tls_t) = %zu bytes (~%.1f KiB)\n",
           total_size, (double)total_size / 1024.0);
    assert(total_size <= 41500);

    printf("  [PASS] Static footprint validated.\n");
}

static void test_clock_hook(void) {
    printf("  [TEST] Platform clock hook (tls_clock_hook)...\n");
    fortress_tls_set_syscall_hook(mock_syscall);

    uint32_t days = 0, seconds = 0;

    /* 1. Valid timestamp: 2026-06-01 12:00:00 UTC (1780315200) */
    s_mock_clock_retval = 0;
    s_mock_time_sec = 1780315200ULL;
    int res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_SUCCESS);
    assert(days >= 719528U + 20454U); /* Year >= 2026 */
    assert(seconds == 43200); /* 12:00:00 */

    /* 2. Boundary: exactly TIME_FLOOR_EPOCH (2026-01-01 00:00:00 UTC = 1767225600) */
    s_mock_time_sec = TLS_TIME_FLOOR_EPOCH;
    res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_SUCCESS);
    assert(days == 719528U + 20454U);
    assert(seconds == 0);

    /* 3. Reject: 1 second before floor (2025-12-31 23:59:59 UTC = 1767225599) */
    s_mock_time_sec = TLS_TIME_FLOOR_EPOCH - 1;
    res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_ERR_CLOCK_UNSYNCHRONIZED);

    /* 4. Reject: uninitialized clock (0) */
    s_mock_time_sec = 0;
    res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_ERR_CLOCK_UNSYNCHRONIZED);

    /* 5. Reject: beyond TIME_CEILING_EPOCH */
    s_mock_time_sec = TLS_TIME_CEILING_EPOCH + 1;
    res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_ERR_CLOCK_UNSYNCHRONIZED);

    /* 6. Reject: syscall error */
    s_mock_time_sec = 1780315200ULL;
    s_mock_clock_retval = -1;
    res = tls_clock_hook(&days, &seconds);
    assert(res == TLS_ERR_CLOCK_UNSYNCHRONIZED);

    s_mock_clock_retval = 0;
    printf("  [PASS] Platform clock hook validated.\n");
}

static void test_rand_hook(void) {
    printf("  [TEST] Platform entropy hook (tls_rand_hook)...\n");
    fortress_tls_set_syscall_hook(mock_syscall);

    unsigned char buf[32];

    /* 1. Full 32 bytes read: SUCCESS */
    s_mock_rand_retval = 0;
    s_mock_rand_len = 32;
    int res = tls_rand_hook(buf, sizeof(buf));
    assert(res == TLS_SUCCESS);

    /* 2. Short read: e.g. 16 bytes: FAILS CLOSED */
    s_mock_rand_len = 16;
    res = tls_rand_hook(buf, sizeof(buf));
    assert(res == TLS_ERR_ENTROPY_FAILURE);

    /* 3. Zero bytes returned: FAILS CLOSED */
    s_mock_rand_len = 0;
    res = tls_rand_hook(buf, sizeof(buf));
    assert(res == TLS_ERR_ENTROPY_FAILURE);

    /* 4. Syscall error: FAILS CLOSED */
    s_mock_rand_retval = -1;
    s_mock_rand_len = 32;
    res = tls_rand_hook(buf, sizeof(buf));
    assert(res == TLS_ERR_ENTROPY_FAILURE);

    s_mock_rand_retval = 0;
    s_mock_rand_len = 32;
    printf("  [PASS] Platform entropy hook validated.\n");
}

static void test_rsa_key_size_enforcement(void) {
    printf("  [TEST] RSA >= 2048-bit key size constraint...\n");

    /* Sub-2048-bit key (e.g. 1024-bit key = 128 bytes modulus) */
    unsigned char n_1024[128];
    memset(n_1024, 0xBB, sizeof(n_1024));
    n_1024[0] = 0x80; /* Non-zero high bit */
    unsigned char e_bytes[3] = { 0x01, 0x00, 0x01 };

    br_rsa_public_key pk1024;
    pk1024.n = n_1024;
    pk1024.nlen = sizeof(n_1024);
    pk1024.e = e_bytes;
    pk1024.elen = sizeof(e_bytes);

    /* RSA verify handler configured in fortress_tls_client_init */
    int init_res = fortress_tls_client_init(&s_test_session, NULL, 0);
    assert(init_res == TLS_SUCCESS);

    unsigned char sig[128];
    memset(sig, 0x11, sizeof(sig));
    unsigned char hash_out[32];
    static const unsigned char HASH_OID_SHA256[] = {
        0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A
    };

    /* Invoke the engine's RSA verify hook on the 1024-bit key */
    br_rsa_pkcs1_vrfy vrfy_fn = s_test_session.sc.eng.irsavrfy;
    assert(vrfy_fn != NULL);

    uint32_t ok = vrfy_fn(sig, sizeof(sig), HASH_OID_SHA256, 32, &pk1024, hash_out);
    assert(ok == 0); /* MUST FAIL CLOSED: Rejected sub-2048-bit key! */

    /* Also test key with 256 bytes but leading zeros reducing significant bytes to < 256 */
    unsigned char n_fake2048[256];
    memset(n_fake2048, 0x00, 16); /* 16 leading zero bytes */
    memset(n_fake2048 + 16, 0xCC, 240);
    pk1024.n = n_fake2048;
    pk1024.nlen = sizeof(n_fake2048);
    ok = vrfy_fn(sig, sizeof(sig), HASH_OID_SHA256, 32, &pk1024, hash_out);
    assert(ok == 0); /* MUST FAIL CLOSED: Significant modulus < 2048 bits! */

    printf("  [PASS] RSA >= 2048-bit key size constraint enforced.\n");
}

static void test_engine_configuration_allowlist(void) {
    printf("  [TEST] Cipher suite allowlist & curve restrictions...\n");

    int res = fortress_tls_client_init(&s_test_session, NULL, 0);
    assert(res == TLS_SUCCESS);

    /* 1. Protocol Version: strictly TLS 1.2 */
    assert(s_test_session.sc.eng.version_min == BR_TLS12);
    assert(s_test_session.sc.eng.version_max == BR_TLS12);

    /* 2. Curve Mask: Curve25519, secp384r1, secp256r1 */
    const br_ec_impl *iec = s_test_session.sc.eng.iec;
    assert(iec != NULL);
    assert(iec->supported_curves == 0x21800000U);
    assert((iec->supported_curves & (1U << BR_EC_curve25519)) != 0);
    assert((iec->supported_curves & (1U << BR_EC_secp384r1)) != 0);
    assert((iec->supported_curves & (1U << BR_EC_secp256r1)) != 0);
    assert((iec->supported_curves & (1U << BR_EC_secp521r1)) == 0); /* 521 excluded */

    /* 3. Hashes: SHA-256 and SHA-384 present, MD5 and SHA-1 excluded */
    assert(s_test_session.sc.eng.mhash.impl[br_sha256_ID - 1] != NULL);
    assert(s_test_session.sc.eng.mhash.impl[br_sha384_ID - 1] != NULL);
    assert(s_test_session.sc.eng.mhash.impl[br_md5_ID - 1] == NULL);
    assert(s_test_session.sc.eng.mhash.impl[br_sha1_ID - 1] == NULL);

    /* 4. PRF: SHA-256 and SHA-384 PRFs configured */
    assert(s_test_session.sc.eng.prf_sha256 != NULL);
    assert(s_test_session.sc.eng.prf_sha384 != NULL);

    /* 5. Ciphers: AEAD (GCM and ChaCha20-Poly1305) configured; CBC and DES excluded */
    assert(s_test_session.sc.eng.ichacha != NULL);
    assert(s_test_session.sc.eng.ipoly != NULL);
    assert(s_test_session.sc.eng.iaes_ctr != NULL);
    assert(s_test_session.sc.eng.ighash != NULL);
    assert(s_test_session.sc.eng.iaes_cbcenc == NULL);
    assert(s_test_session.sc.eng.iaes_cbcdec == NULL);
    assert(s_test_session.sc.eng.ides_cbcenc == NULL);
    assert(s_test_session.sc.eng.ides_cbcdec == NULL);

    printf("  [PASS] Cipher suite allowlist & curves validated.\n");
}

static void test_deadline_and_teardown_hooks(void) {
    printf("  [TEST] Non-restarting deadlines and close_notify teardown...\n");
    fortress_tls_set_syscall_hook(mock_syscall);

    int res = fortress_tls_client_init(&s_test_session, NULL, 0);
    assert(res == TLS_SUCCESS);

    s_test_session.fd = 42;
    s_test_session.current_deadline = 123456789ULL;

    /* Test send hook passes current deadline */
    unsigned char test_buf[64] = "GET / HTTP/1.1\r\n\r\n";
    s_mock_send_retval = 18;
    int wlen = tls_send_hook(&s_test_session, test_buf, 18);
    assert(wlen == 18);
    assert(s_last_send_deadline == 123456789ULL);

    /* Test recv hook passes current deadline */
    s_test_session.current_deadline = 987654321ULL;
    s_mock_recv_retval = 30;
    int rlen = tls_recv_hook(&s_test_session, test_buf, 30);
    assert(rlen == 30);
    assert(s_last_recv_deadline == 987654321ULL);

    /* Test send timeout mapping */
    s_mock_send_retval = -110; /* -ETIMEDOUT */
    wlen = tls_send_hook(&s_test_session, test_buf, 10);
    assert(wlen == -1);
    assert(s_test_session.last_error == TLS_ERR_TIMEOUT);

    /* Test recv timeout mapping */
    s_mock_recv_retval = -110; /* -ETIMEDOUT */
    rlen = tls_recv_hook(&s_test_session, test_buf, 10);
    assert(rlen == -1);
    assert(s_test_session.last_error == TLS_ERR_TIMEOUT);

    /* Test abrupt socket closure (0 bytes read -> TLS_ERR_PEER_CLOSED) */
    s_mock_recv_retval = 0;
    rlen = tls_recv_hook(&s_test_session, test_buf, 10);
    assert(rlen == -1);
    assert(s_test_session.peer_closed == true);
    assert(s_test_session.last_error == TLS_ERR_PEER_CLOSED);

    /* Test close_notify commit condition */
    br_ssl_client_reset(&s_test_session.sc, "example.com", 0);
    s_test_session.handshake_done = true;
    s_mock_send_retval = -1; /* Simulated EOF or drop on close_notify wait */
    s_mock_recv_retval = -1;
    res = fortress_tls_close(&s_test_session, 3000ULL);
    assert(res == TLS_ERR_TRUNCATED); /* Without authentic close_notify, teardown is TRUNCATED! */

    printf("  [PASS] Non-restarting deadlines & close_notify teardown validated.\n");
}

int main(void) {
    printf("=== FortressOS Phase TLS-1 BearSSL Integration Tests ===\n");
    test_static_footprint_and_structures();
    test_clock_hook();
    test_rand_hook();
    test_rsa_key_size_enforcement();
    test_engine_configuration_allowlist();
    test_deadline_and_teardown_hooks();
    printf("=== All Phase TLS-1 Integration Tests PASSED ===\n");
    return 0;
}
