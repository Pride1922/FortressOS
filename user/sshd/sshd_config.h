#ifndef USER_SSHD_CONFIG_H
#define USER_SSHD_CONFIG_H

#include "types.h"

/* Canonical configuration paths and precedence:
 * 1. Explicit CLI argument (-f <path>): if provided, loaded directly.
 * 2. Installed root: SSHD_INSTALLED_CONFIG_PATH (/etc/ssh/sshd_config) [authoritative].
 * 3. Live media:     SSHD_LIVE_CONFIG_PATH (/mnt/.fortress/sshd.conf) [fallback, non-persistent].
 * 4. No file found:  SSH disabled, refuses to start with clear diagnostic.
 */
#define SSHD_INSTALLED_CONFIG_PATH "/etc/ssh/sshd_config"
#define SSHD_LIVE_CONFIG_PATH      "/mnt/.fortress/sshd.conf"
#define SSHD_DEFAULT_CONFIG_PATH   SSHD_LIVE_CONFIG_PATH /* backwards-compat alias */
#define SSHD_MAX_CONFIG_FILE_SIZE  4096
#define SSHD_MAX_LINE_LEN          256

typedef struct {
    bool     enabled;           /* Default true if config file found and not disabled */
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

/* Load config from file at path (or canonical search paths if path is NULL). Returns 0 on success. */
int sshd_config_load(const char *path, sshd_config_t *out_cfg,
                     char *err_msg, size_t err_cap);

/* Load config with custom search paths (for precedence testing and explicit overrides) */
int sshd_config_load_from_paths(const char *installed_path, const char *live_path,
                                sshd_config_t *out_cfg, char *err_msg, size_t err_cap);

#endif /* USER_SSHD_CONFIG_H */
