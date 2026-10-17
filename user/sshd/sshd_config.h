#ifndef USER_SSHD_CONFIG_H
#define USER_SSHD_CONFIG_H

#include "types.h"

#define SSHD_DEFAULT_CONFIG_PATH   "/mnt/.fortress/sshd.conf"
#define SSHD_INSTALLED_CONFIG_PATH "/etc/ssh/sshd_config"
#define SSHD_MAX_CONFIG_FILE_SIZE  4096
#define SSHD_MAX_LINE_LEN          256

typedef struct {
    bool     enabled;           /* Default true */
    uint16_t port;              /* Default 22 */
    uint32_t listen_address;    /* Network byte order, default 0 (INADDR_ANY) */
    bool     permit_root_login; /* Default false */
    bool     password_auth;     /* Default true */
    bool     pubkey_auth;       /* Default true */
    uint32_t max_sessions;      /* Default 2, max 2 */
} sshd_config_t;

/* Initialize config with secure FortressOS defaults */
void sshd_config_init_defaults(sshd_config_t *cfg);

/* Parse config from memory buffer. Returns 0 on success, -1 on fatal error. */
int sshd_config_parse_buffer(const char *buf, size_t len, sshd_config_t *out_cfg,
                             char *err_msg, size_t err_cap);

/* Load config from file at path (or default paths if path is NULL). Returns 0 on success. */
int sshd_config_load(const char *path, sshd_config_t *out_cfg,
                     char *err_msg, size_t err_cap);

#endif /* USER_SSHD_CONFIG_H */
