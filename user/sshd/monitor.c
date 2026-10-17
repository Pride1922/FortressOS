#include "monitor.h"
#include "crypto.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#else
#include "syscall_abi.h"
#include "vfs.h"
static long mon_syscall(long n, uintptr_t a, uintptr_t b, uintptr_t c) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
static long mon_syscall4(long n, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    long ret;
    register long r10 __asm__("r10") = (long)d;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
#endif

#ifndef CRYPTO_HOST_TEST
bool creds_valid(const creds_t *v) {
    if (!v || v->ngroups > CREDS_MAX_GROUPS || v->reserved ||
        (v->umask & ~0777U) || (v->cap_effective & ~CAP_ALL)) return false;
    for (unsigned i = v->ngroups; i < CREDS_MAX_GROUPS; ++i)
        if (v->groups[i]) return false;
    return true;
}
#endif

/* Constant-time dummy hash for unknown, locked, or root accounts (100000 rounds sha256-crypt matching userdb) */
static const char g_dummy_shadow_hash[] =
    "$5$rounds=100000$1234567890abcdef$0123456789012345678901234567890123456789012";

static void safe_strcpy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* IPC Pipe I/O Helpers */
static int read_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t rem = len;
    while (rem > 0) {
#ifdef CRYPTO_HOST_TEST
        ssize_t n = read(fd, p, rem);
#else
        long n = mon_syscall(SYS_READ, fd, (uintptr_t)p, rem);
#endif
        if (n <= 0) return -1;
        p += n;
        rem -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t rem = len;
    while (rem > 0) {
#ifdef CRYPTO_HOST_TEST
        ssize_t n = write(fd, p, rem);
#else
        long n = mon_syscall(SYS_WRITE, fd, (uintptr_t)p, rem);
#endif
        if (n <= 0) return -1;
        p += n;
        rem -= (size_t)n;
    }
    return 0;
}

int monitor_send_msg(int fd, uint8_t msg_type, uint8_t session_id,
                     const void *payload, uint16_t payload_len) {
    if (payload_len > MONITOR_MAX_PAYLOAD) return -1;
    if (payload_len > 0 && !payload) return -1;

    monitor_hdr_t hdr;
    hdr.magic = MONITOR_MAGIC;
    hdr.msg_type = msg_type;
    hdr.session_id = session_id;
    hdr.payload_len = payload_len;
    hdr.reserved = 0;

    if (write_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload_len > 0) {
        if (write_all(fd, payload, payload_len) < 0) return -1;
    }
    return 0;
}

int monitor_recv_msg(int fd, monitor_hdr_t *out_hdr,
                     void *payload_buf, size_t payload_cap) {
    if (!out_hdr) return -1;

    monitor_hdr_t hdr;
    if (read_all(fd, &hdr, sizeof(hdr)) < 0) return -1;

    if (hdr.magic != MONITOR_MAGIC) return -1;
    if (hdr.payload_len > MONITOR_MAX_PAYLOAD || hdr.payload_len > payload_cap) return -1;

    if (hdr.payload_len > 0) {
        if (!payload_buf) return -1;
        if (read_all(fd, payload_buf, hdr.payload_len) < 0) return -1;
    }

    *out_hdr = hdr;
    return 0;
}

void monitor_init(monitor_ctx_t *ctx, const uint8_t host_secret[64]) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    if (host_secret) {
        memcpy(ctx->host_secret, host_secret, 64);
        crypto_ed25519_from_seed(host_secret, ctx->host_public, ctx->host_secret);
        ctx->host_key_loaded = true;
    }
    for (unsigned i = 0; i < 2; i++) {
        ctx->conns[i].session_id = (uint8_t)i;
        ctx->conns[i].active = false;
        ctx->conns[i].kex_signed = false;
        ctx->conns[i].auth_attempts = 0;
        ctx->conns[i].authenticated = false;
    }
}

int monitor_set_userdb(monitor_ctx_t *ctx, const userdb_t *db) {
    if (!ctx || !db) return -1;
    ctx->db = *db;
    ctx->db_loaded = true;
    return 0;
}

/* Base64 Decoding Helper */
static int b64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t at = 0;
    if (!in || in_len % 4 != 0) return -1;

    for (size_t i = 0; i < in_len; i += 4) {
        int a = b64_digit((unsigned char)in[i]);
        int b = b64_digit((unsigned char)in[i + 1]);
        int c = b64_digit((unsigned char)in[i + 2]);
        int d = b64_digit((unsigned char)in[i + 3]);
        bool pad2 = (in[i + 2] == '=');
        bool pad1 = (in[i + 3] == '=');

        if (a < 0 || b < 0) return -1;
        if (!pad2 && c < 0) return -1;
        if (!pad1 && d < 0) return -1;
        if (pad2 && !pad1) return -1;
        if ((pad1 || pad2) && (i + 4 != in_len)) return -1;
        if (pad2 && (b & 15)) return -1;
        if (pad1 && !pad2 && (c & 3)) return -1;

        if (at >= out_cap) return -1;
        uint32_t val = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                       ((uint32_t)(pad2 ? 0 : c) << 6) |
                       ((uint32_t)(pad1 ? 0 : d));

        out[at++] = (uint8_t)(val >> 16);
        if (!pad2) {
            if (at >= out_cap) return -1;
            out[at++] = (uint8_t)(val >> 8);
        }
        if (!pad1) {
            if (at >= out_cap) return -1;
            out[at++] = (uint8_t)val;
        }
    }
    *out_len = at;
    return 0;
}

/* Parse a single authorized_keys line: "ssh-ed25519 <base64-blob> [comment]" */
int monitor_parse_authorized_key_line(const char *line, size_t len,
                                      uint8_t out_pubkey[32]) {
    if (!line || !out_pubkey) return -1;
    size_t i = 0;

    /* Skip leading whitespace */
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] == '\0' || line[i] == '\r' || line[i] == '\n' || line[i] == '#') {
        return 1; /* Skip comment or blank line */
    }

    /* Algorithm identifier must be "ssh-ed25519" */
    static const char algo[] = "ssh-ed25519";
    size_t algo_len = sizeof(algo) - 1;
    if (len - i < algo_len || memcmp(line + i, algo, algo_len) != 0) {
        return -1;
    }
    i += algo_len;
    if (i >= len || (line[i] != ' ' && line[i] != '\t')) return -1;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return -1;

    /* Extract base64 token */
    size_t b64_start = i;
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
        i++;
    }
    size_t b64_len = i - b64_start;
    if (b64_len == 0 || b64_len > 512) return -1;

    /* Decode base64 blob */
    uint8_t blob[256];
    size_t blob_len = 0;
    if (b64_decode(line + b64_start, b64_len, blob, sizeof(blob), &blob_len) != 0) {
        return -1;
    }

    /* Blob must be 51 bytes: 4 len + 11 "ssh-ed25519" + 4 len + 32 pubkey */
    if (blob_len != 51) return -1;
    uint32_t len1 = ((uint32_t)blob[0] << 24) | ((uint32_t)blob[1] << 16) | ((uint32_t)blob[2] << 8) | blob[3];
    if (len1 != 11 || memcmp(blob + 4, "ssh-ed25519", 11) != 0) return -1;

    uint32_t len2 = ((uint32_t)blob[15] << 24) | ((uint32_t)blob[16] << 16) | ((uint32_t)blob[17] << 8) | blob[18];
    if (len2 != 32) return -1;

    memcpy(out_pubkey, blob + 19, 32);
    return 0;
}

/* Validates user's ~/.ssh (dir 0700, uid user/root) and ~/.ssh/authorized_keys (file 0600, uid user/root) */
int monitor_check_authorized_keys(const char *home_dir, uint32_t expected_uid,
                                  const uint8_t pubkey[32]) {
    if (!home_dir || !pubkey || home_dir[0] != '/') return -1;
    if (strstr(home_dir, "..")) return -1;

    char dir_path[256];
    char file_path[256];
    size_t hlen = strlen(home_dir);
    if (hlen + 6 >= sizeof(dir_path) || hlen + 22 >= sizeof(file_path)) return -1;

    memcpy(dir_path, home_dir, hlen);
    memcpy(dir_path + hlen, "/.ssh", 6);

    memcpy(file_path, home_dir, hlen);
    memcpy(file_path + hlen, "/.ssh/authorized_keys", 22);

#ifdef CRYPTO_HOST_TEST
    struct stat st_dir;
    if (stat(dir_path, &st_dir) != 0) return -1;
    if (!S_ISDIR(st_dir.st_mode)) return -1;
    if (st_dir.st_uid != expected_uid && st_dir.st_uid != 0) return -1;
    if ((st_dir.st_mode & 0077) != 0) return -1; /* Strict dir 0700: no group/other permissions */

    struct stat st_file;
    if (stat(file_path, &st_file) != 0) return -1;
    if (!S_ISREG(st_file.st_mode)) return -1;
    if (st_file.st_uid != expected_uid && st_file.st_uid != 0) return -1;
    if ((st_file.st_mode & 0077) != 0 || (st_file.st_mode & 0111) != 0) return -1; /* Strict file 0600 */
    if (st_file.st_size > 8192) return -1;

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) return -1;
    char content[8193];
    ssize_t n = read(fd, content, sizeof(content) - 1);
    close(fd);
    if (n < 0) return -1;
    content[n] = '\0';
    size_t file_len = (size_t)n;
#else
    stat_ext_v1_t st_dir;
    if (mon_syscall4(SYS_STAT_EXT, (uintptr_t)dir_path, (uintptr_t)&st_dir, sizeof(st_dir), 1) < 0) return -1;
    if (st_dir.type != VFS_DIRECTORY) return -1;
    if (st_dir.uid != expected_uid && st_dir.uid != 0) return -1;
    if ((st_dir.mode & 0077) != 0) return -1;

    stat_ext_v1_t st_file;
    if (mon_syscall4(SYS_STAT_EXT, (uintptr_t)file_path, (uintptr_t)&st_file, sizeof(st_file), 1) < 0) return -1;
    if (st_file.type != VFS_FILE) return -1;
    if (st_file.uid != expected_uid && st_file.uid != 0) return -1;
    if ((st_file.mode & 0077) != 0 || (st_file.mode & 0111) != 0) return -1;
    if (st_file.file_size > 8192) return -1;

    long fd = mon_syscall(SYS_OPEN, (uintptr_t)file_path, VFS_O_RDONLY, 0);
    if (fd < 0) return -1;
    char content[8193];
    long n = mon_syscall(SYS_READ, fd, (uintptr_t)content, sizeof(content) - 1);
    mon_syscall(SYS_CLOSE, fd, 0, 0);
    if (n < 0) return -1;
    content[n] = '\0';
    size_t file_len = (size_t)n;
#endif

    /* Iterate lines and search for matching key */
    size_t offset = 0;
    while (offset < file_len) {
        size_t line_start = offset;
        while (offset < file_len && content[offset] != '\n') offset++;
        size_t line_len = offset - line_start;
        if (offset < file_len && content[offset] == '\n') offset++;

        uint8_t parsed_key[32];
        int r = monitor_parse_authorized_key_line(content + line_start, line_len, parsed_key);
        if (r == 0) {
            if (crypto_verify32(parsed_key, pubkey) == 0) {
                return 0; /* Authorized key found! */
            }
        }
    }
    return -1; /* Key not found */
}

/* Dispatch incoming request and formulate response */
int monitor_handle_request(monitor_ctx_t *ctx, const monitor_hdr_t *hdr,
                           const uint8_t *req_payload,
                           uint8_t *resp_type, uint8_t *resp_payload,
                           uint16_t *resp_payload_len, size_t resp_cap) {
    if (!ctx || !hdr || !resp_type || !resp_payload || !resp_payload_len) return -1;

    if (hdr->magic != MONITOR_MAGIC || hdr->session_id >= 2 || hdr->payload_len > MONITOR_MAX_PAYLOAD) {
        *resp_type = MONITOR_RESP_ERROR;
        uint32_t status = MONITOR_STATUS_ERR_ARG;
        memcpy(resp_payload, &status, sizeof(status));
        *resp_payload_len = sizeof(status);
        return 0;
    }

    monitor_conn_t *conn = &ctx->conns[hdr->session_id];
    conn->session_id = hdr->session_id;
    conn->active = true;

    /* 1. REQ_SIGN_HASH: Host key signing oracle */
    if (hdr->msg_type == MONITOR_REQ_SIGN_HASH) {
        if (hdr->payload_len != 32 || !req_payload) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Strict limit: exactly 1 signature per KEX round per connection */
        if (conn->kex_signed) {
            *resp_type = MONITOR_RESP_SIGN_HASH;
            uint32_t status = MONITOR_STATUS_ERR_ORACLE;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        if (!ctx->host_key_loaded) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_STATE;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        if (resp_cap < 4 + 64) return -1;

        uint8_t sig[64];
        if (crypto_ed25519_sign(sig, ctx->host_secret, req_payload, 32) != 0) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_STATE;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        conn->kex_signed = true;
        *resp_type = MONITOR_RESP_SIGN_HASH;
        uint32_t status = MONITOR_STATUS_OK;
        memcpy(resp_payload, &status, sizeof(status));
        memcpy(resp_payload + sizeof(status), sig, 64);
        *resp_payload_len = sizeof(status) + 64;
        return 0;
    }

    /* 2. REQ_AUTH_PASSWORD */
    if (hdr->msg_type == MONITOR_REQ_AUTH_PASSWORD) {
        if (hdr->payload_len < 8 || !req_payload) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        uint32_t user_len = ((uint32_t)req_payload[0]) | ((uint32_t)req_payload[1] << 8) |
                            ((uint32_t)req_payload[2] << 16) | ((uint32_t)req_payload[3] << 24);
        if (user_len == 0 || user_len >= 32 || 4 + user_len + 4 > hdr->payload_len) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        char username[32];
        memcpy(username, req_payload + 4, user_len);
        username[user_len] = '\0';

        const uint8_t *p_pass = req_payload + 4 + user_len;
        uint32_t pass_len = ((uint32_t)p_pass[0]) | ((uint32_t)p_pass[1] << 8) |
                            ((uint32_t)p_pass[2] << 16) | ((uint32_t)p_pass[3] << 24);
        if (pass_len >= 128 || 4 + user_len + 4 + pass_len != hdr->payload_len) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        char password[128];
        memcpy(password, p_pass + 4, pass_len);
        password[pass_len] = '\0';

        *resp_type = MONITOR_RESP_AUTH_PASSWORD;

        /* Enforce attempt cap: max 5 attempts */
        if (conn->auth_attempts >= 5) {
            crypto_wipe(password, sizeof(password));
            uint32_t status = MONITOR_STATUS_AUTH_DENIED;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Root remote login rejection with dummy hash verification */
        if (strcmp(username, "root") == 0) {
            db_verify(password, g_dummy_shadow_hash);
            crypto_wipe(password, sizeof(password));
            conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Empty password rejection with dummy verification */
        if (pass_len == 0 || password[0] == '\0') {
            db_verify("dummy", g_dummy_shadow_hash);
            crypto_wipe(password, sizeof(password));
            conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* User database lookup */
        if (!ctx->db_loaded) {
            db_load(&ctx->db, true);
            ctx->db_loaded = true;
        }

        const db_user_t *u = db_user_name(&ctx->db, username);
        const db_shadow_t *s = db_shadow_name(&ctx->db, username);

        /* Unknown, locked, or restricted accounts execute constant-time dummy verification */
        if (!u || !s || s->restricted || !s->hash[0] || s->hash[0] == '!' || s->hash[0] == '*') {
            db_verify(password, g_dummy_shadow_hash);
            crypto_wipe(password, sizeof(password));
            conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Active user authentication */
        bool ok = db_verify(password, s->hash);
        crypto_wipe(password, sizeof(password));

        if (!ok) {
            conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Authentication successful: load session credentials */
        conn->authenticated = true;
        safe_strcpy(conn->username, u->name, sizeof(conn->username));
        conn->uid = u->uid;
        conn->gid = u->gid;
        safe_strcpy(conn->home, u->home, sizeof(conn->home));
        safe_strcpy(conn->shell, u->shell, sizeof(conn->shell));

        unsigned ng = 0;
        db_groups(&ctx->db, u, conn->groups, &ng);
        conn->ngroups = (uint16_t)ng;

        if (resp_cap < 334) return -1;

        uint32_t status = MONITOR_STATUS_OK;
        size_t off = 0;
        memcpy(resp_payload + off, &status, sizeof(status)); off += sizeof(status);
        memcpy(resp_payload + off, &conn->uid, sizeof(conn->uid)); off += sizeof(conn->uid);
        memcpy(resp_payload + off, &conn->gid, sizeof(conn->gid)); off += sizeof(conn->gid);
        memcpy(resp_payload + off, &conn->ngroups, sizeof(conn->ngroups)); off += sizeof(conn->ngroups);
        memcpy(resp_payload + off, conn->groups, sizeof(conn->groups)); off += sizeof(conn->groups);
        memcpy(resp_payload + off, conn->home, 128); off += 128;
        memcpy(resp_payload + off, conn->shell, 128); off += 128;

        *resp_payload_len = (uint16_t)off;
        return 0;
    }

    /* 3. REQ_AUTH_PUBKEY */
    if (hdr->msg_type == MONITOR_REQ_AUTH_PUBKEY) {
        if (hdr->payload_len < 4 + 1 + 32 + 64 + 2 || !req_payload) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        size_t off = 0;
        uint32_t user_len = ((uint32_t)req_payload[off]) | ((uint32_t)req_payload[off + 1] << 8) |
                            ((uint32_t)req_payload[off + 2] << 16) | ((uint32_t)req_payload[off + 3] << 24);
        off += 4;
        if (user_len == 0 || user_len >= 32 || off + user_len > hdr->payload_len) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        char username[32];
        memcpy(username, req_payload + off, user_len);
        username[user_len] = '\0';
        off += user_len;

        uint8_t check_only = req_payload[off++];
        const uint8_t *pubkey = req_payload + off; off += 32;
        const uint8_t *sig = req_payload + off; off += 64;

        uint16_t signed_data_len = ((uint16_t)req_payload[off]) | ((uint16_t)req_payload[off + 1] << 8);
        off += 2;
        if (off + signed_data_len != hdr->payload_len) {
            *resp_type = MONITOR_RESP_ERROR;
            uint32_t status = MONITOR_STATUS_ERR_ARG;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }
        const uint8_t *signed_data = req_payload + off;

        *resp_type = MONITOR_RESP_AUTH_PUBKEY;

        if (conn->auth_attempts >= 5) {
            uint32_t status = MONITOR_STATUS_AUTH_DENIED;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        if (strcmp(username, "root") == 0) {
            if (!check_only) conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        if (!ctx->db_loaded) {
            db_load(&ctx->db, true);
            ctx->db_loaded = true;
        }

        const db_user_t *u = db_user_name(&ctx->db, username);
        if (!u) {
            if (!check_only) conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Check authorized_keys file */
        if (monitor_check_authorized_keys(u->home, u->uid, pubkey) != 0) {
            if (!check_only) conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Probe check only */
        if (check_only) {
            uint32_t status = MONITOR_STATUS_PK_OK;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Verify Ed25519 signature over RFC 4252 signed_data */
        if (crypto_ed25519_verify(sig, pubkey, signed_data, signed_data_len) != 0) {
            conn->auth_attempts++;
            uint32_t status = MONITOR_STATUS_AUTH_FAIL;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Signature valid: record session authentication */
        conn->authenticated = true;
        safe_strcpy(conn->username, u->name, sizeof(conn->username));
        conn->uid = u->uid;
        conn->gid = u->gid;
        safe_strcpy(conn->home, u->home, sizeof(conn->home));
        safe_strcpy(conn->shell, u->shell, sizeof(conn->shell));

        unsigned ng = 0;
        db_groups(&ctx->db, u, conn->groups, &ng);
        conn->ngroups = (uint16_t)ng;

        if (resp_cap < 334) return -1;

        uint32_t status = MONITOR_STATUS_OK;
        off = 0;
        memcpy(resp_payload + off, &status, sizeof(status)); off += sizeof(status);
        memcpy(resp_payload + off, &conn->uid, sizeof(conn->uid)); off += sizeof(conn->uid);
        memcpy(resp_payload + off, &conn->gid, sizeof(conn->gid)); off += sizeof(conn->gid);
        memcpy(resp_payload + off, &conn->ngroups, sizeof(conn->ngroups)); off += sizeof(conn->ngroups);
        memcpy(resp_payload + off, conn->groups, sizeof(conn->groups)); off += sizeof(conn->groups);
        memcpy(resp_payload + off, conn->home, 128); off += 128;
        memcpy(resp_payload + off, conn->shell, 128); off += 128;

        *resp_payload_len = (uint16_t)off;
        return 0;
    }

    /* 4. REQ_SPAWN_SESSION */
    if (hdr->msg_type == MONITOR_REQ_SPAWN_SESSION) {
        *resp_type = MONITOR_RESP_SPAWN_SESSION;
        if (!conn->authenticated) {
            uint32_t status = MONITOR_STATUS_ERR_PERM;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Revalidate account generation per CREDENTIAL_SNAPSHOT_PROTOCOL.md */
        if (!db_check()) {
            uint32_t status = MONITOR_STATUS_ERR_STALE;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        /* Credential snapshot construction */
        creds_t creds;
        memset(&creds, 0, sizeof(creds));
        creds.uid = conn->uid;
        creds.euid = conn->uid;
        creds.suid = conn->uid;
        creds.gid = conn->gid;
        creds.egid = conn->gid;
        creds.sgid = conn->gid;
        creds.ngroups = conn->ngroups;
        for (unsigned i = 0; i < conn->ngroups && i < CREDS_MAX_GROUPS; i++) {
            creds.groups[i] = conn->groups[i];
        }
        creds.cap_effective = 0; /* Drop all capabilities! */
        creds.umask = 0022;

        if (!creds_valid(&creds)) {
            uint32_t status = MONITOR_STATUS_ERR_STATE;
            memcpy(resp_payload, &status, sizeof(status));
            *resp_payload_len = sizeof(status);
            return 0;
        }

        uint32_t status = MONITOR_STATUS_OK;
        int64_t mock_pid = 42;
        memcpy(resp_payload, &status, sizeof(status));
        memcpy(resp_payload + sizeof(status), &mock_pid, sizeof(mock_pid));
        *resp_payload_len = sizeof(status) + sizeof(mock_pid);
        return 0;
    }

    *resp_type = MONITOR_RESP_ERROR;
    uint32_t status = MONITOR_STATUS_ERR_ARG;
    memcpy(resp_payload, &status, sizeof(status));
    *resp_payload_len = sizeof(status);
    return 0;
}
