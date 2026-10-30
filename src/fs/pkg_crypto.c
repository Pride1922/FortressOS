#include "pkg_engine_config.h"
#if FORTRESS_PKG_ENGINE
#include "crypto.h"
#include "string.h"
#include "../../user/tools/digest.h"

int crypto_sha256_init(crypto_sha256_ctx *c) {
    if (!c) return -1;
    *c = (crypto_sha256_ctx){0};
    digest_init(&c->digest, DIGEST_SHA256);
    c->active = true;
    return 0;
}
int crypto_sha256_update(crypto_sha256_ctx *c, const void *p, size_t n) {
    if (!c || !c->active || (!p && n)) return -1;
    return digest_update(&c->digest, p, n) ? 0 : -1;
}
int crypto_sha256_final(crypto_sha256_ctx *c, uint8_t out[32]) {
    if (!c || !c->active || !out) return -1;
    digest_final(&c->digest, out);
    memset(c, 0, sizeof(*c));
    return 0;
}
int crypto_sha256(const uint8_t *p, size_t n, uint8_t out[32]) {
    if (!out || (!p && n)) return -1;
    crypto_sha256_ctx c;
    crypto_sha256_init(&c);
    int r = crypto_sha256_update(&c, p, n);
    if (!r) r = crypto_sha256_final(&c, out);
    memset(&c, 0, sizeof(c));
    return r;
}

void crypto_wipe(void *p, size_t n) {
    if (!p) return;
    volatile uint8_t *vp = (volatile uint8_t *)p;
    while (n--) *vp++ = 0;
}

int crypto_verify32(const uint8_t a[32], const uint8_t b[32]) {
    if (!a || !b) return -1;
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= a[i] ^ b[i];
    return diff == 0 ? 0 : -1;
}

#include "../../src/lib/crypto/namespace.h"
#include "../../src/lib/crypto/upstream/src/monocypher.c"
#undef FOR
#define hash_reduce ed25519_hash_reduce
#include "../../src/lib/crypto/upstream/src/optional/monocypher-ed25519.c"
#undef hash_reduce
#include "../../src/lib/crypto/namespace_end.h"

int crypto_ed25519_verify(const uint8_t sig[64], const uint8_t pk[32], const uint8_t *msg, size_t n) {
    if (!sig || !pk || (!msg && n)) return -1;
    return mono_crypto_ed25519_check(sig, pk, msg, n) == 0 ? 0 : -1;
}
#endif
