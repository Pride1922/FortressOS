#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

static void safe_strcpy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

#include "user/sshd/monitor.h"
#include "user/sshd/auth.h"
#include "user/sshd/ssh_packet.h"
#include "src/include/crypto.h"
#include "user/tools/userdb.h"

static bool g_mock_db_check_valid = true;
bool db_check(void) { return g_mock_db_check_valid; }
bool db_busy(void) { return false; }
bool db_load(userdb_t *out, bool shadow) { (void)out; (void)shadow; return true; }

static uint64_t g_test_rng = 123456789ULL;
int64_t crypto_test_getrandom(void *p, size_t n, uint64_t flags) {
    (void)flags;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        g_test_rng = g_test_rng * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i] = (uint8_t)(g_test_rng >> 33);
    }
    return (int64_t)n;
}

void crypto_test_wiped(const void *p, size_t n) {
    (void)p; (void)n;
}

static void test_monitor_ipc_framing(void) {
    printf("Running test_monitor_ipc_framing...\n");

    int pfd[2];
    assert(pipe(pfd) == 0);

    /* 1. Normal round-trip message */
    uint8_t payload[64] = "Hello FortressOS Monitor IPC!";
    uint16_t plen = (uint16_t)strlen((char *)payload);

    assert(monitor_send_msg(pfd[1], MONITOR_REQ_SIGN_HASH, 0, payload, plen) == 0);

    monitor_hdr_t r_hdr;
    uint8_t r_buf[128];
    assert(monitor_recv_msg(pfd[0], &r_hdr, r_buf, sizeof(r_buf)) == 0);
    assert(r_hdr.magic == MONITOR_MAGIC);
    assert(r_hdr.msg_type == MONITOR_REQ_SIGN_HASH);
    assert(r_hdr.session_id == 0);
    assert(r_hdr.payload_len == plen);
    assert(memcmp(r_buf, payload, plen) == 0);

    /* 2. Zero-length payload */
    assert(monitor_send_msg(pfd[1], MONITOR_REQ_SPAWN_SESSION, 1, NULL, 0) == 0);
    assert(monitor_recv_msg(pfd[0], &r_hdr, r_buf, sizeof(r_buf)) == 0);
    assert(r_hdr.magic == MONITOR_MAGIC);
    assert(r_hdr.msg_type == MONITOR_REQ_SPAWN_SESSION);
    assert(r_hdr.session_id == 1);
    assert(r_hdr.payload_len == 0);

    /* 3. Oversized payload rejected by sender */
    assert(monitor_send_msg(pfd[1], MONITOR_REQ_SIGN_HASH, 0, payload, MONITOR_MAX_PAYLOAD + 1) < 0);

    /* 4. Corrupted magic rejected by receiver */
    monitor_hdr_t bad_hdr = {
        .magic = 0xDEADBEEF,
        .msg_type = MONITOR_REQ_SIGN_HASH,
        .session_id = 0,
        .payload_len = 0,
        .reserved = 0
    };
    assert(write(pfd[1], &bad_hdr, sizeof(bad_hdr)) == sizeof(bad_hdr));
    assert(monitor_recv_msg(pfd[0], &r_hdr, r_buf, sizeof(r_buf)) < 0);

    close(pfd[0]);
    close(pfd[1]);
    printf("PASS: test_monitor_ipc_framing\n");
}

static void test_monitor_signing_oracle(void) {
    printf("Running test_monitor_signing_oracle...\n");

    /* Create dummy host Ed25519 secret key from seed */
    uint8_t seed[32] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t host_pub[32], host_sec[64];
    assert(crypto_ed25519_from_seed(seed, host_pub, host_sec) == 0);

    monitor_ctx_t mon;
    monitor_init(&mon, host_sec);

    uint8_t hash[32];
    for (int i = 0; i < 32; i++) hash[i] = (uint8_t)(0xAA + i);

    monitor_hdr_t req_hdr = {
        .magic = MONITOR_MAGIC,
        .msg_type = MONITOR_REQ_SIGN_HASH,
        .session_id = 0,
        .payload_len = 32,
        .reserved = 0
    };

    uint8_t resp_type = 0;
    uint8_t resp_payload[256];
    uint16_t resp_len = 0;

    /* 1. First signature request for KEX round on conn 0 succeeds */
    assert(monitor_handle_request(&mon, &req_hdr, hash, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_SIGN_HASH);
    assert(resp_len == 4 + 64);
    uint32_t status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_OK);
    uint8_t *sig = resp_payload + 4;

    /* Verify signature is valid over H */
    assert(crypto_ed25519_verify(sig, host_pub, hash, 32) == 0);

    /* 2. Second signature request in same KEX round MUST BE REJECTED (signing oracle restriction) */
    assert(monitor_handle_request(&mon, &req_hdr, hash, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_SIGN_HASH);
    status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_ERR_ORACLE);

    /* 3. Arbitrary size request (length != 32) MUST BE REJECTED */
    req_hdr.session_id = 1; /* conn 1 has not signed yet */
    req_hdr.payload_len = 64;
    uint8_t long_data[64] = {0};
    assert(monitor_handle_request(&mon, &req_hdr, long_data, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_ERROR);
    status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_ERR_ARG);

    printf("PASS: test_monitor_signing_oracle\n");
}

static void test_password_auth(void) {
    printf("Running test_password_auth...\n");

    monitor_ctx_t mon;
    monitor_init(&mon, NULL);

    /* Construct in-memory userdb */
    userdb_t db;
    memset(&db, 0, sizeof(db));

    /* User 0: alice (normal user) */
    db.users[0] = (db_user_t){.name = "alice", .home = "/home/alice", .shell = "/bin/sh", .uid = 1001, .gid = 1001};
    db.groups[0] = (db_group_t){.name = "alice", .members = "", .gid = 1001};
    char alice_hash[78];
    assert(db_crypt("secret123", "1234567890abcdef", alice_hash));
    db.shadows[0] = (db_shadow_t){.name = "alice", .restricted = false};
    safe_strcpy(db.shadows[0].hash, alice_hash, sizeof(db.shadows[0].hash));

    /* User 1: bob (locked account) */
    db.users[1] = (db_user_t){.name = "bob", .home = "/home/bob", .shell = "/bin/sh", .uid = 1002, .gid = 1002};
    db.groups[1] = (db_group_t){.name = "bob", .members = "", .gid = 1002};
    db.shadows[1] = (db_shadow_t){.name = "bob", .restricted = false, .hash = "!locked_hash"};

    /* User 2: root */
    db.users[2] = (db_user_t){.name = "root", .home = "/root", .shell = "/bin/sh", .uid = 0, .gid = 0};
    db.groups[2] = (db_group_t){.name = "root", .members = "", .gid = 0};
    char root_hash[78];
    assert(db_crypt("rootpass", "1234567890abcdef", root_hash));
    db.shadows[2] = (db_shadow_t){.name = "root", .restricted = false};
    safe_strcpy(db.shadows[2].hash, root_hash, sizeof(db.shadows[2].hash));

    db.nusers = 3;
    db.ngroups = 3;
    db.nshadows = 3;
    assert(monitor_set_userdb(&mon, &db) == 0);

    /* Helper to build REQ_AUTH_PASSWORD */
    uint8_t req_buf[256];
    uint8_t resp_type;
    uint8_t resp_payload[512];
    uint16_t resp_len;

    monitor_hdr_t hdr = {
        .magic = MONITOR_MAGIC,
        .msg_type = MONITOR_REQ_AUTH_PASSWORD,
        .session_id = 0,
        .payload_len = 0,
        .reserved = 0
    };

    /* 1. Valid password for alice */
    {
        uint32_t ulen = 5;
        uint32_t plen = 9;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "secret123", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_OK);
        assert(mon.conns[0].authenticated == true);
        assert(mon.conns[0].uid == 1001);
        assert(mon.conns[0].gid == 1001);
        assert(strcmp(mon.conns[0].home, "/home/alice") == 0);
    }

    /* 2. Invalid password for alice */
    {
        mon.conns[0].authenticated = false;
        mon.conns[0].auth_attempts = 0;
        uint32_t ulen = 5;
        uint32_t plen = 9;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "wrongpass", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL);
        assert(mon.conns[0].auth_attempts == 1);
        assert(mon.conns[0].authenticated == false);
    }

    /* 3. Locked account bob */
    {
        uint32_t ulen = 3;
        uint32_t plen = 4;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "bob", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "test", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL);
    }

    /* 4. Root remote login rejected with constant-time dummy verify */
    {
        uint32_t ulen = 4;
        uint32_t plen = 8;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "root", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "rootpass", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL);
    }

    /* 5. Empty password rejected */
    {
        uint32_t ulen = 5;
        uint32_t plen = 0;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL);
    }

    /* 6. Unknown user charlie executes dummy verification path */
    {
        uint32_t ulen = 7;
        uint32_t plen = 4;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "charlie", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "pass", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL);
    }

    /* 7. Max attempt cap: 5 attempts max */
    {
        mon.conns[0].auth_attempts = 5;
        uint32_t ulen = 5;
        uint32_t plen = 9;
        size_t off = 0;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        memcpy(req_buf + off, &plen, 4); off += 4;
        memcpy(req_buf + off, "secret123", plen); off += plen;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PASSWORD);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_DENIED);
    }

    printf("PASS: test_password_auth\n");
}

/* Helper to generate base64 string */
static void make_b64_pubkey_blob(const uint8_t pk[32], char *out_b64) {
    uint8_t blob[51];
    blob[0] = 0; blob[1] = 0; blob[2] = 0; blob[3] = 11;
    memcpy(blob + 4, "ssh-ed25519", 11);
    blob[15] = 0; blob[16] = 0; blob[17] = 0; blob[18] = 32;
    memcpy(blob + 19, pk, 32);

    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < 51; i += 3) {
        uint32_t v = (uint32_t)blob[i] << 16;
        size_t left = 51 - i;
        if (left > 1) v |= (uint32_t)blob[i + 1] << 8;
        if (left > 2) v |= blob[i + 2];
        out_b64[at++] = alphabet[v >> 18];
        out_b64[at++] = alphabet[(v >> 12) & 63];
        if (left > 1) out_b64[at++] = alphabet[(v >> 6) & 63]; else out_b64[at++] = '=';
        if (left > 2) out_b64[at++] = alphabet[v & 63]; else out_b64[at++] = '=';
    }
    out_b64[at] = '\0';
}

static void test_authorized_keys_parser(void) {
    printf("Running test_authorized_keys_parser...\n");

    uint8_t pk[32] = {0x12, 0x34, 0x56, 0x78};
    char b64[128];
    make_b64_pubkey_blob(pk, b64);

    /* 1. Normal line with comment */
    char line1[256];
    snprintf(line1, sizeof(line1), "ssh-ed25519 %s user@host\n", b64);
    uint8_t out_pk[32];
    assert(monitor_parse_authorized_key_line(line1, strlen(line1), out_pk) == 0);
    assert(crypto_verify32(out_pk, pk) == 0);

    /* 2. Line with leading spaces and tabs */
    char line2[256];
    snprintf(line2, sizeof(line2), "  \t ssh-ed25519   %s   my key comment\n", b64);
    assert(monitor_parse_authorized_key_line(line2, strlen(line2), out_pk) == 0);
    assert(crypto_verify32(out_pk, pk) == 0);

    /* 3. Comment lines and blank lines return 1 (skip) */
    assert(monitor_parse_authorized_key_line("# this is a comment", 19, out_pk) == 1);
    assert(monitor_parse_authorized_key_line("   # indented comment", 21, out_pk) == 1);
    assert(monitor_parse_authorized_key_line("   \n", 4, out_pk) == 1);
    assert(monitor_parse_authorized_key_line("", 0, out_pk) == 1);

    /* 4. Unsupported algorithm rejection */
    assert(monitor_parse_authorized_key_line("ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQ... comment\n", 50, out_pk) < 0);

    /* 5. Malformed base64 */
    assert(monitor_parse_authorized_key_line("ssh-ed25519 !!!invalid_base64!!! comment\n", 41, out_pk) < 0);

    printf("PASS: test_authorized_keys_parser\n");
}

static void test_pubkey_auth_and_permissions(void) {
    printf("Running test_pubkey_auth_and_permissions...\n");

    /* Setup temporary test directory */
    char tmp_home[] = "/tmp/fortress_ssh_test_XXXXXX";
    assert(mkdtemp(tmp_home) != NULL);

    char ssh_dir[256];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", tmp_home);
    assert(mkdir(ssh_dir, 0700) == 0);

    char ak_file[256];
    snprintf(ak_file, sizeof(ak_file), "%s/.ssh/authorized_keys", tmp_home);
    int fd = open(ak_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);

    uint8_t pk_alice[32] = {0xAA, 0xBB, 0xCC, 0xDD};
    char b64_alice[128];
    make_b64_pubkey_blob(pk_alice, b64_alice);

    char entry[256];
    int elen = snprintf(entry, sizeof(entry), "# Authorized keys for test\nssh-ed25519 %s alice@test\n", b64_alice);
    assert(write(fd, entry, elen) == elen);
    close(fd);

    uint32_t my_uid = (uint32_t)getuid();

    /* 1. Correct permissions (dir 0700, file 0600) -> valid key matches */
    assert(monitor_check_authorized_keys(tmp_home, my_uid, pk_alice) == 0);

    /* 2. Key not in file fails */
    uint8_t pk_unknown[32] = {0x99, 0x88, 0x77, 0x66};
    assert(monitor_check_authorized_keys(tmp_home, my_uid, pk_unknown) < 0);

    /* 3. Strict directory permissions: chmod 0755 on .ssh MUST FAIL */
    assert(chmod(ssh_dir, 0755) == 0);
    assert(monitor_check_authorized_keys(tmp_home, my_uid, pk_alice) < 0);
    assert(chmod(ssh_dir, 0700) == 0);

    /* 4. Strict file permissions: chmod 0644 on authorized_keys MUST FAIL */
    assert(chmod(ak_file, 0644) == 0);
    assert(monitor_check_authorized_keys(tmp_home, my_uid, pk_alice) < 0);
    assert(chmod(ak_file, 0600) == 0);

    /* 5. Directory traversal rejection */
    assert(monitor_check_authorized_keys("/tmp/../etc", my_uid, pk_alice) < 0);

    /* Clean up fixture files */
    unlink(ak_file);
    rmdir(ssh_dir);
    rmdir(tmp_home);

    printf("PASS: test_pubkey_auth_and_permissions\n");
}

static void test_pubkey_signature_and_session_binding(void) {
    printf("Running test_pubkey_signature_and_session_binding...\n");

    /* Create temporary test home */
    char tmp_home[] = "/tmp/fortress_ssh_sess_XXXXXX";
    assert(mkdtemp(tmp_home) != NULL);
    char ssh_dir[256], ak_file[256];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", tmp_home);
    assert(mkdir(ssh_dir, 0700) == 0);
    snprintf(ak_file, sizeof(ak_file), "%s/.ssh/authorized_keys", tmp_home);
    int fd = open(ak_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);

    uint8_t seed[32] = {0x42, 0x11, 0x22, 0x33, 0x44};
    uint8_t client_pub[32], client_sec[64];
    assert(crypto_ed25519_from_seed(seed, client_pub, client_sec) == 0);

    char b64[128];
    make_b64_pubkey_blob(client_pub, b64);
    char entry[256];
    int elen = snprintf(entry, sizeof(entry), "ssh-ed25519 %s client@session\n", b64);
    assert(write(fd, entry, elen) == elen);
    close(fd);

    /* Setup monitor */
    monitor_ctx_t mon;
    monitor_init(&mon, NULL);
    userdb_t db;
    memset(&db, 0, sizeof(db));
    db.users[0] = (db_user_t){.name = "alice", .uid = (uint32_t)getuid(), .gid = (uint32_t)getgid()};
    safe_strcpy(db.users[0].home, tmp_home, sizeof(db.users[0].home));
    safe_strcpy(db.users[0].shell, "/bin/sh", sizeof(db.users[0].shell));
    db.shadows[0] = (db_shadow_t){.name = "alice", .restricted = false, .hash = "$5$rounds=5000$salt$hash"};
    db.groups[0] = (db_group_t){.name = "alice", .gid = (uint32_t)getgid()};
    db.nusers = 1; db.ngroups = 1; db.nshadows = 1;
    assert(monitor_set_userdb(&mon, &db) == 0);

    /* Construct public key blob */
    uint8_t pk_blob[51];
    pk_blob[0] = 0; pk_blob[1] = 0; pk_blob[2] = 0; pk_blob[3] = 11;
    memcpy(pk_blob + 4, "ssh-ed25519", 11);
    pk_blob[15] = 0; pk_blob[16] = 0; pk_blob[17] = 0; pk_blob[18] = 32;
    memcpy(pk_blob + 19, client_pub, 32);

    /* Session A setup */
    uint8_t session_id_A[32] = {0xAA, 0x11, 0x22};
    uint8_t signed_data_A[512];
    size_t signed_data_A_len = 0;
    assert(auth_build_signed_data(session_id_A, "alice", "ssh-connection", "ssh-ed25519",
                                  pk_blob, 51,
                                  signed_data_A, sizeof(signed_data_A), &signed_data_A_len) == 0);

    uint8_t sig_A[64];
    assert(crypto_ed25519_sign(sig_A, client_sec, signed_data_A, signed_data_A_len) == 0);

    /* Probe check on Session A */
    uint8_t req_buf[1024];
    uint8_t resp_type;
    uint8_t resp_payload[512];
    uint16_t resp_len;
    monitor_hdr_t hdr = {.magic = MONITOR_MAGIC, .msg_type = MONITOR_REQ_AUTH_PUBKEY, .session_id = 0};

    {
        size_t off = 0;
        uint32_t ulen = 5;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        req_buf[off++] = 1; /* check_only = 1 */
        memcpy(req_buf + off, client_pub, 32); off += 32;
        memset(req_buf + off, 0, 64); off += 64;
        uint16_t zero_len = 0;
        memcpy(req_buf + off, &zero_len, 2); off += 2;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PUBKEY);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_PK_OK);
    }

    /* Valid signature on Session A */
    {
        size_t off = 0;
        uint32_t ulen = 5;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        req_buf[off++] = 0; /* check_only = 0 */
        memcpy(req_buf + off, client_pub, 32); off += 32;
        memcpy(req_buf + off, sig_A, 64); off += 64;
        uint16_t sd_len = (uint16_t)signed_data_A_len;
        memcpy(req_buf + off, &sd_len, 2); off += 2;
        memcpy(req_buf + off, signed_data_A, signed_data_A_len); off += signed_data_A_len;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PUBKEY);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_OK);
    }

    /* REPLAY ATTACK TEST:
     * Try to authenticate on Session B (different session_id) using sig_A from Session A.
     * The worker / monitor builds signed_data bound to Session B's session_id.
     * Verification MUST FAIL! */
    uint8_t session_id_B[32] = {0xBB, 0x33, 0x44};
    uint8_t signed_data_B[512];
    size_t signed_data_B_len = 0;
    assert(auth_build_signed_data(session_id_B, "alice", "ssh-connection", "ssh-ed25519",
                                  pk_blob, 51,
                                  signed_data_B, sizeof(signed_data_B), &signed_data_B_len) == 0);

    {
        hdr.session_id = 1;
        mon.conns[1].auth_attempts = 0;
        size_t off = 0;
        uint32_t ulen = 5;
        memcpy(req_buf + off, &ulen, 4); off += 4;
        memcpy(req_buf + off, "alice", ulen); off += ulen;
        req_buf[off++] = 0; /* check_only = 0 */
        memcpy(req_buf + off, client_pub, 32); off += 32;
        memcpy(req_buf + off, sig_A, 64); off += 64; /* Replaying signature from Session A! */
        uint16_t sd_len = (uint16_t)signed_data_B_len;
        memcpy(req_buf + off, &sd_len, 2); off += 2;
        memcpy(req_buf + off, signed_data_B, signed_data_B_len); off += signed_data_B_len;
        hdr.payload_len = (uint16_t)off;

        assert(monitor_handle_request(&mon, &hdr, req_buf, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
        assert(resp_type == MONITOR_RESP_AUTH_PUBKEY);
        uint32_t status = *(uint32_t *)resp_payload;
        assert(status == MONITOR_STATUS_AUTH_FAIL); /* Replay attack defeated! */
    }

    unlink(ak_file);
    rmdir(ssh_dir);
    rmdir(tmp_home);

    printf("PASS: test_pubkey_signature_and_session_binding\n");
}

static void test_auth_protocol_engine(void) {
    printf("Running test_auth_protocol_engine...\n");

    int p_w2m[2], p_m2w[2];
    assert(pipe(p_w2m) == 0);
    assert(pipe(p_m2w) == 0);

    /* Setup monitor */
    monitor_ctx_t mon;
    monitor_init(&mon, NULL);
    userdb_t db;
    memset(&db, 0, sizeof(db));
    db.users[0] = (db_user_t){.name = "alice", .home = "/home/alice", .shell = "/bin/sh", .uid = 1001, .gid = 1001};
    db.groups[0] = (db_group_t){.name = "alice", .gid = 1001};
    char alice_hash[78];
    assert(db_crypt("secret123", "1234567890abcdef", alice_hash));
    db.shadows[0] = (db_shadow_t){.name = "alice", .restricted = false};
    safe_strcpy(db.shadows[0].hash, alice_hash, sizeof(db.shadows[0].hash));
    db.nusers = 1; db.ngroups = 1; db.nshadows = 1;
    assert(monitor_set_userdb(&mon, &db) == 0);

    /* Setup auth worker engine */
    uint8_t session_id[32] = {0x55, 0x66, 0x77};
    ssh_auth_ctx_t auth;
    auth_init(&auth, session_id, 0, p_m2w[0], p_w2m[1]);

    /* 1. Service request: "ssh-userauth" */
    sshbuf_t s_req, s_reply;
    assert(sshbuf_init(&s_req, 64, 256) == 0);
    assert(sshbuf_init(&s_reply, 64, 256) == 0);

    assert(sshbuf_put_u8(&s_req, SSH_MSG_SERVICE_REQUEST) == 0);
    assert(sshbuf_put_cstring(&s_req, "ssh-userauth") == 0);

    assert(auth_process_service_request(&auth, sshbuf_ptr(&s_req), sshbuf_len(&s_req), &s_reply) == 0);
    assert(auth.state == SSH_AUTH_STATE_SERVICE_REQUESTED);

    /* Check reply is SSH_MSG_SERVICE_ACCEPT */
    uint8_t msg_type = 0;
    assert(sshbuf_get_u8(&s_reply, &msg_type) == 0);
    assert(msg_type == SSH_MSG_SERVICE_ACCEPT);
    const uint8_t *svc = NULL; size_t slen = 0;
    assert(sshbuf_get_string_ptr(&s_reply, &svc, &slen) == 0);
    assert(slen == 12 && memcmp(svc, "ssh-userauth", 12) == 0);

    /* 2. Method "none" query */
    sshbuf_reset(&s_req);
    sshbuf_reset(&s_reply);
    assert(sshbuf_put_u8(&s_req, SSH_MSG_USERAUTH_REQUEST) == 0);
    assert(sshbuf_put_cstring(&s_req, "alice") == 0);
    assert(sshbuf_put_cstring(&s_req, "ssh-connection") == 0);
    assert(sshbuf_put_cstring(&s_req, "none") == 0);

    assert(auth_process_request(&auth, sshbuf_ptr(&s_req), sshbuf_len(&s_req), &s_reply) == 0);
    assert(sshbuf_get_u8(&s_reply, &msg_type) == 0);
    assert(msg_type == SSH_MSG_USERAUTH_FAILURE);

    /* 3. Method "password" - valid password */
    /* In a thread or helper simulation: when auth_process_request sends IPC to monitor,
     * we handle it and send back the response */
    sshbuf_reset(&s_req);
    sshbuf_reset(&s_reply);
    assert(sshbuf_put_u8(&s_req, SSH_MSG_USERAUTH_REQUEST) == 0);
    assert(sshbuf_put_cstring(&s_req, "alice") == 0);
    assert(sshbuf_put_cstring(&s_req, "ssh-connection") == 0);
    assert(sshbuf_put_cstring(&s_req, "password") == 0);
    assert(sshbuf_put_u8(&s_req, 0) == 0); /* change_pass = FALSE */
    assert(sshbuf_put_cstring(&s_req, "secret123") == 0);

    /* We run a child process or simulate pipe interaction */
    pid_t pid = fork();
    if (pid == 0) {
        /* Monitor server loop */
        monitor_hdr_t m_hdr;
        uint8_t m_req[512];
        if (monitor_recv_msg(p_w2m[0], &m_hdr, m_req, sizeof(m_req)) == 0) {
            uint8_t r_type;
            uint8_t r_pay[512];
            uint16_t r_len;
            monitor_handle_request(&mon, &m_hdr, m_req, &r_type, r_pay, &r_len, sizeof(r_pay));
            monitor_send_msg(p_m2w[1], r_type, m_hdr.session_id, r_pay, r_len);
        }
        _exit(0);
    }

    assert(auth_process_request(&auth, sshbuf_ptr(&s_req), sshbuf_len(&s_req), &s_reply) == 0);
    assert(auth.state == SSH_AUTH_STATE_SUCCESS);
    assert(auth.authenticated == true);
    assert(auth.uid == 1001);
    assert(auth.gid == 1001);

    assert(sshbuf_get_u8(&s_reply, &msg_type) == 0);
    assert(msg_type == SSH_MSG_USERAUTH_SUCCESS);

    /* 4. Banner generation */
    sshbuf_reset(&s_reply);
    assert(auth_build_banner("Authorized Access Only\n", &s_reply) == 0);
    assert(sshbuf_get_u8(&s_reply, &msg_type) == 0);
    assert(msg_type == SSH_MSG_USERAUTH_BANNER);

    sshbuf_free(&s_req);
    sshbuf_free(&s_reply);
    auth_cleanup(&auth);

    close(p_w2m[0]); close(p_w2m[1]);
    close(p_m2w[0]); close(p_m2w[1]);

    printf("PASS: test_auth_protocol_engine\n");
}

static void test_spawn_session_credentials(void) {
    printf("Running test_spawn_session_credentials...\n");

    monitor_ctx_t mon;
    monitor_init(&mon, NULL);

    userdb_t db;
    memset(&db, 0, sizeof(db));
    db.users[0] = (db_user_t){.name = "alice", .home = "/home/alice", .shell = "/bin/sh", .uid = 1001, .gid = 1001};
    db.groups[0] = (db_group_t){.name = "alice", .gid = 1001};
    db.shadows[0] = (db_shadow_t){.name = "alice", .restricted = false, .hash = "$5$rounds=5000$salt$hash"};
    db.nusers = 1; db.ngroups = 1; db.nshadows = 1;
    assert(monitor_set_userdb(&mon, &db) == 0);

    /* 1. Unauthenticated connection rejected */
    monitor_hdr_t hdr = {
        .magic = MONITOR_MAGIC,
        .msg_type = MONITOR_REQ_SPAWN_SESSION,
        .session_id = 0,
        .payload_len = 0
    };
    uint8_t resp_type;
    uint8_t resp_payload[64];
    uint16_t resp_len;

    assert(monitor_handle_request(&mon, &hdr, NULL, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_SPAWN_SESSION);
    uint32_t status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_ERR_PERM);

    /* 2. Authenticated connection prepares credentials snapshot */
    mon.conns[0].authenticated = true;
    safe_strcpy(mon.conns[0].username, "alice", sizeof(mon.conns[0].username));
    mon.conns[0].uid = 1001;
    mon.conns[0].gid = 1001;
    mon.conns[0].ngroups = 1;
    mon.conns[0].groups[0] = 1001;

    assert(monitor_handle_request(&mon, &hdr, NULL, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_SPAWN_SESSION);
    status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_OK);
    int64_t child_pid = *(int64_t *)(resp_payload + 4);
    assert(child_pid > 0);

    /* 3. Stale account check rejection per CREDENTIAL_SNAPSHOT_PROTOCOL.md */
    g_mock_db_check_valid = false;
    assert(monitor_handle_request(&mon, &hdr, NULL, &resp_type, resp_payload, &resp_len, sizeof(resp_payload)) == 0);
    assert(resp_type == MONITOR_RESP_SPAWN_SESSION);
    status = *(uint32_t *)resp_payload;
    assert(status == MONITOR_STATUS_ERR_STALE);
    g_mock_db_check_valid = true;

    printf("PASS: test_spawn_session_credentials\n");
}

int main(void) {
    test_monitor_ipc_framing();
    test_monitor_signing_oracle();
    test_password_auth();
    test_authorized_keys_parser();
    test_pubkey_auth_and_permissions();
    test_pubkey_signature_and_session_binding();
    test_auth_protocol_engine();
    test_spawn_session_credentials();

    printf("\nALL TESTS PASSED (tests/ssh_auth_host.c)\n");
    return 0;
}
