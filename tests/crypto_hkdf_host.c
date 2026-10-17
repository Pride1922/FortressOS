#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto.h"
#include "syscall_abi.h"

#ifdef CRYPTO_HOST_TEST
void crypto_test_wiped(const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        assert(!((const uint8_t *)p)[i]);
    }
}
int64_t crypto_test_getrandom(void *p, size_t n, uint64_t f) {
    (void)p; (void)n; (void)f;
    return SYSCALL_EIO;
}
#endif

static void unhex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned x;
        assert(sscanf(hex + 2 * i, "%2x", &x) == 1);
        out[i] = (uint8_t)x;
    }
}

static void eqhex(const uint8_t *got, const char *hex, size_t n) {
    uint8_t expected[128];
    assert(n <= sizeof(expected));
    unhex(hex, expected, n);
    assert(!memcmp(got, expected, n));
}

/* RFC 5869 Appendix A.1 Test Case 1 */
static void test_case_1(void) {
    uint8_t ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));
    uint8_t salt[13];
    for (unsigned i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    uint8_t info[10];
    for (unsigned i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);

    uint8_t prk[32];
    assert(crypto_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk) == 0);
    eqhex(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32);

    uint8_t okm[42];
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), okm, sizeof(okm)) == 0);
    eqhex(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42);

    /* Test convenience wrapper */
    uint8_t wrapper_okm[42];
    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), wrapper_okm, sizeof(wrapper_okm)) == 0);
    assert(!memcmp(okm, wrapper_okm, sizeof(okm)));

    puts("PASS RFC 5869 Test Case 1 (SHA-256 basic)");
}

/* RFC 5869 Appendix A.2 Test Case 2 */
static void test_case_2(void) {
    uint8_t ikm[80];
    for (unsigned i = 0; i < 80; i++) ikm[i] = (uint8_t)i;
    uint8_t salt[80];
    for (unsigned i = 0; i < 80; i++) salt[i] = (uint8_t)(0x60 + i);
    uint8_t info[80];
    for (unsigned i = 0; i < 80; i++) info[i] = (uint8_t)(0xb0 + i);

    uint8_t prk[32];
    assert(crypto_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk) == 0);
    eqhex(prk, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244", 32);

    uint8_t okm[82];
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), okm, sizeof(okm)) == 0);
    eqhex(okm,
          "b11e398dc80327a1c8e7f78c596a4934"
          "4f012eda2d4efad8a050cc4c19afa97c"
          "59045a99cac7827271cb41c65e590e09"
          "da3275600c2f09b8367793a9aca3db71"
          "cc30c58179ec3e87c14c01d5c1f3434f"
          "1d87", 82);

    uint8_t wrapper_okm[82];
    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), wrapper_okm, sizeof(wrapper_okm)) == 0);
    assert(!memcmp(okm, wrapper_okm, sizeof(okm)));

    puts("PASS RFC 5869 Test Case 2 (SHA-256 long inputs/outputs)");
}

/* RFC 5869 Appendix A.3 Test Case 3 */
static void test_case_3(void) {
    uint8_t ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));

    /* Case 3a: salt = NULL, salt_len = 0, info = NULL, info_len = 0 */
    uint8_t prk[32];
    assert(crypto_hkdf_extract(NULL, 0, ikm, sizeof(ikm), prk) == 0);
    eqhex(prk, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", 32);

    uint8_t okm[42];
    assert(crypto_hkdf_expand(prk, sizeof(prk), NULL, 0, okm, sizeof(okm)) == 0);
    eqhex(okm, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", 42);

    uint8_t wrapper_okm[42];
    assert(crypto_hkdf(NULL, 0, ikm, sizeof(ikm), NULL, 0, wrapper_okm, sizeof(wrapper_okm)) == 0);
    assert(!memcmp(okm, wrapper_okm, sizeof(okm)));

    /* Case 3b: non-NULL pointers with zero lengths */
    uint8_t dummy = 0x42;
    uint8_t prk2[32];
    assert(crypto_hkdf_extract(&dummy, 0, ikm, sizeof(ikm), prk2) == 0);
    assert(!memcmp(prk, prk2, 32));

    uint8_t okm2[42];
    assert(crypto_hkdf_expand(prk, sizeof(prk), &dummy, 0, okm2, sizeof(okm2)) == 0);
    assert(!memcmp(okm, okm2, 42));

    uint8_t wrapper_okm2[42];
    assert(crypto_hkdf(&dummy, 0, ikm, sizeof(ikm), &dummy, 0, wrapper_okm2, sizeof(wrapper_okm2)) == 0);
    assert(!memcmp(okm, wrapper_okm2, 42));

    puts("PASS RFC 5869 Test Case 3 (SHA-256 zero-length salt/info)");
}

/* Edge cases, limits, bounds, and error handling */
static void test_edge_cases_and_errors(void) {
    uint8_t ikm[22];
    memset(ikm, 0x0b, sizeof(ikm));
    uint8_t salt[13];
    memset(salt, 0x01, sizeof(salt));
    uint8_t info[10];
    memset(info, 0x02, sizeof(info));
    uint8_t prk[32];
    assert(crypto_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk) == 0);

    /* okm_len = 0 edge case: must succeed and not write anything */
    uint8_t canary = 0x5a;
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), NULL, 0) == 0);
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), &canary, 0) == 0);
    assert(canary == 0x5a);

    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), NULL, 0) == 0);
    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), &canary, 0) == 0);
    assert(canary == 0x5a);

    /* okm_len > 8160 (255 * 32): must reject */
    uint8_t *large_buf = malloc(8192);
    assert(large_buf != NULL);
    memset(large_buf, 0xaa, 8192);
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), large_buf, 8161) == SYSCALL_EINVAL);
    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), large_buf, 8161) == SYSCALL_EINVAL);
    assert(large_buf[0] == 0xaa); /* preserved before output */

    /* okm_len = 8160 (exact max boundary = 255 * 32) */
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), large_buf, 8160) == 0);
    /* Verify first 32 bytes match T(1) */
    uint8_t t1[32];
    assert(crypto_hkdf_expand(prk, sizeof(prk), info, sizeof(info), t1, 32) == 0);
    assert(!memcmp(large_buf, t1, 32));

    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), large_buf, 8160) == 0);
    assert(!memcmp(large_buf, t1, 32));
    free(large_buf);

    /* NULL pointer rejections */
    assert(crypto_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), NULL) == SYSCALL_EINVAL);
    assert(crypto_hkdf_extract(NULL, 10, ikm, sizeof(ikm), prk) == SYSCALL_EINVAL);
    assert(crypto_hkdf_extract(salt, sizeof(salt), NULL, 10, prk) == SYSCALL_EINVAL);

    uint8_t out[32];
    assert(crypto_hkdf_expand(NULL, 32, info, sizeof(info), out, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf_expand(prk, 32, NULL, 10, out, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf_expand(prk, 32, info, sizeof(info), NULL, 32) == SYSCALL_EINVAL);

    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), NULL, 32) == SYSCALL_EINVAL);

    /* Short PRK (< 32 bytes) rejection per RFC 5869 Section 2.3 */
    assert(crypto_hkdf_expand(prk, 31, info, sizeof(info), out, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf_expand(prk, 0, info, sizeof(info), out, 32) == SYSCALL_EINVAL);

    /* Overlap rejections */
    uint8_t shared_buf[64];
    memset(shared_buf, 0x33, sizeof(shared_buf));
    /* PRK out overlapping salt/ikm in extract */
    assert(crypto_hkdf_extract(shared_buf, 32, ikm, sizeof(ikm), shared_buf) == SYSCALL_EINVAL);
    assert(crypto_hkdf_extract(salt, sizeof(salt), shared_buf, 32, shared_buf) == SYSCALL_EINVAL);

    /* OKM overlapping PRK/info in expand */
    assert(crypto_hkdf_expand(shared_buf, 32, info, sizeof(info), shared_buf, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf_expand(prk, sizeof(prk), shared_buf, 32, shared_buf, 32) == SYSCALL_EINVAL);

    /* OKM overlapping inputs in convenience wrapper */
    assert(crypto_hkdf(shared_buf, 32, ikm, sizeof(ikm), info, sizeof(info), shared_buf, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf(salt, sizeof(salt), shared_buf, 32, info, sizeof(info), shared_buf, 32) == SYSCALL_EINVAL);
    assert(crypto_hkdf(salt, sizeof(salt), ikm, sizeof(ikm), shared_buf, 32, shared_buf, 32) == SYSCALL_EINVAL);

    puts("PASS edge cases (salt_len=0, info_len=0, okm_len=0, max okm_len=8160, null ptrs, short prk, overlaps)");
}

int main(void) {
    test_case_1();
    test_case_2();
    test_case_3();
    test_edge_cases_and_errors();
    puts("ALL HKDF-SHA-256 TESTS PASSED (tests/crypto_hkdf_host.c)");
    return 0;
}
