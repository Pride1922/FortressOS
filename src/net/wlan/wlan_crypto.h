#ifndef FORTRESS_WLAN_CRYPTO_H
#define FORTRESS_WLAN_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WLAN_SHA1_DIGEST_SIZE   20
#define WLAN_PMK_LEN            32
#define WLAN_PTK_LEN            64
#define WLAN_KCK_LEN            16
#define WLAN_KEK_LEN            16
#define WLAN_TK_LEN             16
#define WLAN_MIC_LEN            8

/* SHA-1 Context */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} wlan_sha1_ctx_t;

/* SHA-1 & HMAC-SHA-1 API */
void wlan_sha1_init(wlan_sha1_ctx_t *ctx);
void wlan_sha1_update(wlan_sha1_ctx_t *ctx, const void *data, size_t len);
void wlan_sha1_final(wlan_sha1_ctx_t *ctx, uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);
void wlan_sha1(const void *data, size_t len, uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);

void wlan_hmac_sha1(const void *key, size_t key_len,
                    const void *data, size_t data_len,
                    uint8_t mac[WLAN_SHA1_DIGEST_SIZE]);

/* WPA2 PBKDF2 Key Derivation: Passphrase + SSID -> PMK (256-bit) */
int wlan_pbkdf2_sha1(const char *passphrase,
                     const uint8_t *ssid, size_t ssid_len,
                     uint32_t iterations,
                     uint8_t *out_key, size_t out_len);

/* IEEE 802.11i PRF-512 (Pseudo-Random Function) */
void wlan_prf_512(const uint8_t pmk[WLAN_PMK_LEN],
                  const char *label,
                  const uint8_t *data, size_t data_len,
                  uint8_t ptk[WLAN_PTK_LEN]);

/* Derives PTK (KCK | KEK | TK) from PMK, addresses, and nonces */
void wlan_derive_ptk(const uint8_t pmk[WLAN_PMK_LEN],
                     const uint8_t addr1[6], const uint8_t addr2[6],
                     const uint8_t nonce1[32], const uint8_t nonce2[32],
                     uint8_t ptk[WLAN_PTK_LEN]);

/* NIST AES Key Wrap (RFC 3394) for GTK unwrapping */
int wlan_aes_unwrap(const uint8_t kek[16],
                    const uint8_t *wrapped, size_t wrapped_len,
                    uint8_t *plain);

/* IEEE 802.11i / RFC 3610 AES-128-CCMP Encryption & Decryption */
int wlan_ccmp_encrypt(const uint8_t tk[16],
                      uint64_t pn,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *plain, size_t plain_len,
                      uint8_t *cipher,
                      uint8_t mic[WLAN_MIC_LEN]);

int wlan_ccmp_decrypt(const uint8_t tk[16],
                      uint64_t pn,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *cipher, size_t cipher_len,
                      const uint8_t mic[WLAN_MIC_LEN],
                      uint8_t *plain);

/* Whole-frame CCMP 802.11 encapsulation/decapsulation helpers */
int wlan_ccmp_encrypt_frame(const uint8_t tk[16],
                            uint64_t pn,
                            const uint8_t *plain_frame, size_t plain_len,
                            uint8_t *out_frame, size_t *out_len);

int wlan_ccmp_decrypt_frame(const uint8_t tk[16],
                            const uint8_t *cipher_frame, size_t cipher_len,
                            uint8_t *out_frame, size_t *out_len,
                            uint64_t *rx_pn,
                            uint64_t *rx_bitmap);

#endif /* FORTRESS_WLAN_CRYPTO_H */
