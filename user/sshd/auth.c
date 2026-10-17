#include "auth.h"
#include "monitor.h"
#include "ssh_packet.h"
#include "crypto.h"
#include "string.h"

static void safe_strcpy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void auth_init(ssh_auth_ctx_t *ctx, const uint8_t session_id[32], uint8_t session_idx,
               int mon_in_fd, int mon_out_fd) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = SSH_AUTH_STATE_NONE;
    if (session_id) {
        memcpy(ctx->session_id, session_id, 32);
        ctx->session_id_set = true;
    }
    ctx->session_idx = session_idx;
    ctx->mon_in_fd = mon_in_fd;
    ctx->mon_out_fd = mon_out_fd;
    ctx->attempts = 0;
    ctx->authenticated = false;
}

void auth_cleanup(ssh_auth_ctx_t *ctx) {
    if (!ctx) return;
    crypto_wipe(ctx->session_id, sizeof(ctx->session_id));
    memset(ctx, 0, sizeof(*ctx));
}

int auth_build_signed_data(const uint8_t session_id[32],
                           const char *username,
                           const char *service_name,
                           const char *algo_name,
                           const uint8_t *pk_blob, size_t pk_blob_len,
                           uint8_t *out_buf, size_t out_cap, size_t *out_len) {
    if (!session_id || !username || !service_name || !algo_name || !pk_blob || !out_buf || !out_len) {
        return SSH_ERR_INVALID_ARG;
    }

    sshbuf_t b;
    if (sshbuf_init_fixed(&b, out_buf, out_cap) != 0) return SSH_ERR_BUFFER_TOO_SMALL;

    /* 1. string session_id (RFC 4252 §7) */
    if (sshbuf_put_string(&b, session_id, 32) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 2. byte SSH_MSG_USERAUTH_REQUEST */
    if (sshbuf_put_u8(&b, SSH_MSG_USERAUTH_REQUEST) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 3. string user name */
    if (sshbuf_put_cstring(&b, username) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 4. string service name ("ssh-connection") */
    if (sshbuf_put_cstring(&b, service_name) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 5. string method name ("publickey") */
    if (sshbuf_put_cstring(&b, "publickey") != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 6. boolean TRUE (1) */
    if (sshbuf_put_u8(&b, 1) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 7. string public key algorithm name ("ssh-ed25519") */
    if (sshbuf_put_cstring(&b, algo_name) != 0) return SSH_ERR_BUFFER_TOO_SMALL;
    /* 8. string public key blob */
    if (sshbuf_put_string(&b, pk_blob, pk_blob_len) != 0) return SSH_ERR_BUFFER_TOO_SMALL;

    *out_len = sshbuf_len(&b);
    return 0;
}

int auth_process_service_request(ssh_auth_ctx_t *ctx, const uint8_t *payload, size_t len,
                                 sshbuf_t *out_reply) {
    if (!ctx || !payload || len == 0 || !out_reply) return SSH_ERR_INVALID_ARG;

    sshbuf_t in;
    if (sshbuf_init_from(&in, payload, len) != 0) return SSH_ERR_INVALID_FORMAT;

    uint8_t msg_type = 0;
    if (sshbuf_get_u8(&in, &msg_type) != 0 || msg_type != SSH_MSG_SERVICE_REQUEST) {
        return SSH_ERR_INVALID_FORMAT;
    }

    const uint8_t *svc_ptr = NULL;
    size_t svc_len = 0;
    if (sshbuf_get_string_ptr(&in, &svc_ptr, &svc_len) != 0) return SSH_ERR_INVALID_FORMAT;

    if (svc_len != 12 || memcmp(svc_ptr, "ssh-userauth", 12) != 0) {
        return SSH_ERR_INVALID_ARG;
    }

    ctx->state = SSH_AUTH_STATE_SERVICE_REQUESTED;

    /* Build SSH_MSG_SERVICE_ACCEPT ("ssh-userauth") */
    if (sshbuf_put_u8(out_reply, SSH_MSG_SERVICE_ACCEPT) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out_reply, "ssh-userauth") != 0) return SSH_ERR_NO_MEM;

    return 0;
}

static int build_auth_failure(sshbuf_t *out_reply) {
    if (sshbuf_put_u8(out_reply, SSH_MSG_USERAUTH_FAILURE) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out_reply, "publickey,password") != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u8(out_reply, 0) != 0) return SSH_ERR_NO_MEM; /* partial success = FALSE */
    return 0;
}

static int build_auth_success(sshbuf_t *out_reply) {
    if (sshbuf_put_u8(out_reply, SSH_MSG_USERAUTH_SUCCESS) != 0) return SSH_ERR_NO_MEM;
    return 0;
}

int auth_build_banner(const char *message, sshbuf_t *out_msg) {
    if (!message || !out_msg) return SSH_ERR_INVALID_ARG;
    if (sshbuf_put_u8(out_msg, SSH_MSG_USERAUTH_BANNER) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out_msg, message) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out_msg, "") != 0) return SSH_ERR_NO_MEM; /* language tag */
    return 0;
}

static uint8_t s_auth_req_buf[1024];
static uint8_t s_auth_resp_buf[512];
static uint8_t s_auth_signed_data[512];

int auth_process_request(ssh_auth_ctx_t *ctx, const uint8_t *payload, size_t len,
                         sshbuf_t *out_reply) {
    if (!ctx || !payload || len == 0 || !out_reply) return SSH_ERR_INVALID_ARG;

    if (ctx->state != SSH_AUTH_STATE_SERVICE_REQUESTED && ctx->state != SSH_AUTH_STATE_IN_PROGRESS) {
        return SSH_ERR_INVALID_FORMAT;
    }

    ctx->state = SSH_AUTH_STATE_IN_PROGRESS;

    /* Check max attempts */
    if (ctx->attempts >= SSH_AUTH_MAX_ATTEMPTS) {
        ctx->state = SSH_AUTH_STATE_FAILED;
        return build_auth_failure(out_reply);
    }

    sshbuf_t in;
    if (sshbuf_init_from(&in, payload, len) != 0) return SSH_ERR_INVALID_FORMAT;

    uint8_t msg_type = 0;
    if (sshbuf_get_u8(&in, &msg_type) != 0 || msg_type != SSH_MSG_USERAUTH_REQUEST) {
        return SSH_ERR_INVALID_FORMAT;
    }

    const uint8_t *user_ptr = NULL;
    size_t user_len = 0;
    if (sshbuf_get_string_ptr(&in, &user_ptr, &user_len) != 0 || user_len == 0 || user_len >= 32) {
        return SSH_ERR_INVALID_FORMAT;
    }
    char username[32];
    memcpy(username, user_ptr, user_len);
    username[user_len] = '\0';

    const uint8_t *svc_ptr = NULL;
    size_t svc_len = 0;
    if (sshbuf_get_string_ptr(&in, &svc_ptr, &svc_len) != 0) return SSH_ERR_INVALID_FORMAT;
    if (svc_len != 14 || memcmp(svc_ptr, "ssh-connection", 14) != 0) {
        return build_auth_failure(out_reply);
    }

    const uint8_t *method_ptr = NULL;
    size_t method_len = 0;
    if (sshbuf_get_string_ptr(&in, &method_ptr, &method_len) != 0) return SSH_ERR_INVALID_FORMAT;

    /* Method: "none" (auth methods probe) */
    if (method_len == 4 && memcmp(method_ptr, "none", 4) == 0) {
        return build_auth_failure(out_reply);
    }

    /* Method: "password" */
    if (method_len == 8 && memcmp(method_ptr, "password", 8) == 0) {
        uint8_t change_pass = 0;
        if (sshbuf_get_u8(&in, &change_pass) != 0 || change_pass != 0) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        const uint8_t *pass_ptr = NULL;
        size_t pass_len = 0;
        if (sshbuf_get_string_ptr(&in, &pass_ptr, &pass_len) != 0 || pass_len >= 128) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        /* Forward to monitor via IPC */
        size_t off = 0;
        uint32_t u_len = (uint32_t)user_len;
        memcpy(s_auth_req_buf + off, &u_len, 4); off += 4;
        memcpy(s_auth_req_buf + off, username, u_len); off += u_len;
        uint32_t p_len = (uint32_t)pass_len;
        memcpy(s_auth_req_buf + off, &p_len, 4); off += 4;
        memcpy(s_auth_req_buf + off, pass_ptr, p_len); off += p_len;

        if (monitor_send_msg(ctx->mon_out_fd, MONITOR_REQ_AUTH_PASSWORD,
                             ctx->session_idx, s_auth_req_buf, (uint16_t)off) < 0) {
            crypto_wipe(s_auth_req_buf, sizeof(s_auth_req_buf));
            return SSH_ERR_INVALID_FORMAT;
        }
        crypto_wipe(s_auth_req_buf, sizeof(s_auth_req_buf));

        monitor_hdr_t resp_hdr;
        if (monitor_recv_msg(ctx->mon_in_fd, &resp_hdr, s_auth_resp_buf, sizeof(s_auth_resp_buf)) < 0) {
            return SSH_ERR_INVALID_FORMAT;
        }

        if (resp_hdr.msg_type != MONITOR_RESP_AUTH_PASSWORD || resp_hdr.payload_len < 4) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        uint32_t status = ((uint32_t)s_auth_resp_buf[0]) | ((uint32_t)s_auth_resp_buf[1] << 8) |
                          ((uint32_t)s_auth_resp_buf[2] << 16) | ((uint32_t)s_auth_resp_buf[3] << 24);

        if (status == MONITOR_STATUS_OK) {
            ctx->state = SSH_AUTH_STATE_SUCCESS;
            ctx->authenticated = true;
            safe_strcpy(ctx->username, username, sizeof(ctx->username));
            if (resp_hdr.payload_len >= 4 + 4 + 4 + 2 + 64 + 128 + 128) {
                size_t roff = 4;
                memcpy(&ctx->uid, s_auth_resp_buf + roff, 4); roff += 4;
                memcpy(&ctx->gid, s_auth_resp_buf + roff, 4); roff += 4;
                memcpy(&ctx->ngroups, s_auth_resp_buf + roff, 2); roff += 2;
                memcpy(ctx->groups, s_auth_resp_buf + roff, 64); roff += 64;
                memcpy(ctx->home, s_auth_resp_buf + roff, 128); roff += 128;
                memcpy(ctx->shell, s_auth_resp_buf + roff, 128); roff += 128;
            }
            return build_auth_success(out_reply);
        } else {
            ctx->attempts++;
            if (ctx->attempts >= SSH_AUTH_MAX_ATTEMPTS) ctx->state = SSH_AUTH_STATE_FAILED;
            return build_auth_failure(out_reply);
        }
    }

    /* Method: "publickey" */
    if (method_len == 9 && memcmp(method_ptr, "publickey", 9) == 0) {
        uint8_t has_sig = 0;
        if (sshbuf_get_u8(&in, &has_sig) != 0) return SSH_ERR_INVALID_FORMAT;

        const uint8_t *algo_ptr = NULL;
        size_t algo_len = 0;
        if (sshbuf_get_string_ptr(&in, &algo_ptr, &algo_len) != 0) return SSH_ERR_INVALID_FORMAT;
        if (algo_len != 11 || memcmp(algo_ptr, "ssh-ed25519", 11) != 0) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        const uint8_t *pk_blob = NULL;
        size_t pk_blob_len = 0;
        if (sshbuf_get_string_ptr(&in, &pk_blob, &pk_blob_len) != 0 || pk_blob_len != 51) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        /* Validate pubkey blob prefix */
        uint32_t len1 = ((uint32_t)pk_blob[0] << 24) | ((uint32_t)pk_blob[1] << 16) |
                        ((uint32_t)pk_blob[2] << 8) | pk_blob[3];
        if (len1 != 11 || memcmp(pk_blob + 4, "ssh-ed25519", 11) != 0) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }
        uint32_t len2 = ((uint32_t)pk_blob[15] << 24) | ((uint32_t)pk_blob[16] << 16) |
                        ((uint32_t)pk_blob[17] << 8) | pk_blob[18];
        if (len2 != 32) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }
        const uint8_t *pubkey = pk_blob + 19;

        /* Probe check: client asks if key is acceptable */
        if (!has_sig) {
            size_t off = 0;
            uint32_t u_len = (uint32_t)user_len;
            memcpy(s_auth_req_buf + off, &u_len, 4); off += 4;
            memcpy(s_auth_req_buf + off, username, u_len); off += u_len;
            s_auth_req_buf[off++] = 1; /* check_only = 1 */
            memcpy(s_auth_req_buf + off, pubkey, 32); off += 32;
            memset(s_auth_req_buf + off, 0, 64); off += 64; /* empty signature */
            uint16_t zero_data_len = 0;
            memcpy(s_auth_req_buf + off, &zero_data_len, 2); off += 2;

            if (monitor_send_msg(ctx->mon_out_fd, MONITOR_REQ_AUTH_PUBKEY,
                                 ctx->session_idx, s_auth_req_buf, (uint16_t)off) < 0) {
                return SSH_ERR_INVALID_FORMAT;
            }

            monitor_hdr_t resp_hdr;
            if (monitor_recv_msg(ctx->mon_in_fd, &resp_hdr, s_auth_resp_buf, sizeof(s_auth_resp_buf)) < 0) {
                return SSH_ERR_INVALID_FORMAT;
            }

            uint32_t status = ((uint32_t)s_auth_resp_buf[0]) | ((uint32_t)s_auth_resp_buf[1] << 8) |
                              ((uint32_t)s_auth_resp_buf[2] << 16) | ((uint32_t)s_auth_resp_buf[3] << 24);

            if (status == MONITOR_STATUS_PK_OK) {
                /* Emit SSH_MSG_USERAUTH_PK_OK (60) */
                if (sshbuf_put_u8(out_reply, SSH_MSG_USERAUTH_PK_OK) != 0) return SSH_ERR_NO_MEM;
                if (sshbuf_put_cstring(out_reply, "ssh-ed25519") != 0) return SSH_ERR_NO_MEM;
                if (sshbuf_put_string(out_reply, pk_blob, pk_blob_len) != 0) return SSH_ERR_NO_MEM;
                return 0;
            } else {
                return build_auth_failure(out_reply);
            }
        }

        /* Full signature verification */
        const uint8_t *sig_blob = NULL;
        size_t sig_blob_len = 0;
        if (sshbuf_get_string_ptr(&in, &sig_blob, &sig_blob_len) != 0) return SSH_ERR_INVALID_FORMAT;

        /* Parse OpenSSH signature blob: string "ssh-ed25519" || string 64-byte signature */
        sshbuf_t sb;
        if (sshbuf_init_from(&sb, sig_blob, sig_blob_len) != 0) return SSH_ERR_INVALID_FORMAT;
        const uint8_t *sig_algo = NULL;
        size_t sig_algo_len = 0;
        if (sshbuf_get_string_ptr(&sb, &sig_algo, &sig_algo_len) != 0 ||
            sig_algo_len != 11 || memcmp(sig_algo, "ssh-ed25519", 11) != 0) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }
        const uint8_t *raw_sig = NULL;
        size_t raw_sig_len = 0;
        if (sshbuf_get_string_ptr(&sb, &raw_sig, &raw_sig_len) != 0 || raw_sig_len != 64) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        /* Construct RFC 4252 signed data bound to session_id */
        size_t signed_data_len = 0;
        if (auth_build_signed_data(ctx->session_id, username, "ssh-connection", "ssh-ed25519",
                                   pk_blob, pk_blob_len,
                                   s_auth_signed_data, sizeof(s_auth_signed_data), &signed_data_len) != 0) {
            ctx->attempts++;
            return build_auth_failure(out_reply);
        }

        /* Send to monitor for verification and credential retrieval */
        size_t off = 0;
        uint32_t u_len = (uint32_t)user_len;
        memcpy(s_auth_req_buf + off, &u_len, 4); off += 4;
        memcpy(s_auth_req_buf + off, username, u_len); off += u_len;
        s_auth_req_buf[off++] = 0; /* check_only = 0 */
        memcpy(s_auth_req_buf + off, pubkey, 32); off += 32;
        memcpy(s_auth_req_buf + off, raw_sig, 64); off += 64;
        uint16_t sd_len = (uint16_t)signed_data_len;
        memcpy(s_auth_req_buf + off, &sd_len, 2); off += 2;
        memcpy(s_auth_req_buf + off, s_auth_signed_data, signed_data_len); off += signed_data_len;

        if (monitor_send_msg(ctx->mon_out_fd, MONITOR_REQ_AUTH_PUBKEY,
                             ctx->session_idx, s_auth_req_buf, (uint16_t)off) < 0) {
            return SSH_ERR_INVALID_FORMAT;
        }

        monitor_hdr_t resp_hdr;
        if (monitor_recv_msg(ctx->mon_in_fd, &resp_hdr, s_auth_resp_buf, sizeof(s_auth_resp_buf)) < 0) {
            return SSH_ERR_INVALID_FORMAT;
        }

        uint32_t status = ((uint32_t)s_auth_resp_buf[0]) | ((uint32_t)s_auth_resp_buf[1] << 8) |
                          ((uint32_t)s_auth_resp_buf[2] << 16) | ((uint32_t)s_auth_resp_buf[3] << 24);

        if (status == MONITOR_STATUS_OK) {
            ctx->state = SSH_AUTH_STATE_SUCCESS;
            ctx->authenticated = true;
            safe_strcpy(ctx->username, username, sizeof(ctx->username));
            if (resp_hdr.payload_len >= 4 + 4 + 4 + 2 + 64 + 128 + 128) {
                size_t roff = 4;
                memcpy(&ctx->uid, s_auth_resp_buf + roff, 4); roff += 4;
                memcpy(&ctx->gid, s_auth_resp_buf + roff, 4); roff += 4;
                memcpy(&ctx->ngroups, s_auth_resp_buf + roff, 2); roff += 2;
                memcpy(ctx->groups, s_auth_resp_buf + roff, 64); roff += 64;
                memcpy(ctx->home, s_auth_resp_buf + roff, 128); roff += 128;
                memcpy(ctx->shell, s_auth_resp_buf + roff, 128); roff += 128;
            }
            return build_auth_success(out_reply);
        } else {
            ctx->attempts++;
            if (ctx->attempts >= SSH_AUTH_MAX_ATTEMPTS) ctx->state = SSH_AUTH_STATE_FAILED;
            return build_auth_failure(out_reply);
        }
    }

    /* Unsupported method */
    ctx->attempts++;
    return build_auth_failure(out_reply);
}
