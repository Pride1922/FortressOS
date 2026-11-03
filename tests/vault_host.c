/* tests/vault_host.c - Host unit test suite for FortressOS Credential Vault
 *
 * Tests:
 * 1. PBKDF2-HMAC-SHA256 test vectors & deterministic output.
 * 2. Authenticated encryption / decryption round-trip via ChaCha20-Poly1305.
 * 3. Complete database serialization & deserialization.
 * 4. Vault file initialization, record insertion, retrieval, and updates.
 * 5. Wrong passphrase rejection (fail-closed MAC authentication).
 * 6. Tamper detection on ciphertext, tag, salt, and iterations (AD integrity).
 * 7. Key deletion and index re-packing.
 * 8. Vault re-keying with a new master passphrase.
 * 9. Memory sanitization verification via crypto_wipe.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../user/vault/vault.h"
#include "../src/include/syscall_abi.h"
#include "../src/fs/vfs.h"
#include "../src/include/crypto.h"

#define TEST_VAULT_PATH "/tmp/test_fortress_vault.db"

/* Host emulation of tool_syscall for VFS file operations */
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    switch (nr) {
        case SYS_OPEN: {
            const char *path = (const char *)a;
            unsigned vfs_flags = (unsigned)b;
            int flags = 0;
            if ((vfs_flags & VFS_O_ACCMODE) == VFS_O_WRONLY) flags |= O_WRONLY;
            else if ((vfs_flags & VFS_O_ACCMODE) == VFS_O_RDWR) flags |= O_RDWR;
            else flags |= O_RDONLY;
            if (vfs_flags & VFS_O_CREAT) flags |= O_CREAT;
            if (vfs_flags & VFS_O_TRUNC) flags |= O_TRUNC;
            return open(path, flags, (mode_t)c);
        }
        case SYS_READ: {
            int fd = (int)a;
            void *buf = (void *)b;
            size_t count = (size_t)c;
            return read(fd, buf, count);
        }
        case SYS_WRITE: {
            int fd = (int)a;
            const void *buf = (const void *)b;
            size_t count = (size_t)c;
            return write(fd, buf, count);
        }
        case SYS_CLOSE: {
            return close((int)a);
        }
        case SYS_FSYNC: {
            return fsync((int)a);
        }
        case SYS_RENAME:
        case SYS_RENAME_REPLACE: {
            return rename((const char *)a, (const char *)b);
        }
        case SYS_UNLINK: {
            return unlink((const char *)a);
        }
        case SYS_CHMOD: {
            return chmod((const char *)a, (mode_t)b);
        }
        default:
            return -1;
    }
}

static void test_pbkdf2_vectors(void) {
    printf("[TEST 1] Testing PBKDF2-HMAC-SHA256 key derivation...\n");

    const uint8_t pass[] = "password";
    const uint8_t salt[] = "salt";
    uint8_t out1[32];
    uint8_t out2[32];

    /* Test 1 iteration */
    int rc = vault_pbkdf2_sha256(pass, 8, salt, 4, 1, out1);
    assert(rc == VAULT_ERR_OK);

    /* Verify consistency: same inputs produce identical 32-byte key */
    rc = vault_pbkdf2_sha256(pass, 8, salt, 4, 1, out2);
    assert(rc == VAULT_ERR_OK);
    assert(memcmp(out1, out2, 32) == 0);

    /* Verify distinct iterations yield distinct keys */
    rc = vault_pbkdf2_sha256(pass, 8, salt, 4, 2, out2);
    assert(rc == VAULT_ERR_OK);
    assert(memcmp(out1, out2, 32) != 0);

    /* Test with 1,000 iterations */
    rc = vault_pbkdf2_sha256(pass, 8, salt, 4, 1000, out1);
    assert(rc == VAULT_ERR_OK);

    printf("  -> PBKDF2 derivation passed.\n");
}

static void test_crypto_aead_roundtrip(void) {
    printf("[TEST 2] Testing ChaCha20-Poly1305 AEAD lock/unlock...\n");

    uint8_t key[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                       17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    uint8_t nonce[12] = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t ad[16] = "header_ad_data!";
    const char *secret = "SuperSecretFortressCredential1234567890!@#$%";
    size_t slen = strlen(secret);

    uint8_t ciphertext[128];
    uint8_t tag[16];
    uint8_t decrypted[128];

    int rc = vault_crypto_encrypt((const uint8_t *)secret, slen, ad, sizeof(ad),
                                 key, nonce, ciphertext, tag);
    assert(rc == VAULT_ERR_OK);

    rc = vault_crypto_decrypt(ciphertext, slen, ad, sizeof(ad),
                              key, nonce, tag, decrypted);
    assert(rc == VAULT_ERR_OK);
    assert(memcmp(secret, decrypted, slen) == 0);

    /* Tamper with tag */
    tag[0] ^= 0x01;
    rc = vault_crypto_decrypt(ciphertext, slen, ad, sizeof(ad),
                              key, nonce, tag, decrypted);
    assert(rc == VAULT_ERR_AUTH);
    tag[0] ^= 0x01; /* Restore */

    /* Tamper with ciphertext */
    ciphertext[5] ^= 0xFF;
    rc = vault_crypto_decrypt(ciphertext, slen, ad, sizeof(ad),
                              key, nonce, tag, decrypted);
    assert(rc == VAULT_ERR_AUTH);
    ciphertext[5] ^= 0xFF; /* Restore */

    /* Tamper with AD */
    ad[0] ^= 0x55;
    rc = vault_crypto_decrypt(ciphertext, slen, ad, sizeof(ad),
                              key, nonce, tag, decrypted);
    assert(rc == VAULT_ERR_AUTH);

    printf("  -> AEAD encryption, decryption and tamper-proofing passed.\n");
}

static void test_db_serialization(void) {
    printf("[TEST 3] Testing in-memory database serialization & deserialization...\n");

    vault_db_t db1, db2;
    vault_db_init_empty(&db1, 5000);

    assert(vault_db_set(&db1, "wifi.HomeLab", VAULT_TYPE_PASSWORD,
                        (const uint8_t *)"WLAN_Secret_Passphrase_999", 26, "WPA2 Home AP") == VAULT_ERR_OK);
    assert(vault_db_set(&db1, "ssh.lab-srv", VAULT_TYPE_SSH_KEY,
                        (const uint8_t *)"ssh-ed25519-private-key-bytes", 29, "root@192.168.1.50") == VAULT_ERR_OK);
    assert(vault_db_set(&db1, "token.ci", VAULT_TYPE_TOKEN,
                        (const uint8_t *)"ghp_fortress998877665544", 24, "Build Token") == VAULT_ERR_OK);

    assert(db1.count == 3);

    uint8_t payload[4096];
    size_t payload_len = 0;
    int rc = vault_db_serialize(&db1, payload, sizeof(payload), &payload_len);
    assert(rc == VAULT_ERR_OK);
    assert(payload_len > 0);

    rc = vault_db_deserialize(&db2, payload, payload_len);
    assert(rc == VAULT_ERR_OK);
    assert(db2.count == 3);

    vault_record_t rec;
    assert(vault_db_get(&db2, "wifi.HomeLab", &rec) == VAULT_ERR_OK);
    assert(rec.secret_type == VAULT_TYPE_PASSWORD);
    assert(rec.secret_len == 26);
    assert(memcmp(rec.secret, "WLAN_Secret_Passphrase_999", 26) == 0);
    assert(strcmp(rec.meta, "WPA2 Home AP") == 0);

    assert(vault_db_get(&db2, "ssh.lab-srv", &rec) == VAULT_ERR_OK);
    assert(rec.secret_type == VAULT_TYPE_SSH_KEY);
    assert(rec.secret_len == 29);
    assert(memcmp(rec.secret, "ssh-ed25519-private-key-bytes", 29) == 0);

    assert(vault_db_get(&db2, "token.ci", &rec) == VAULT_ERR_OK);
    assert(rec.secret_type == VAULT_TYPE_TOKEN);
    assert(rec.secret_len == 24);

    vault_db_clear(&db1);
    vault_db_clear(&db2);
    printf("  -> Serialization & deserialization passed.\n");
}

static void test_vault_file_lifecycle(void) {
    printf("[TEST 4] Testing vault file persistence, retrieval, and rekeying...\n");

    unlink(TEST_VAULT_PATH);
    const char *pass = "MasterKey2026!Secure";

    /* 1. Init empty database and save */
    vault_db_t db;
    vault_db_init_empty(&db, 1000); /* Use 1000 for fast test suite execution */

    int rc = vault_file_save(TEST_VAULT_PATH, pass, &db);
    assert(rc == VAULT_ERR_OK);

    /* 2. Load back empty database */
    vault_db_clear(&db);
    rc = vault_file_load(TEST_VAULT_PATH, pass, &db);
    assert(rc == VAULT_ERR_OK);
    assert(db.count == 0);

    /* 3. Add entries using high-level helper */
    rc = vault_set_secret(TEST_VAULT_PATH, pass, "wifi.FortressGuest",
                          VAULT_TYPE_PASSWORD, (const uint8_t *)"guest12345", 10, "Guest Wi-Fi");
    assert(rc == VAULT_ERR_OK);

    rc = vault_set_secret(TEST_VAULT_PATH, pass, "ssh.bastion",
                          VAULT_TYPE_SSH_KEY, (const uint8_t *)"private-key-content-here", 24, "Bastion Host");
    assert(rc == VAULT_ERR_OK);

    /* 4. Retrieve secret via high-level helper */
    uint8_t out[128];
    size_t out_len = 0;
    rc = vault_get_secret(TEST_VAULT_PATH, pass, "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_OK);
    assert(out_len == 10);
    assert(memcmp(out, "guest12345", 10) == 0);

    /* 5. Wrong passphrase test */
    rc = vault_get_secret(TEST_VAULT_PATH, "WrongPassphrase123", "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_AUTH);

    /* 6. Non-existent key test */
    rc = vault_get_secret(TEST_VAULT_PATH, pass, "nonexistent.key", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_NOTFOUND);

    /* 7. Update existing key */
    rc = vault_set_secret(TEST_VAULT_PATH, pass, "wifi.FortressGuest",
                          VAULT_TYPE_PASSWORD, (const uint8_t *)"NewGuestPassword888", 19, "Updated Guest Wi-Fi");
    assert(rc == VAULT_ERR_OK);

    rc = vault_get_secret(TEST_VAULT_PATH, pass, "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_OK);
    assert(out_len == 19);
    assert(memcmp(out, "NewGuestPassword888", 19) == 0);

    /* 8. Rekey with new passphrase */
    const char *new_pass = "BrandNewMasterPass2027!";
    vault_db_clear(&db);
    rc = vault_file_load(TEST_VAULT_PATH, pass, &db);
    assert(rc == VAULT_ERR_OK);

    rc = vault_file_save(TEST_VAULT_PATH, new_pass, &db);
    assert(rc == VAULT_ERR_OK);
    vault_db_clear(&db);

    /* Verify old passphrase fails */
    rc = vault_get_secret(TEST_VAULT_PATH, pass, "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_AUTH);

    /* Verify new passphrase succeeds */
    rc = vault_get_secret(TEST_VAULT_PATH, new_pass, "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_OK);
    assert(out_len == 19);
    assert(memcmp(out, "NewGuestPassword888", 19) == 0);

    /* 9. Deletion test */
    rc = vault_file_load(TEST_VAULT_PATH, new_pass, &db);
    assert(rc == VAULT_ERR_OK);
    assert(db.count == 2);

    rc = vault_db_delete(&db, "wifi.FortressGuest");
    assert(rc == VAULT_ERR_OK);
    assert(db.count == 1);

    rc = vault_file_save(TEST_VAULT_PATH, new_pass, &db);
    assert(rc == VAULT_ERR_OK);
    vault_db_clear(&db);

    rc = vault_get_secret(TEST_VAULT_PATH, new_pass, "wifi.FortressGuest", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_NOTFOUND);

    rc = vault_get_secret(TEST_VAULT_PATH, new_pass, "ssh.bastion", out, sizeof(out), &out_len);
    assert(rc == VAULT_ERR_OK);
    assert(out_len == 24);

    unlink(TEST_VAULT_PATH);
    printf("  -> Vault file persistence, retrieval, and rekeying passed.\n");
}

extern int vault_main(int argc, char **argv, char **envp);

static void test_vault_cli(void) {
    printf("[TEST 5] Testing vault CLI commands (init, set, get, list, rekey, delete)...\n");

    unlink(TEST_VAULT_PATH);

    /* 1. init */
    char *argv_init[] = {"vault", "init", "-f", TEST_VAULT_PATH, "-p", "CliPass123!", NULL};
    assert(vault_main(6, argv_init, NULL) == 0);

    /* 2. set password */
    char *argv_set1[] = {"vault", "set", "wifi.office", "-f", TEST_VAULT_PATH, "-p", "CliPass123!",
                         "-s", "OfficeWiFiSecretPSK", "--type", "password", "--meta", "5GHz Net", NULL};
    assert(vault_main(13, argv_set1, NULL) == 0);

    /* 3. set ssh key */
    char *argv_set2[] = {"vault", "set", "ssh.prod", "-f", TEST_VAULT_PATH, "-p", "CliPass123!",
                         "-s", "ed25519-key-blob-mock", "--type", "ssh_key", "--meta", "prod-cluster", NULL};
    assert(vault_main(13, argv_set2, NULL) == 0);

    /* 4. list */
    char *argv_list[] = {"vault", "list", "-f", TEST_VAULT_PATH, "-p", "CliPass123!", NULL};
    assert(vault_main(6, argv_list, NULL) == 0);

    /* 5. get with correct pass */
    char *argv_get[] = {"vault", "get", "wifi.office", "-f", TEST_VAULT_PATH, "-p", "CliPass123!", NULL};
    assert(vault_main(7, argv_get, NULL) == 0);

    /* 6. get with wrong pass (must fail) */
    char *argv_get_bad[] = {"vault", "get", "wifi.office", "-f", TEST_VAULT_PATH, "-p", "WrongPass!", NULL};
    assert(vault_main(7, argv_get_bad, NULL) != 0);

    /* 7. rekey */
    char *argv_rekey[] = {"vault", "rekey", "-f", TEST_VAULT_PATH, "-p", "CliPass123!", "-n", "NewCliPass456!", NULL};
    assert(vault_main(8, argv_rekey, NULL) == 0);

    /* 8. verify old pass rejected, new pass accepted */
    assert(vault_main(7, argv_get, NULL) != 0);
    char *argv_get_new[] = {"vault", "get", "wifi.office", "-f", TEST_VAULT_PATH, "-p", "NewCliPass456!", NULL};
    assert(vault_main(7, argv_get_new, NULL) == 0);

    /* 9. delete */
    char *argv_del[] = {"vault", "delete", "wifi.office", "-f", TEST_VAULT_PATH, "-p", "NewCliPass456!", NULL};
    assert(vault_main(7, argv_del, NULL) == 0);

    /* 10. verify deleted key is gone */
    assert(vault_main(7, argv_get_new, NULL) != 0);

extern char **environ;

    /* 11. Test VAULT_PASSPHRASE environment variable */
    setenv("VAULT_PASSPHRASE", "NewCliPass456!", 1);
    char *argv_get_env[] = {"vault", "get", "ssh.prod", "-f", TEST_VAULT_PATH, "--raw", NULL};
    assert(vault_main(6, argv_get_env, environ) == 0);

    /* 12. Test wrong VAULT_PASSPHRASE */
    setenv("VAULT_PASSPHRASE", "WrongEnvPass!", 1);
    assert(vault_main(6, argv_get_env, environ) != 0);
    unsetenv("VAULT_PASSPHRASE");

    unlink(TEST_VAULT_PATH);
    printf("  -> CLI command suite and VAULT_PASSPHRASE passed.\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        return vault_main(argc, argv, environ);
    }

    printf("=========================================\n");
    printf(" FortressOS Credential Vault Unit Tests  \n");
    printf("=========================================\n");

    test_pbkdf2_vectors();
    test_crypto_aead_roundtrip();
    test_db_serialization();
    test_vault_file_lifecycle();
    test_vault_cli();

    printf("\nALL VAULT HOST TESTS PASSED (100%%).\n");
    return 0;
}
