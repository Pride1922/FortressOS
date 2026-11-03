#ifndef FORTRESS_VAULT_H
#define FORTRESS_VAULT_H

#include "types.h"

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c);

#define VAULT_MAGIC                 0x4C554156 /* "VAUL" in little-endian */
#define VAULT_VERSION               1
#define VAULT_KDF_PBKDF2_SHA256     1

#define VAULT_SALT_LEN              16
#define VAULT_NONCE_LEN             12
#define VAULT_TAG_LEN               16
#define VAULT_KEY_LEN               32
#define VAULT_DEFAULT_ITERATIONS    100000

#define VAULT_MAX_RECORDS           128
#define VAULT_MAX_KEY_LEN           64
#define VAULT_MAX_SECRET_LEN        4096
#define VAULT_MAX_META_LEN          256
#define VAULT_MAX_PAYLOAD_LEN       65536

#define VAULT_TYPE_PASSWORD         1
#define VAULT_TYPE_SSH_KEY          2
#define VAULT_TYPE_TOKEN            3
#define VAULT_TYPE_GENERIC          4

#define VAULT_ERR_OK                0
#define VAULT_ERR_INVAL            -1
#define VAULT_ERR_IO               -2
#define VAULT_ERR_CORRUPT          -3
#define VAULT_ERR_AUTH             -4
#define VAULT_ERR_NOTFOUND         -5
#define VAULT_ERR_FULL             -6
#define VAULT_ERR_NOMEM            -7

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* VAULT_MAGIC: "VAUL" */
    uint16_t version;        /* VAULT_VERSION (1) */
    uint8_t  kdf_type;       /* VAULT_KDF_PBKDF2_SHA256 (1) */
    uint8_t  reserved;       /* 0 */
    uint32_t iterations;     /* PBKDF2 iteration count */
    uint8_t  salt[VAULT_SALT_LEN];
    uint8_t  nonce[VAULT_NONCE_LEN];
    uint8_t  tag[VAULT_TAG_LEN];
    uint32_t payload_len;    /* Byte count of following ciphertext */
} vault_header_t;
#pragma pack(pop)

/* Size of Additional Authenticated Data: fields before nonce */
#define VAULT_AD_LEN 24

typedef struct {
    char     key[VAULT_MAX_KEY_LEN];
    uint8_t  secret_type;
    uint8_t  secret[VAULT_MAX_SECRET_LEN];
    size_t   secret_len;
    char     meta[VAULT_MAX_META_LEN];
    uint64_t created_at;
} vault_record_t;

typedef struct {
    uint32_t       iterations;
    uint8_t        salt[VAULT_SALT_LEN];
    vault_record_t records[VAULT_MAX_RECORDS];
    size_t         count;
    bool           modified;
} vault_db_t;

/* Crypto primitives */
int vault_pbkdf2_sha256(const uint8_t *pass, size_t pass_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t out_key[VAULT_KEY_LEN]);

int vault_crypto_encrypt(const uint8_t *plaintext, size_t plain_len,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t key[VAULT_KEY_LEN],
                         const uint8_t nonce[VAULT_NONCE_LEN],
                         uint8_t *ciphertext,
                         uint8_t tag[VAULT_TAG_LEN]);

int vault_crypto_decrypt(const uint8_t *ciphertext, size_t cipher_len,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t key[VAULT_KEY_LEN],
                         const uint8_t nonce[VAULT_NONCE_LEN],
                         const uint8_t tag[VAULT_TAG_LEN],
                         uint8_t *plaintext);

/* DB Management */
void vault_db_init_empty(vault_db_t *db, uint32_t iterations);
void vault_db_clear(vault_db_t *db);
int vault_db_deserialize(vault_db_t *db, const uint8_t *payload, size_t len);
int vault_db_serialize(const vault_db_t *db, uint8_t *out_payload, size_t max_len, size_t *out_len);

const vault_record_t *vault_db_get_ref(const vault_db_t *db, const char *key);
int vault_db_get(const vault_db_t *db, const char *key, vault_record_t *out_rec);
int vault_db_set(vault_db_t *db, const char *key, uint8_t type,
                 const uint8_t *secret, size_t secret_len, const char *meta);
int vault_db_delete(vault_db_t *db, const char *key);

/* File persistence (atomic via SYS_RENAME_REPLACE and fsync) */
int vault_file_load(const char *path, const char *passphrase, vault_db_t *db);
int vault_file_save(const char *path, const char *passphrase, vault_db_t *db);

/* High-level direct helpers */
int vault_get_secret(const char *path, const char *passphrase, const char *key,
                     uint8_t *out_buf, size_t max_len, size_t *out_len);
int vault_set_secret(const char *path, const char *passphrase, const char *key,
                     uint8_t type, const uint8_t *secret, size_t secret_len, const char *meta);

#endif /* FORTRESS_VAULT_H */
