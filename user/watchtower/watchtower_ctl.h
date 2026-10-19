#ifndef USER_WATCHTOWER_CTL_H
#define USER_WATCHTOWER_CTL_H

#include "types.h"
#include "watchtower.h"

#define WATCHTOWER_CTL_LOCK_FILE "/run/watchtower/ctl.lock"
#define WATCHTOWER_CTL_REQ_FILE  "/run/watchtower/ctl.req"
#define WATCHTOWER_CTL_RES_FILE  "/run/watchtower/ctl.res"
#define WATCHTOWER_CTL_MAX_REQ   512
#define WATCHTOWER_CTL_MAX_RES   4096

/* Process a single control command string.
 * caller_uid / caller_gid are used for authorization:
 * - Read commands (LIST, STATUS, CHECK) permitted for all callers.
 * - Mutating commands (START, STOP, RESTART) require caller_uid == 0 or caller_gid == 10 (wheel).
 *
 * Returns 0 on success, negative error code on failure.
 * Response is written into res buffer as NUL-terminated text. */
int watchtower_ctl_process_request(watchtower_table_t *tbl, const char *req,
                                   uint32_t caller_uid, uint32_t caller_gid,
                                   char *res, size_t res_cap,
                                   const watchtower_sys_ops_t *ops);

/* Configure directories for service file lookup/mutation (defaults to /etc and /mnt/.fortress) */
void watchtower_ctl_set_dirs(const char *installed_dir, const char *live_dir);

/* Configure directory for service log lookup */
void watchtower_ctl_set_log_dir(const char *log_dir);

#endif /* USER_WATCHTOWER_CTL_H */
