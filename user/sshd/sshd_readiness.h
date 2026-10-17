#ifndef USER_SSHD_READINESS_H
#define USER_SSHD_READINESS_H

#include "types.h"
#include "sshd_config.h"
#include "userdb.h"

typedef struct {
    /* Function pointers for test mocking (NULL uses system defaults) */
    int (*load_config)(const char *path, sshd_config_t *cfg, char *err_msg, size_t err_cap);
    int (*load_host_key)(uint8_t out_secret[64], uint8_t out_public[32]);
    bool (*load_userdb)(userdb_t *db, bool shadow);
    long (*getrandom)(void *buf, size_t len);
    long (*bind_socket)(uint32_t listen_addr, uint16_t port, char *err_msg, size_t err_cap);
    int (*listen_socket)(long sock_fd);
} sshd_readiness_ops_t;

/* Execute the pinned startup boot readiness gate sequence:
 * 1. Verify installed root vs. live media & Read config
 *    -> if disabled ("Enabled no"), sets *out_disabled = true, returns 0 (clean exit)
 * 2. Load host key via SYS_HOSTKEY
 *    -> fail = "host key unavailable/corrupt"
 * 3. Load account database (userdb_t)
 *    -> fail = "account database unavailable"
 * 4. Verify SYS_GETRANDOM succeeds
 *    -> fail = "strong randomness unavailable" (fails closed, no weaker fallback)
 * 5. Bind port 22 (or configured port)
 *    -> fail = "bind failed on <addr>:<port>"
 * 6. Listen on socket
 *    -> fail = "Failed to listen on TCP socket."
 *
 * Returns 0 on success, or non-zero on failure.
 * Populates out_diag with the specific failure diagnostic.
 */
int sshd_check_readiness(const char *cfg_path, const sshd_readiness_ops_t *ops,
                         sshd_config_t *out_cfg, uint8_t out_host_secret[64],
                         userdb_t *out_db, long *out_sock_fd,
                         char *out_diag, size_t diag_cap, bool *out_disabled);

/* Helper to format "bind failed on <addr>:<port>" */
void sshd_format_bind_error(char *dst, size_t cap, uint32_t ip, uint16_t port);

/* Helper to read only the public host key (usable by unprivileged worker) */
int sshd_read_host_public_key(uint8_t out_public[32]);

#endif /* USER_SSHD_READINESS_H */
