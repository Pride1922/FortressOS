/* user/vault/vault_db.c - FortressOS Credential Vault Database Engine
 *
 * Implements:
 * 1. Vault in-memory table management (CRUD operations).
 * 2. Record serialization & deserialization.
 * 3. Atomic persistence via SYS_RENAME_REPLACE and fsync.
 * 4. Zero-wipe memory sanitation on close / clear.
 */

#include "vault.h"
#include "../../src/include/crypto.h"
#include "../../src/include/syscall_abi.h"
#include "../../src/fs/vfs.h"

#ifdef TOOL_HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include "../../user/tools/common.h"
static inline void *vault_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static inline void *vault_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}
static inline int vault_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static inline size_t vault_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
#define memcpy vault_memcpy
#define memset vault_memset
#define strcmp vault_strcmp
#define strlen vault_strlen
#endif

void vault_db_init_empty(vault_db_t *db, uint32_t iterations) {
    if (!db) return;
    memset(db, 0, sizeof(*db));
    db->iterations = (iterations > 0) ? iterations : VAULT_DEFAULT_ITERATIONS;
    db->modified = true;
}

void vault_db_clear(vault_db_t *db) {
    if (!db) return;
    crypto_wipe(db, sizeof(*db));
}

const vault_record_t *vault_db_get_ref(const vault_db_t *db, const char *key) {
    if (!db || !key) return NULL;

    for (size_t i = 0; i < db->count; i++) {
        if (strcmp(db->records[i].key, key) == 0) {
            return &db->records[i];
        }
    }
    return NULL;
}

int vault_db_get(const vault_db_t *db, const char *key, vault_record_t *out_rec) {
    if (!db || !key || !out_rec) return VAULT_ERR_INVAL;

    const vault_record_t *rec = vault_db_get_ref(db, key);
    if (!rec) return VAULT_ERR_NOTFOUND;

    memcpy(out_rec, rec, sizeof(vault_record_t));
    return VAULT_ERR_OK;
}

int vault_db_set(vault_db_t *db, const char *key, uint8_t type,
                 const uint8_t *secret, size_t secret_len, const char *meta)
{
    if (!db || !key || !secret || secret_len > VAULT_MAX_SECRET_LEN) {
        return VAULT_ERR_INVAL;
    }
    size_t key_len = strlen(key);
    if (key_len == 0 || key_len >= VAULT_MAX_KEY_LEN) {
        return VAULT_ERR_INVAL;
    }

    size_t target_idx = db->count;
    /* Check for existing record to overwrite */
    for (size_t i = 0; i < db->count; i++) {
        if (strcmp(db->records[i].key, key) == 0) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == db->count) {
        if (db->count >= VAULT_MAX_RECORDS) {
            return VAULT_ERR_FULL;
        }
        db->count++;
    }

    vault_record_t *rec = &db->records[target_idx];
    crypto_wipe(rec, sizeof(*rec));

    memcpy(rec->key, key, key_len);
    rec->key[key_len] = '\0';
    rec->secret_type = type;
    memcpy(rec->secret, secret, secret_len);
    rec->secret_len = secret_len;

    if (meta) {
        size_t mlen = strlen(meta);
        if (mlen >= VAULT_MAX_META_LEN) mlen = VAULT_MAX_META_LEN - 1;
        memcpy(rec->meta, meta, mlen);
        rec->meta[mlen] = '\0';
    } else {
        rec->meta[0] = '\0';
    }

    rec->created_at = 0; /* Monotonic / system timestamp can be filled by caller */
    db->modified = true;
    return VAULT_ERR_OK;
}

int vault_db_delete(vault_db_t *db, const char *key) {
    if (!db || !key) return VAULT_ERR_INVAL;

    for (size_t i = 0; i < db->count; i++) {
        if (strcmp(db->records[i].key, key) == 0) {
            /* Shift down remaining records */
            for (size_t j = i; j + 1 < db->count; j++) {
                memcpy(&db->records[j], &db->records[j + 1], sizeof(vault_record_t));
            }
            db->count--;
            crypto_wipe(&db->records[db->count], sizeof(vault_record_t));
            db->modified = true;
            return VAULT_ERR_OK;
        }
    }
    return VAULT_ERR_NOTFOUND;
}

int vault_db_serialize(const vault_db_t *db, uint8_t *out_payload, size_t max_len, size_t *out_len) {
    if (!db || !out_payload || !out_len) return VAULT_ERR_INVAL;

    size_t at = 0;
    for (size_t i = 0; i < db->count; i++) {
        const vault_record_t *rec = &db->records[i];
        size_t klen = strlen(rec->key);
        size_t mlen = strlen(rec->meta);
        size_t rec_bytes = 2 + 1 + 2 + 2 + 8 + klen + rec->secret_len + mlen;

        if (at + rec_bytes > max_len) {
            return VAULT_ERR_NOMEM;
        }

        /* 1. key_len (2 bytes) */
        out_payload[at++] = (uint8_t)(klen & 0xFF);
        out_payload[at++] = (uint8_t)((klen >> 8) & 0xFF);

        /* 2. secret_type (1 byte) */
        out_payload[at++] = rec->secret_type;

        /* 3. secret_len (2 bytes) */
        out_payload[at++] = (uint8_t)(rec->secret_len & 0xFF);
        out_payload[at++] = (uint8_t)((rec->secret_len >> 8) & 0xFF);

        /* 4. meta_len (2 bytes) */
        out_payload[at++] = (uint8_t)(mlen & 0xFF);
        out_payload[at++] = (uint8_t)((mlen >> 8) & 0xFF);

        /* 5. created_at (8 bytes) */
        for (int b = 0; b < 8; b++) {
            out_payload[at++] = (uint8_t)((rec->created_at >> (b * 8)) & 0xFF);
        }

        /* 6. key string */
        memcpy(out_payload + at, rec->key, klen);
        at += klen;

        /* 7. secret bytes */
        memcpy(out_payload + at, rec->secret, rec->secret_len);
        at += rec->secret_len;

        /* 8. meta string */
        memcpy(out_payload + at, rec->meta, mlen);
        at += mlen;
    }

    *out_len = at;
    return VAULT_ERR_OK;
}

int vault_db_deserialize(vault_db_t *db, const uint8_t *payload, size_t len) {
    if (!db || (!payload && len > 0)) return VAULT_ERR_INVAL;

    db->count = 0;
    size_t at = 0;

    while (at < len) {
        if (at + 15 > len) { /* 2 + 1 + 2 + 2 + 8 = 15 */
            return VAULT_ERR_CORRUPT;
        }

        uint16_t klen = (uint16_t)payload[at] | ((uint16_t)payload[at + 1] << 8);
        at += 2;
        uint8_t stype = payload[at++];
        uint16_t slen = (uint16_t)payload[at] | ((uint16_t)payload[at + 1] << 8);
        at += 2;
        uint16_t mlen = (uint16_t)payload[at] | ((uint16_t)payload[at + 1] << 8);
        at += 2;

        uint64_t created = 0;
        for (int b = 0; b < 8; b++) {
            created |= ((uint64_t)payload[at++]) << (b * 8);
        }

        if (klen == 0 || klen >= VAULT_MAX_KEY_LEN ||
            slen > VAULT_MAX_SECRET_LEN || mlen >= VAULT_MAX_META_LEN)
        {
            return VAULT_ERR_CORRUPT;
        }

        if (at + klen + slen + mlen > len) {
            return VAULT_ERR_CORRUPT;
        }

        if (db->count >= VAULT_MAX_RECORDS) {
            return VAULT_ERR_FULL;
        }

        vault_record_t *rec = &db->records[db->count++];
        memcpy(rec->key, payload + at, klen);
        rec->key[klen] = '\0';
        at += klen;

        rec->secret_type = stype;
        memcpy(rec->secret, payload + at, slen);
        rec->secret_len = slen;
        at += slen;

        memcpy(rec->meta, payload + at, mlen);
        rec->meta[mlen] = '\0';
        at += mlen;

        rec->created_at = created;
    }

    db->modified = false;
    return VAULT_ERR_OK;
}

#ifndef TOOL_HOST_TEST
static inline long sys_getrandom_bytes(void *buf, size_t len) {
    return tool_syscall(SYS_GETRANDOM, (uintptr_t)buf, len, 0);
}
#endif

int vault_file_load(const char *path, const char *passphrase, vault_db_t *db) {
    if (!path || !passphrase || !db) return VAULT_ERR_INVAL;

    long fd = tool_syscall(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        return VAULT_ERR_IO;
    }

    vault_header_t hdr;
    long n = tool_syscall(SYS_READ, fd, (uintptr_t)&hdr, sizeof(hdr));
    if (n != (long)sizeof(hdr)) {
        tool_syscall(SYS_CLOSE, fd, 0, 0);
        return VAULT_ERR_CORRUPT;
    }

    if (hdr.magic != VAULT_MAGIC || hdr.version != VAULT_VERSION ||
        hdr.kdf_type != VAULT_KDF_PBKDF2_SHA256)
    {
        tool_syscall(SYS_CLOSE, fd, 0, 0);
        return VAULT_ERR_CORRUPT;
    }

    if (hdr.payload_len > VAULT_MAX_PAYLOAD_LEN) {
        tool_syscall(SYS_CLOSE, fd, 0, 0);
        return VAULT_ERR_CORRUPT;
    }

    static uint8_t cipher_buf[VAULT_MAX_PAYLOAD_LEN];
    static uint8_t plain_buf[VAULT_MAX_PAYLOAD_LEN];

    size_t total_read = 0;
    while (total_read < hdr.payload_len) {
        long r = tool_syscall(SYS_READ, fd, (uintptr_t)(cipher_buf + total_read),
                              hdr.payload_len - total_read);
        if (r <= 0) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            return VAULT_ERR_IO;
        }
        total_read += (size_t)r;
    }
    tool_syscall(SYS_CLOSE, fd, 0, 0);

    /* Derive master key using PBKDF2-HMAC-SHA256 */
    uint8_t key[VAULT_KEY_LEN];
    int rc = vault_pbkdf2_sha256((const uint8_t *)passphrase, strlen(passphrase),
                                hdr.salt, VAULT_SALT_LEN, hdr.iterations, key);
    if (rc != VAULT_ERR_OK) {
        crypto_wipe(key, sizeof(key));
        return rc;
    }

    /* Decrypt ciphertext via ChaCha20-Poly1305 AEAD */
    rc = vault_crypto_decrypt(cipher_buf, hdr.payload_len,
                              (const uint8_t *)&hdr, VAULT_AD_LEN,
                              key, hdr.nonce, hdr.tag, plain_buf);
    crypto_wipe(key, sizeof(key));

    if (rc != VAULT_ERR_OK) {
        crypto_wipe(plain_buf, sizeof(plain_buf));
        return VAULT_ERR_AUTH; /* Wrong passphrase or tampered file */
    }

    /* Deserialize records into db */
    rc = vault_db_deserialize(db, plain_buf, hdr.payload_len);
    crypto_wipe(plain_buf, sizeof(plain_buf));

    if (rc == VAULT_ERR_OK) {
        db->iterations = hdr.iterations;
        memcpy(db->salt, hdr.salt, VAULT_SALT_LEN);
        db->modified = false;
    }

    return rc;
}

int vault_file_save(const char *path, const char *passphrase, vault_db_t *db) {
    if (!path || !passphrase || !db) return VAULT_ERR_INVAL;

    static uint8_t plain_buf[VAULT_MAX_PAYLOAD_LEN];
    static uint8_t cipher_buf[VAULT_MAX_PAYLOAD_LEN];

    size_t plain_len = 0;
    int rc = vault_db_serialize(db, plain_buf, sizeof(plain_buf), &plain_len);
    if (rc != VAULT_ERR_OK) {
        crypto_wipe(plain_buf, sizeof(plain_buf));
        return rc;
    }

    vault_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VAULT_MAGIC;
    hdr.version = VAULT_VERSION;
    hdr.kdf_type = VAULT_KDF_PBKDF2_SHA256;
    hdr.iterations = (db->iterations > 0) ? db->iterations : VAULT_DEFAULT_ITERATIONS;
    hdr.payload_len = (uint32_t)plain_len;

    /* Generate fresh salt and nonce */
#ifdef TOOL_HOST_TEST
    for (int i = 0; i < VAULT_SALT_LEN; i++) hdr.salt[i] = (uint8_t)rand();
    for (int i = 0; i < VAULT_NONCE_LEN; i++) hdr.nonce[i] = (uint8_t)rand();
#else
    if (sys_getrandom_bytes(hdr.salt, VAULT_SALT_LEN) != VAULT_SALT_LEN ||
        sys_getrandom_bytes(hdr.nonce, VAULT_NONCE_LEN) != VAULT_NONCE_LEN)
    {
        crypto_wipe(plain_buf, sizeof(plain_buf));
        return VAULT_ERR_IO;
    }
#endif
    memcpy(db->salt, hdr.salt, VAULT_SALT_LEN);

    /* Derive master key */
    uint8_t key[VAULT_KEY_LEN];
    rc = vault_pbkdf2_sha256((const uint8_t *)passphrase, strlen(passphrase),
                             hdr.salt, VAULT_SALT_LEN, hdr.iterations, key);
    if (rc != VAULT_ERR_OK) {
        crypto_wipe(key, sizeof(key));
        crypto_wipe(plain_buf, sizeof(plain_buf));
        return rc;
    }

    /* Encrypt via ChaCha20-Poly1305 */
    rc = vault_crypto_encrypt(plain_buf, plain_len,
                              (const uint8_t *)&hdr, VAULT_AD_LEN,
                              key, hdr.nonce, cipher_buf, hdr.tag);
    crypto_wipe(key, sizeof(key));
    crypto_wipe(plain_buf, sizeof(plain_buf));

    if (rc != VAULT_ERR_OK) {
        return rc;
    }

    /* Atomic persistence via temporary file and SYS_RENAME_REPLACE */
    char tmp_path[256];
    size_t plen = strlen(path);
    if (plen + 5 >= sizeof(tmp_path)) return VAULT_ERR_INVAL;
    memcpy(tmp_path, path, plen);
    memcpy(tmp_path + plen, ".tmp", 5);

    long fd = tool_syscall(SYS_OPEN, (uintptr_t)tmp_path,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0600);
    if (fd < 0) {
        return VAULT_ERR_IO;
    }

    /* Write header */
    long w = tool_syscall(SYS_WRITE, fd, (uintptr_t)&hdr, sizeof(hdr));
    if (w != (long)sizeof(hdr)) {
        tool_syscall(SYS_CLOSE, fd, 0, 0);
        tool_syscall(SYS_UNLINK, (uintptr_t)tmp_path, 0, 0);
        return VAULT_ERR_IO;
    }

    /* Write ciphertext */
    size_t written = 0;
    while (written < plain_len) {
        long nw = tool_syscall(SYS_WRITE, fd, (uintptr_t)(cipher_buf + written),
                               plain_len - written);
        if (nw <= 0) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_syscall(SYS_UNLINK, (uintptr_t)tmp_path, 0, 0);
            return VAULT_ERR_IO;
        }
        written += (size_t)nw;
    }

    /* Force data durability to disk before rename */
    tool_syscall(SYS_FSYNC, fd, 0, 0);
    tool_syscall(SYS_CLOSE, fd, 0, 0);

    /* Atomic rename replace */
    long ren = tool_syscall(SYS_RENAME_REPLACE, (uintptr_t)tmp_path, (uintptr_t)path, 0);
    if (ren < 0) {
        ren = tool_syscall(SYS_RENAME, (uintptr_t)tmp_path, (uintptr_t)path, 0);
    }
    if (ren < 0) {
        tool_syscall(SYS_UNLINK, (uintptr_t)tmp_path, 0, 0);
        return VAULT_ERR_IO;
    }

    /* Ensure strict 0600 permissions */
    tool_syscall(SYS_CHMOD, (uintptr_t)path, 0600, 0);

    db->modified = false;
    return VAULT_ERR_OK;
}

static vault_db_t s_helper_db;

int vault_get_secret(const char *path, const char *passphrase, const char *key,
                     uint8_t *out_buf, size_t max_len, size_t *out_len)
{
    int rc = vault_file_load(path, passphrase, &s_helper_db);
    if (rc != VAULT_ERR_OK) {
        vault_db_clear(&s_helper_db);
        return rc;
    }

    const vault_record_t *rec = vault_db_get_ref(&s_helper_db, key);
    if (!rec) {
        rc = VAULT_ERR_NOTFOUND;
    } else if (rec->secret_len > max_len) {
        rc = VAULT_ERR_NOMEM;
    } else {
        memcpy(out_buf, rec->secret, rec->secret_len);
        if (out_len) *out_len = rec->secret_len;
        rc = VAULT_ERR_OK;
    }
    vault_db_clear(&s_helper_db);
    return rc;
}

int vault_set_secret(const char *path, const char *passphrase, const char *key,
                     uint8_t type, const uint8_t *secret, size_t secret_len, const char *meta)
{
    int rc = vault_file_load(path, passphrase, &s_helper_db);
    if (rc != VAULT_ERR_OK) {
        /* If file does not exist, initialize empty db */
        vault_db_init_empty(&s_helper_db, VAULT_DEFAULT_ITERATIONS);
    }

    rc = vault_db_set(&s_helper_db, key, type, secret, secret_len, meta);
    if (rc == VAULT_ERR_OK) {
        rc = vault_file_save(path, passphrase, &s_helper_db);
    }
    vault_db_clear(&s_helper_db);
    return rc;
}
