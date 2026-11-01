#ifndef FORTRESS_WLAN_CRYPTO_H
#define FORTRESS_WLAN_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WLAN_SHA1_DIGEST_SIZE   20
#define WLAN_SHA256_DIGEST_SIZE 32
#define WLAN_PMK_LEN            32
#define WLAN_PTK_LEN            64
#define WLAN_KCK_LEN            16
#define WLAN_KEK_LEN            16
#define WLAN_TK_LEN             16
#define WLAN_GTK_TKIP_LEN       32
#define WLAN_MIC_LEN            8
#define WLAN_PMKID_LEN          16
#define WLAN_SAE_GROUP_P256     19u
#define WLAN_SAE_SCALAR_LEN     32
#define WLAN_SAE_ELEMENT_LEN    64
#define WLAN_SAE_COMMIT_LEN     (2 + WLAN_SAE_SCALAR_LEN + WLAN_SAE_ELEMENT_LEN)
#define WLAN_SAE_CONFIRM_LEN    (2 + WLAN_SHA256_DIGEST_SIZE)

/* SHA-1 Context */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} wlan_sha1_ctx_t;

/* SHA-256 Context */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} wlan_sha256_ctx_t;

/* WPA3-SAE (Dragonfly / NIST P-256 Group 19) Context */
typedef struct {
    bool     pwe_valid;
    bool     commit_ready;
    bool     peer_committed;
    bool     confirmed;
    uint16_t send_confirm;
    uint8_t  sta_mac[6];
    uint8_t  bssid[6];
    uint8_t  pwe_x[32];
    uint8_t  pwe_y[32];
    uint8_t  rand_val[32];
    uint8_t  mask_val[32];
    uint8_t  own_scalar[32];
    uint8_t  own_elem_x[32];
    uint8_t  own_elem_y[32];
    uint8_t  peer_scalar[32];
    uint8_t  peer_elem_x[32];
    uint8_t  peer_elem_y[32];
    uint8_t  kck[32];
    uint8_t  pmk[WLAN_PMK_LEN];
    uint8_t  pmkid[WLAN_PMKID_LEN];
} wlan_sae_ctx_t;

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

/* IEEE 802.11i TKIP Group Frame Encryption & Decryption (Phase 1/2 + RC4 + CRC-32 + Michael MIC) */
int wlan_tkip_encrypt_frame(const uint8_t gtk[WLAN_GTK_TKIP_LEN],
                            uint64_t tsc,
                            uint8_t key_id,
                            const uint8_t *plain_frame, size_t plain_len,
                            uint8_t *out_frame, size_t *out_len);

int wlan_tkip_decrypt_frame(const uint8_t gtk[WLAN_GTK_TKIP_LEN],
                            const uint8_t *cipher_frame, size_t cipher_len,
                            uint8_t *out_frame, size_t *out_len,
                            uint64_t *rx_tsc,
                            uint64_t *rx_bitmap);

/* SHA-256, HMAC-SHA-256, and IEEE 802.11 KDF-SHA-256 API */
void wlan_sha256_init(wlan_sha256_ctx_t *ctx);
void wlan_sha256_update(wlan_sha256_ctx_t *ctx, const void *data, size_t len);
void wlan_sha256_final(wlan_sha256_ctx_t *ctx, uint8_t digest[WLAN_SHA256_DIGEST_SIZE]);
void wlan_sha256(const void *data, size_t len, uint8_t digest[WLAN_SHA256_DIGEST_SIZE]);

void wlan_hmac_sha256(const void *key, size_t key_len,
                      const void *data, size_t data_len,
                      uint8_t mac[WLAN_SHA256_DIGEST_SIZE]);

void wlan_kdf_sha256(const uint8_t *key, size_t key_len,
                     const char *label,
                     const uint8_t *context, size_t context_len,
                     uint8_t *out, size_t out_bits);

void wlan_derive_ptk_sha256(const uint8_t pmk[WLAN_PMK_LEN],
                            const uint8_t addr1[6], const uint8_t addr2[6],
                            const uint8_t nonce1[32], const uint8_t nonce2[32],
                            uint8_t ptk[WLAN_PTK_LEN]);

/* WPA3-Personal Simultaneous Authentication of Equals (SAE / RFC 7664 / Group 19 NIST P-256) */
int wlan_sae_derive_pwe(wlan_sae_ctx_t *ctx,
                        const uint8_t sta_mac[6],
                        const uint8_t bssid[6],
                        const char *passphrase);

int wlan_sae_build_commit(wlan_sae_ctx_t *ctx,
                          uint8_t out_body[WLAN_SAE_COMMIT_LEN]);

int wlan_sae_process_commit(wlan_sae_ctx_t *ctx,
                            const uint8_t *peer_body, size_t peer_len);

int wlan_sae_build_confirm(wlan_sae_ctx_t *ctx,
                           uint8_t out_body[WLAN_SAE_CONFIRM_LEN]);

int wlan_sae_verify_confirm(wlan_sae_ctx_t *ctx,
                            const uint8_t *peer_body, size_t peer_len);

#endif /* FORTRESS_WLAN_CRYPTO_H */
