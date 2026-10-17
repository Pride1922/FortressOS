#ifndef FORTRESS_HMAC_DRBG_H
#define FORTRESS_HMAC_DRBG_H
#include "types.h"
/* SP 800-90A rev.1 10.1.2, SHA-256, no additional input. Caller serializes.
 * Instantiate seed is entropy || nonce || personalization (>=48 HW bytes).
 * Reseed seed starts with >=32 fresh hardware bytes. No entropy API here. */
typedef struct {
    uint8_t k[32], v[32];
    uint64_t requests;
} hmac_drbg_t;
#define HMAC_DRBG_INTERVAL 65536u
void hmac_drbg_init(hmac_drbg_t *d, const uint8_t *seed, size_t n);
void hmac_drbg_reseed(hmac_drbg_t *d, const uint8_t *seed, size_t n);
bool hmac_drbg_generate(hmac_drbg_t *d, uint8_t *out, size_t n);
#endif
