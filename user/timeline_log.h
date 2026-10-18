#ifndef USER_TIMELINE_LOG_H
#define USER_TIMELINE_LOG_H

#include "types.h"
#include "syscall_abi.h"

#define TIMELINE_LOG_BUFFER_SIZE 512
#define TIMELINE_DEFAULT_TAG "daemon"

#define TIMELINE_LOG_DIR_PRIMARY   "/var/log"
#define TIMELINE_LOG_FILE_PRIMARY  "/var/log/messages"
#define TIMELINE_LOG_DIR_FALLBACK  "/run"
#define TIMELINE_LOG_FILE_FALLBACK "/run/messages"

/* Rate-limiting interval for stderr diagnostics (in seconds) */
#define TIMELINE_DIAG_RATE_LIMIT_SEC 5

/* Append a formatted audit event to the system timeline (/var/log/messages).
 * Formats message in-process, applies UTC timestamp (or uptime fallback),
 * validates security boundaries (untrusted directory/file ownership, mode, symlink),
 * falls back to /run/messages on ENOENT/EROFS, and writes via a single append write.
 * Emits rate-limited diagnostics to stderr on failure.
 * Returns 0 on success, or -1 on failure. Never terminates the caller.
 */
int timeline_log(const char *tag, const char *fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)));

int timeline_vlog(const char *tag, const char *fmt, __builtin_va_list ap);

#ifdef TIMELINE_LOG_HOST_TEST
void timeline_log_reset_rate_limit(void);
#endif

#endif /* USER_TIMELINE_LOG_H */
