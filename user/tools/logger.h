#ifndef USER_TOOLS_LOGGER_H
#define USER_TOOLS_LOGGER_H

#include "types.h"
#include "syscall_abi.h"

#define LOGGER_BUFFER_SIZE 512
#define LOGGER_DEFAULT_TAG "logger"

#define LOGGER_LOG_DIR_PRIMARY   "/var/log"
#define LOGGER_LOG_FILE_PRIMARY  "/var/log/messages"
#define LOGGER_LOG_DIR_FALLBACK  "/run"
#define LOGGER_LOG_FILE_FALLBACK "/run/messages"

/* Core CLI entry point */
int logger_main(int argc, char **argv);

/* Sanitization helper:
 * Replaces \r and \n with ' '.
 * Escapes characters < 0x20 and >= 0x7f as \xHH.
 * Returns number of bytes written to dst (excluding null terminator).
 */
size_t logger_sanitize(char *dst, size_t cap, const char *src, size_t src_len);

/* Format a complete log record into buf:
 * If real_sec corresponds to a valid UTC year in [2026, 2100], formats:
 *   [YYYY-MM-DD HH:MM:SS UTC] <tag>: <msg>\n
 * Otherwise falls back to monotonic uptime:
 *   [   12.345s UPTIME] <tag>: <msg>\n
 * Truncates long lines to <= LOGGER_BUFFER_SIZE (512) bytes ending with "...\n".
 * Returns total record length in bytes.
 */
size_t logger_format_record(char *buf, size_t cap,
                            const char *tag,
                            const char *msg, size_t msg_len,
                            int64_t real_sec,
                            int64_t mono_sec, int64_t mono_nsec);

/* Open destination log file with untrusted directory/symlink validation
 * and fallback semantics (/var/log/messages -> /run/messages).
 * Returns non-negative fd or negative error code.
 */
int logger_open_destination(void);

/* Single SYS_WRITE append with short-write detection (never retries).
 * Returns 0 on success, -1 on failure.
 */
int logger_write_record(int fd, const char *buf, size_t len);

/* Stream from stdin until EOF */
int logger_stream_stdin(int out_fd, const char *tag);

#endif /* USER_TOOLS_LOGGER_H */
