/* user/vault/vault_crypto.c - FortressOS Credential Vault Cryptographic Engine
 *
 * Implements:
 * 1. PBKDF2-HMAC-SHA256 (RFC 2898 / RFC 8018) key derivation.
 * 2. Authenticated encryption and decryption via ChaCha20-Poly1305 AEAD.
 * 3. Strict zero-wipe memory hygiene for all keys and plaintext material.
 */

#include "vault.h"
#include "../../src/include/crypto.h"

#ifdef TOOL_HOST_TEST
#include <string.h>
#else
#include "../../user/tools/common.h"
static inline void *vault_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
#define memcpy vault_memcpy
#endif

int vault_pbkdf2_sha256(const uint8_t *pass, size_t pass_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t out_key[VAULT_KEY_LEN])
{
    if (!pass || !salt || !out_key || iterations == 0 || salt_len > 256) {
        return VAULT_ERR_INVAL;
    }

    /*
     * For 32-byte derived key output (matching SHA-256 output size),
     * only a single block (i = 1) is evaluated:
     * U_1 = HMAC-SHA256(Pass, Salt || 0x00 0x00 0x00 0x01)
     * U_2 = HMAC-SHA256(Pass, U_1)
     * ...
     * out_key = U_1 ^ U_2 ^ ... ^ U_iterations
     */
    uint8_t salt_block[260];
    memcpy(salt_block, salt, salt_len);
    salt_block[salt_len + 0] = 0;
    salt_block[salt_len + 1] = 0;
    salt_block[salt_len + 2] = 0;
    salt_block[salt_len + 3] = 1;

    uint8_t u_prev[32];
    uint8_t u_curr[32];

    int r = crypto_hmac_sha256(pass, pass_len, salt_block, salt_len + 4, u_prev);
    crypto_wipe(salt_block, sizeof(salt_block));
    if (r != 0) {
        crypto_wipe(u_prev, sizeof(u_prev));
        return VAULT_ERR_AUTH;
    }

    memcpy(out_key, u_prev, 32);

    for (uint32_t round = 2; round <= iterations; round++) {
        r = crypto_hmac_sha256(pass, pass_len, u_prev, 32, u_curr);
        if (r != 0) {
            crypto_wipe(u_prev, sizeof(u_prev));
            crypto_wipe(u_curr, sizeof(u_curr));
            crypto_wipe(out_key, 32);
            return VAULT_ERR_AUTH;
        }

        for (int b = 0; b < 32; b++) {
            out_key[b] ^= u_curr[b];
        }
        memcpy(u_prev, u_curr, 32);
    }

    crypto_wipe(u_prev, sizeof(u_prev));
    crypto_wipe(u_curr, sizeof(u_curr));
    return VAULT_ERR_OK;
}

int vault_crypto_encrypt(const uint8_t *plaintext, size_t plain_len,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t key[VAULT_KEY_LEN],
                         const uint8_t nonce[VAULT_NONCE_LEN],
                         uint8_t *ciphertext,
                         uint8_t tag[VAULT_TAG_LEN])
{
    if (!key || !nonce || !tag) {
        return VAULT_ERR_INVAL;
    }
    if (plain_len > 0 && (!plaintext || !ciphertext)) {
        return VAULT_ERR_INVAL;
    }

    int rc = crypto_aead_lock(ciphertext, tag, key, nonce, ad, ad_len, plaintext, plain_len);
    if (rc != 0) {
        return VAULT_ERR_AUTH;
    }
    return VAULT_ERR_OK;
}

int vault_crypto_decrypt(const uint8_t *ciphertext, size_t cipher_len,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t key[VAULT_KEY_LEN],
                         const uint8_t nonce[VAULT_NONCE_LEN],
                         const uint8_t tag[VAULT_TAG_LEN],
                         uint8_t *plaintext)
{
    if (!key || !nonce || !tag) {
        return VAULT_ERR_INVAL;
    }
    if (cipher_len > 0 && (!ciphertext || !plaintext)) {
        return VAULT_ERR_INVAL;
    }

    int rc = crypto_aead_unlock(plaintext, tag, key, nonce, ad, ad_len, ciphertext, cipher_len);
    if (rc != 0) {
        /* Fail-closed: authentication tag mismatch or corrupt ciphertext */
        return VAULT_ERR_AUTH;
    }
    return VAULT_ERR_OK;
}
