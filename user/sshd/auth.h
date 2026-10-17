#ifndef FORTRESS_SSH_AUTH_H
#define FORTRESS_SSH_AUTH_H

#include "types.h"
#include "sshbuf.h"

#define SSH_AUTH_MAX_ATTEMPTS 5

typedef enum {
    SSH_AUTH_STATE_NONE = 0,
    SSH_AUTH_STATE_SERVICE_REQUESTED,
    SSH_AUTH_STATE_IN_PROGRESS,
    SSH_AUTH_STATE_SUCCESS,
    SSH_AUTH_STATE_FAILED
} ssh_auth_state_t;

typedef struct {
    ssh_auth_state_t state;
    uint8_t session_id[32];
    bool session_id_set;
    unsigned attempts;
    bool authenticated;
    char username[32];
    uint32_t uid;
    uint32_t gid;
    uint16_t ngroups;
    uint32_t groups[16];
    char home[128];
    char shell[128];
    int mon_in_fd;  /* Pipe from monitor (worker reads) */
    int mon_out_fd; /* Pipe to monitor (worker writes) */
    uint8_t session_idx;
} ssh_auth_ctx_t;

void auth_init(ssh_auth_ctx_t *ctx, const uint8_t session_id[32], uint8_t session_idx,
               int mon_in_fd, int mon_out_fd);
void auth_cleanup(ssh_auth_ctx_t *ctx);

/* Process incoming SSH_MSG_SERVICE_REQUEST ("ssh-userauth") */
int auth_process_service_request(ssh_auth_ctx_t *ctx, const uint8_t *payload, size_t len,
                                 sshbuf_t *out_reply);

/* Process incoming SSH_MSG_USERAUTH_REQUEST */
int auth_process_request(ssh_auth_ctx_t *ctx, const uint8_t *payload, size_t len,
                         sshbuf_t *out_reply);

/* Helper to build SSH_MSG_USERAUTH_BANNER */
int auth_build_banner(const char *message, sshbuf_t *out_msg);

/* Helper to construct the data to be signed for RFC 4252 publickey authentication */
int auth_build_signed_data(const uint8_t session_id[32],
                           const char *username,
                           const char *service_name,
                           const char *algo_name,
                           const uint8_t *pk_blob, size_t pk_blob_len,
                           uint8_t *out_buf, size_t out_cap, size_t *out_len);

#endif /* FORTRESS_SSH_AUTH_H */
