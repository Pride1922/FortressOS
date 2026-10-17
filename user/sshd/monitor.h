#ifndef FORTRESS_SSH_MONITOR_H
#define FORTRESS_SSH_MONITOR_H

#include "types.h"
#include "creds.h"
#include "userdb.h"

#define MONITOR_MAGIC 0x5353484D /* "SSHM" */
#define MONITOR_MAX_PAYLOAD 2048

/* IPC Message Types connecting privileged monitor (/sbin/sshd) with unprivileged workers (sshd-net) */
typedef enum {
    MONITOR_REQ_SIGN_HASH       = 1,
    MONITOR_RESP_SIGN_HASH      = 2,
    MONITOR_REQ_AUTH_PASSWORD   = 3,
    MONITOR_RESP_AUTH_PASSWORD  = 4,
    MONITOR_REQ_AUTH_PUBKEY     = 5,
    MONITOR_RESP_AUTH_PUBKEY    = 6,
    MONITOR_REQ_SPAWN_SESSION   = 7,
    MONITOR_RESP_SPAWN_SESSION  = 8,
    MONITOR_RESP_ERROR          = 255
} monitor_msg_type_t;

/* Status codes in responses */
#define MONITOR_STATUS_OK           0
#define MONITOR_STATUS_AUTH_FAIL    1
#define MONITOR_STATUS_AUTH_DENIED  2
#define MONITOR_STATUS_PK_OK        3
#define MONITOR_STATUS_ERR_ARG      4
#define MONITOR_STATUS_ERR_STATE    5
#define MONITOR_STATUS_ERR_ORACLE   6
#define MONITOR_STATUS_ERR_PERM     7
#define MONITOR_STATUS_ERR_STALE    8

/* Header for all IPC messages over anonymous pipes (SYS_PIPE 24) */
typedef struct {
    uint32_t magic;         /* MONITOR_MAGIC */
    uint8_t  msg_type;      /* monitor_msg_type_t */
    uint8_t  session_id;    /* Session index: 0 or 1 */
    uint16_t payload_len;   /* Length of payload (<= MONITOR_MAX_PAYLOAD) */
    uint32_t reserved;      /* Zero */
} monitor_hdr_t;

_Static_assert(sizeof(monitor_hdr_t) == 12, "monitor_hdr_t must be exactly 12 bytes");

/* Per-connection state tracked by privileged monitor */
typedef struct {
    uint8_t session_id;
    bool active;
    bool kex_signed;        /* Enforces limit of 1 host signature per KEX round */
    unsigned auth_attempts; /* Max 5 attempts */
    bool authenticated;
    char username[32];
    uint32_t uid;
    uint32_t gid;
    uint16_t ngroups;
    uint32_t groups[16];
    char home[128];
    char shell[128];
} monitor_conn_t;

/* Privileged monitor context */
typedef struct {
    uint8_t host_secret[64];
    uint8_t host_public[32];
    bool host_key_loaded;
    monitor_conn_t conns[2];
    userdb_t db;
    bool db_loaded;
} monitor_ctx_t;

/* IPC framing helpers over pipes */
int monitor_send_msg(int fd, uint8_t msg_type, uint8_t session_id,
                     const void *payload, uint16_t payload_len);
int monitor_recv_msg(int fd, monitor_hdr_t *out_hdr,
                     void *payload_buf, size_t payload_cap);

/* Monitor context initialization & request handling */
void monitor_init(monitor_ctx_t *ctx, const uint8_t host_secret[64]);
int monitor_set_userdb(monitor_ctx_t *ctx, const userdb_t *db);
int monitor_handle_request(monitor_ctx_t *ctx, const monitor_hdr_t *hdr,
                           const uint8_t *req_payload,
                           uint8_t *resp_type, uint8_t *resp_payload,
                           uint16_t *resp_payload_len, size_t resp_cap);

/* Public key / authorized_keys verification */
int monitor_check_authorized_keys(const char *home_dir, uint32_t expected_uid,
                                  const uint8_t pubkey[32]);
int monitor_parse_authorized_key_line(const char *line, size_t len,
                                      uint8_t out_pubkey[32]);

#endif /* FORTRESS_SSH_MONITOR_H */
