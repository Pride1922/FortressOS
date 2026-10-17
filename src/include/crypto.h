#ifndef FORTRESS_CRYPTO_H
#define FORTRESS_CRYPTO_H
#include "types.h"
#include "../../user/tools/digest.h"

#define CRYPTO_SHA256_LEN 32
#define CRYPTO_SHA512_LEN 64
#define CRYPTO_HMAC_SHA256_LEN 32
#define CRYPTO_ED25519_PUBKEY_LEN 32
#define CRYPTO_ED25519_SECKEY_LEN 64
#define CRYPTO_ED25519_SIG_LEN 64
#define CRYPTO_X25519_SECKEY_LEN 32
#define CRYPTO_X25519_PUBKEY_LEN 32
#define CRYPTO_X25519_SHARED_LEN 32
#define CRYPTO_CHACHA20_KEY_LEN 32
#define CRYPTO_CHACHA20_NONCE_LEN 12
#define CRYPTO_POLY1305_TAG_LEN 16
#define CRYPTO_MAX_INPUT (UINT64_MAX/8-256)
#define CRYPTO_MAX_AEAD_BYTES (UINT64_C(0xffffffff)*64)

/* All return 0 on success/equal/valid, negative on failure. Pointers must name
 * accessible caller memory (this is a library, not a syscall). NULL is allowed
 * only for zero-length data. Length/address wrap and unsupported aliases fail
 * before output. No output on failure, including authenticated decryption.
 * Exact in-place AEAD is supported; other writable/input overlaps are rejected.
 * Key/nonce/AD/tag must be disjoint from writable AEAD data and each other
 * where written. Nonces must be unique for each key; no SSH packet construction.
 * Seeded APIs are explicitly deterministic; keygen always obtains fresh entropy.
 * Streaming context must be initialized, exclusively owned and disjoint from
 * inputs/output; final consumes/wipes it. Failed updates do not change it. */
typedef struct { digest_t digest; bool active; } crypto_sha256_ctx;
typedef struct {
    uint8_t state[256]; /* upstream state copied through properly typed locals */
    uint64_t bytes;
    bool active;
} crypto_sha512_ctx;
void crypto_wipe(void *secret, size_t size);
int crypto_verify32(const uint8_t a[32], const uint8_t b[32]);
int crypto_sha256_init(crypto_sha256_ctx *ctx);
int crypto_sha256_update(crypto_sha256_ctx *ctx,const void *msg,size_t n);
int crypto_sha256_final(crypto_sha256_ctx *ctx,uint8_t out[32]);
int crypto_sha512_init(crypto_sha512_ctx *ctx);
int crypto_sha512_update(crypto_sha512_ctx *ctx,const void *msg,size_t n);
int crypto_sha512_final(crypto_sha512_ctx *ctx,uint8_t out[64]);
int crypto_sha256(const uint8_t *msg,size_t n,uint8_t out[32]);
int crypto_sha512(const uint8_t *msg,size_t n,uint8_t out[64]);
int crypto_hmac_sha256(const uint8_t *key,size_t keylen,const uint8_t *msg,size_t n,uint8_t out[32]);
int crypto_ed25519_from_seed(const uint8_t seed[32],uint8_t pubkey[32],uint8_t seckey[64]);
int crypto_ed25519_keygen(uint8_t pubkey[32],uint8_t seckey[64]);
int crypto_ed25519_sign(uint8_t sig[64],const uint8_t seckey[64],const uint8_t *msg,size_t n);
int crypto_ed25519_verify(const uint8_t sig[64],const uint8_t pubkey[32],const uint8_t *msg,size_t n);
int crypto_x25519_from_seed(const uint8_t seed[32],uint8_t seckey[32],uint8_t pubkey[32]);
int crypto_x25519_keygen(uint8_t seckey[32],uint8_t pubkey[32]);
int crypto_x25519(uint8_t shared[32],const uint8_t seckey[32],const uint8_t pubkey[32]);
int crypto_aead_lock(uint8_t *ciphertext,uint8_t tag[16],const uint8_t key[32],const uint8_t nonce[12],const uint8_t *ad,size_t adlen,const uint8_t *plaintext,size_t n);
int crypto_aead_unlock(uint8_t *plaintext,const uint8_t tag[16],const uint8_t key[32],const uint8_t nonce[12],const uint8_t *ad,size_t adlen,const uint8_t *ciphertext,size_t n);
#define CRYPTO_HKDF_MAX_OKM_LEN 8160
int crypto_hkdf_extract(const uint8_t *salt,size_t salt_len,const uint8_t *ikm,size_t ikm_len,uint8_t prk_out[32]);
int crypto_hkdf_expand(const uint8_t *prk,size_t prk_len,const uint8_t *info,size_t info_len,uint8_t *okm,size_t okm_len);
int crypto_hkdf(const uint8_t *salt,size_t salt_len,const uint8_t *ikm,size_t ikm_len,const uint8_t *info,size_t info_len,uint8_t *okm,size_t okm_len);
#endif
