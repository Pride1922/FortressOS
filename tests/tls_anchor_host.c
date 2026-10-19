/*
 * tests/tls_anchor_host.c
 * Host test suite for Phase TLS-2 Trust Store & Runtime Anchor Parser.
 * Audited under AddressSanitizer and UndefinedBehaviorSanitizer (ASan/UBSan).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>

#include "tls.h"
#include "tls_internal.h"
#include "syscall_abi.h"

static fortress_trust_store_t s_test_store;

/* Helper to build a valid synthetic anchor binary */
static size_t build_synthetic_anchor(uint8_t *buf, size_t buf_cap,
                                    int key_type, int rsa_bits, uint32_t rsa_e,
                                    int ec_curve, size_t ec_qlen, uint8_t ec_prefix)
{
    memset(buf, 0, buf_cap);

    /* Header: 16 bytes */
    memcpy(buf, TLS_ANCHOR_MAGIC, 8);
    uint32_t version = TLS_ANCHOR_VERSION;
    uint32_t count = 1;
    memcpy(buf + 8, &version, 4);
    memcpy(buf + 12, &count, 4);

    uint32_t table_offset = 16;
    uint32_t payload_offset = 16 + 32;

    /* Synthetic DN */
    const uint8_t mock_dn[] = { 0x30, 0x12, 0x31, 0x10, 0x30, 0x0e, 0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x07, 'T', 'e', 's', 't', ' ', 'C', 'A' };
    uint32_t dn_len = sizeof(mock_dn);
    uint32_t dn_off = payload_offset;
    memcpy(buf + dn_off, mock_dn, dn_len);

    uint32_t curr_off = dn_off + dn_len;

    fortress_anchor_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.dn_offset = dn_off;
    desc.dn_len = dn_len;
    desc.flags = 1; /* CA */
    desc.key_type = key_type;

    if (key_type == BR_KEYTYPE_RSA) {
        size_t n_len = rsa_bits / 8;
        uint8_t *n_ptr = buf + curr_off;
        memset(n_ptr, 0xcc, n_len);
        n_ptr[0] = 0x81; /* Ensure high bit is set for exact bit length */

        desc.key.rsa.n_offset = curr_off;
        desc.key.rsa.n_len = n_len;
        curr_off += n_len;

        uint8_t e_buf[4];
        size_t e_len = 0;
        if (rsa_e > 0) {
            if (rsa_e <= 0xff) {
                e_buf[0] = (uint8_t)rsa_e;
                e_len = 1;
            } else if (rsa_e <= 0xffff) {
                e_buf[0] = (uint8_t)(rsa_e >> 8);
                e_buf[1] = (uint8_t)rsa_e;
                e_len = 2;
            } else {
                e_buf[0] = (uint8_t)(rsa_e >> 16);
                e_buf[1] = (uint8_t)(rsa_e >> 8);
                e_buf[2] = (uint8_t)rsa_e;
                e_len = 3;
            }
        }
        memcpy(buf + curr_off, e_buf, e_len);
        desc.key.rsa.e_offset = curr_off;
        desc.key.rsa.e_len = e_len;
        curr_off += e_len;
    } else {
        desc.key.ec.curve_id = ec_curve;
        desc.key.ec.q_offset = curr_off;
        desc.key.ec.q_len = ec_qlen;

        uint8_t *q_ptr = buf + curr_off;
        memset(q_ptr, 0x11, ec_qlen);
        if (ec_qlen > 0) {
            q_ptr[0] = ec_prefix;
        }
        curr_off += ec_qlen;
    }

    memcpy(buf + table_offset, &desc, sizeof(desc));
    return curr_off;
}

/* ========================================================================= */
/* Test 1: Full Valid Store Parsing from compiled anchors.v1                  */
/* ========================================================================= */
static void test_valid_store_parsing(const char *path) {
    printf("  [TEST] Valid anchors.v1 bundle parsing...\n");
    int rc = fortress_tls_load_anchors(path, &s_test_store);
    assert(rc == TLS_SUCCESS);
    assert(s_test_store.count > 0);
    assert(s_test_store.count <= TLS_MAX_ANCHORS);
    printf("    -> Successfully parsed %zu root trust anchors from %s\n",
           s_test_store.count, path);

    /* Verify each parsed anchor descriptor */
    size_t rsa_count = 0;
    size_t ec_count = 0;
    for (size_t i = 0; i < s_test_store.count; i++) {
        const br_x509_trust_anchor *ta = &s_test_store.anchors[i];
        assert(ta->dn.data != NULL);
        assert(ta->dn.len > 0 && ta->dn.len <= TLS_MAX_DN_LEN);
        assert((ta->flags & BR_X509_TA_CA) != 0);

        if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
            rsa_count++;
            assert(ta->pkey.key.rsa.n != NULL);
            assert(ta->pkey.key.rsa.nlen >= TLS_MIN_RSA_MODULUS_LEN);
            assert(ta->pkey.key.rsa.nlen <= TLS_MAX_RSA_MODULUS_LEN);
            assert(ta->pkey.key.rsa.e != NULL);
            assert(ta->pkey.key.rsa.elen >= 1 && ta->pkey.key.rsa.elen <= TLS_MAX_RSA_EXPONENT_LEN);
        } else if (ta->pkey.key_type == BR_KEYTYPE_EC) {
            ec_count++;
            assert(ta->pkey.key.ec.q != NULL);
            assert(ta->pkey.key.ec.curve == BR_EC_secp256r1 ||
                   ta->pkey.key.ec.curve == BR_EC_secp384r1 ||
                   ta->pkey.key.ec.curve == BR_EC_curve25519);
            if (ta->pkey.key.ec.curve == BR_EC_secp256r1) {
                assert(ta->pkey.key.ec.qlen == 65);
                assert(ta->pkey.key.ec.q[0] == 0x04);
            } else if (ta->pkey.key.ec.curve == BR_EC_secp384r1) {
                assert(ta->pkey.key.ec.qlen == 97);
                assert(ta->pkey.key.ec.q[0] == 0x04);
            } else if (ta->pkey.key.ec.curve == BR_EC_curve25519) {
                assert(ta->pkey.key.ec.qlen == 32);
            }
        } else {
            assert(false && "Unexpected key type in trust anchor");
        }
    }
    printf("    -> Validated anchor breakdown: %zu RSA, %zu EC.\n", rsa_count, ec_count);
    printf("  [PASS] Valid anchors bundle validated.\n");
}

/* ========================================================================= */
/* Test 2: File Bounds & Size Limit Enforcement                             */
/* ========================================================================= */
static void test_bounds_and_size_limits(void) {
    printf("  [TEST] File bounds & size limit enforcement (Gate G1)...\n");
    uint8_t buf[2048];
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);

    /* 1. NULL pointer checks */
    assert(fortress_tls_parse_anchors(NULL, sz, &s_test_store) == TLS_ERR_INVALID_PARAM);
    assert(fortress_tls_parse_anchors(buf, sz, NULL) == TLS_ERR_INVALID_PARAM);

    /* 2. Truncated header (< 16 bytes) */
    assert(fortress_tls_parse_anchors(buf, 15, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);
    assert(fortress_tls_parse_anchors(buf, 0, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    /* 3. Exceeding 256 KiB file cap */
    assert(fortress_tls_parse_anchors(buf, TLS_MAX_ANCHOR_FILE_SIZE + 1, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    /* 4. Truncated descriptor table */
    assert(fortress_tls_parse_anchors(buf, 16 + 31, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    printf("  [PASS] Bounds and size limits enforced.\n");
}

/* ========================================================================= */
/* Test 3: Magic & Version Enforcement                                      */
/* ========================================================================= */
static void test_magic_and_version(void) {
    printf("  [TEST] Magic & Version enforcement...\n");
    uint8_t buf[2048];
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);

    /* Bad magic */
    buf[0] = 'X';
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_MAGIC);
    buf[0] = 'F';

    /* Bad version */
    uint32_t bad_version = 2;
    memcpy(buf + 8, &bad_version, 4);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_MAGIC);

    bad_version = 0;
    memcpy(buf + 8, &bad_version, 4);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_MAGIC);

    printf("  [PASS] Magic & Version enforced.\n");
}

/* ========================================================================= */
/* Test 4: Anchor Count Invariants                                          */
/* ========================================================================= */
static void test_anchor_count_limits(void) {
    printf("  [TEST] Anchor count limits (1 <= count <= 256)...\n");
    uint8_t buf[2048];
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);

    /* Zero anchors */
    uint32_t zero_count = 0;
    memcpy(buf + 12, &zero_count, 4);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    /* Too many anchors (> 256) */
    uint32_t too_many = 257;
    memcpy(buf + 12, &too_many, 4);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    /* Count claimed exceeds actual file buffer */
    uint32_t fake_count = 10;
    memcpy(buf + 12, &fake_count, 4);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);

    printf("  [PASS] Anchor count limits enforced.\n");
}

/* ========================================================================= */
/* Test 5: Descriptor Table & Offset Boundaries                             */
/* ========================================================================= */
static void test_descriptor_and_offsets(void) {
    printf("  [TEST] Descriptor fields & offset boundaries...\n");
    uint8_t buf[2048];
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);

    fortress_anchor_desc_t *desc = (fortress_anchor_desc_t *)(buf + 16);

    /* DN length 0 */
    desc->dn_len = 0;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);
    desc->dn_len = 20;

    /* DN length > 1024 */
    desc->dn_len = 1025;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_BOUNDS);
    desc->dn_len = 20;

    /* DN offset pointing inside header */
    desc->dn_offset = 8;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_FORMAT);
    desc->dn_offset = 16 + 32;

    /* DN offset + len overflowing file size */
    desc->dn_offset = (uint32_t)sz - 5;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_FORMAT);
    desc->dn_offset = 16 + 32;

    /* Invalid flags */
    desc->flags = 2;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_FORMAT);
    desc->flags = 1;

    /* Invalid key_type */
    desc->key_type = 99;
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_FORMAT);
    desc->key_type = BR_KEYTYPE_RSA;

    printf("  [PASS] Descriptor fields and offset bounds validated.\n");
}

/* ========================================================================= */
/* Test 6: RSA Modulus and Exponent Constraint Verification                 */
/* ========================================================================= */
static void test_rsa_key_constraints(void) {
    printf("  [TEST] RSA key constraints (modulus >= 2048 bits, odd e >= 3)...\n");
    uint8_t buf[2048];

    /* 1. Sub-2048 bit modulus (1024 bits / 128 bytes) rejected */
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 1024, 65537, 0, 0, 0);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 2. 256-byte modulus with leading zeroes yielding < 2048 significant bits */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);
    fortress_anchor_desc_t *desc = (fortress_anchor_desc_t *)(buf + 16);
    uint8_t *n_ptr = buf + desc->key.rsa.n_offset;
    n_ptr[0] = 0x00; /* Zero top byte -> 2040 significant bits (< 2048) */
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);
    n_ptr[0] = 0x80; /* Restore >= 2048 bits */
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_SUCCESS);

    /* 3. Even public exponent (e = 65536) rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 65536, 0, 0, 0);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 4. Exponent e = 1 rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 1, 0, 0, 0);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 5. Exponent e = 0 rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 0, 0, 0, 0);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 6. Odd exponent e = 3 accepted */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_RSA, 2048, 3, 0, 0, 0);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_SUCCESS);

    printf("  [PASS] RSA modulus and exponent constraints enforced.\n");
}

/* ========================================================================= */
/* Test 7: EC Curve & Coordinate Verification                               */
/* ========================================================================= */
static void test_ec_curve_constraints(void) {
    printf("  [TEST] EC curve & point constraints (secp256r1/384r1/curve25519)...\n");
    uint8_t buf[2048];

    /* 1. Valid secp256r1 accepted */
    size_t sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp256r1, 65, 0x04);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_SUCCESS);

    /* 2. Valid secp384r1 accepted */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp384r1, 97, 0x04);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_SUCCESS);

    /* 3. Valid Curve25519 accepted */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_curve25519, 32, 0x01);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_SUCCESS);

    /* 4. Unsupported curve (e.g. secp521r1 = 25) rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, 25, 133, 0x04);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 5. Compressed point format (0x02 instead of 0x04) rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp256r1, 65, 0x02);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 6. Invalid point length for P-256 (64 instead of 65) rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp256r1, 64, 0x04);
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 7. All-zero coordinates rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp256r1, 65, 0x04);
    fortress_anchor_desc_t *desc = (fortress_anchor_desc_t *)(buf + 16);
    memset(buf + desc->key.ec.q_offset + 1, 0, 32); /* Zero x-coordinate */
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    /* 8. Coordinate exceeding field prime rejected */
    sz = build_synthetic_anchor(buf, sizeof(buf), BR_KEYTYPE_EC, 0, 0, BR_EC_secp256r1, 65, 0x04);
    desc = (fortress_anchor_desc_t *)(buf + 16);
    memset(buf + desc->key.ec.q_offset + 1, 0xff, 32); /* 0xff...ff > p256 */
    assert(fortress_tls_parse_anchors(buf, sz, &s_test_store) == TLS_ERR_ANCHOR_KEY);

    printf("  [PASS] EC curve and coordinate constraints enforced.\n");
}

/* ========================================================================= */
/* Test 8: Single-Descriptor Snapshot Loader (SYS_OPEN / SYS_READ / SYS_CLOSE)*/
/* ========================================================================= */
static int s_mock_open_calls = 0;
static int s_mock_close_calls = 0;
static int s_mock_read_calls = 0;
static int s_mock_simulate_oversize = 0;

static long mock_anchor_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                uintptr_t d, uintptr_t e, uintptr_t f)
{
    (void)d; (void)e; (void)f;
    if (nr == SYS_OPEN) {
        s_mock_open_calls++;
        const char *p = (const char *)a;
        if (strcmp(p, "/nonexistent/anchors.v1") == 0) {
            return -2; /* -ENOENT */
        }
        return 42; /* Simulated file descriptor */
    } else if (nr == SYS_READ) {
        s_mock_read_calls++;
        int fd = (int)a;
        assert(fd == 42);
        void *buf = (void *)b;
        size_t count = (size_t)c;

        if (s_mock_simulate_oversize) {
            /* Simulate filling buffer completely, and then probe returns 1 */
            memset(buf, 0xAA, count);
            return (long)count;
        }

        /* Normal read: return synthetic anchor on first read, then 0 */
        if (s_mock_read_calls == 1) {
            uint8_t syn_buf[2048];
            size_t sz = build_synthetic_anchor(syn_buf, sizeof(syn_buf), BR_KEYTYPE_RSA, 2048, 65537, 0, 0, 0);
            memcpy(buf, syn_buf, sz);
            return (long)sz;
        }
        return 0; /* EOF */
    } else if (nr == SYS_CLOSE) {
        s_mock_close_calls++;
        assert((int)a == 42);
        return 0;
    }
    return -38;
}

static void test_single_descriptor_snapshot_loader(void) {
    printf("  [TEST] Single-descriptor snapshot loader & mock syscalls...\n");

    /* 1. Missing file returns TLS_ERR_ANCHOR_FILE */
    fortress_tls_set_syscall_hook(mock_anchor_syscall);
    s_mock_open_calls = 0;
    s_mock_close_calls = 0;
    s_mock_read_calls = 0;
    s_mock_simulate_oversize = 0;

    int rc = fortress_tls_load_anchors("/nonexistent/anchors.v1", &s_test_store);
    assert(rc == TLS_ERR_ANCHOR_FILE);
    assert(s_mock_open_calls == 1);
    assert(s_mock_close_calls == 0);

    /* 2. Atomic single-descriptor read */
    s_mock_open_calls = 0;
    s_mock_close_calls = 0;
    s_mock_read_calls = 0;
    s_mock_simulate_oversize = 0;

    rc = fortress_tls_load_anchors("/etc/ssl/certs/anchors.v1", &s_test_store);
    assert(rc == TLS_SUCCESS);
    assert(s_test_store.count == 1);
    assert(s_mock_open_calls == 1);
    assert(s_mock_close_calls == 1);
    assert(s_mock_read_calls >= 2); /* First data read, second EOF, third 1-byte probe */

    /* 3. File exceeding 256 KiB cap rejected */
    s_mock_open_calls = 0;
    s_mock_close_calls = 0;
    s_mock_read_calls = 0;
    s_mock_simulate_oversize = 1;

    rc = fortress_tls_load_anchors("/etc/ssl/certs/anchors.v1", &s_test_store);
    assert(rc == TLS_ERR_ANCHOR_BOUNDS);
    assert(s_mock_open_calls == 1);
    assert(s_mock_close_calls == 1);

    /* Restore null hook */
    fortress_tls_set_syscall_hook(NULL);
    printf("  [PASS] Single-descriptor snapshot loader validated.\n");
}

int main(int argc, char **argv) {
    printf("=== FortressOS Phase TLS-2 Trust Store Host Tests ===\n");
    const char *anchor_path = (argc > 1) ? argv[1] : "build/ssl/certs/anchors.v1";

    test_bounds_and_size_limits();
    test_magic_and_version();
    test_anchor_count_limits();
    test_descriptor_and_offsets();
    test_rsa_key_constraints();
    test_ec_curve_constraints();
    test_single_descriptor_snapshot_loader();
    test_valid_store_parsing(anchor_path);

    printf("=== All Phase TLS-2 Trust Store Tests PASSED ===\n");
    return 0;
}
