#ifndef SHELL_BUILTIN_EXEC_H
#define SHELL_BUILTIN_EXEC_H

#include "types.h"

/*
 * Shared builtin execution context.  The parent shell fills this from its own
 * globals before calling a handler.  The /bin/sh-builtin runner fills it from
 * its argv/envp at entry.  Handlers must not read any global shell state.
 */
typedef struct {
    const char *const *envp; /* Exported environment (NULL-terminated, may be NULL) */
    const char *path;        /* Value of PATH from envp (may be NULL → "/bin" fallback) */
} builtin_ctx_t;

/*
 * Execute a child-safe builtin by name.
 *
 * argc/argv are the already-expanded command words (argv[0] is the command
 * name).  ctx provides the environment snapshot.
 *
 * Returns:
 *   0   on success
 *   1   on error
 *   1 on stdout EPIPE if SIGPIPE is survived (quietly, no diagnostic written)
 *   2   if name is unknown or forbidden (not in child-safe allowlist)
 *
 * All output goes to fd 1 (stdout) or fd 2 (stderr) via write_bytes_fd().
 * The first stdout failure stops further writes and is reported as above.
 */
int builtin_exec(int argc, const char *const *argv, const builtin_ctx_t *ctx);

/* Build a builtin_ctx_t from a NULL-terminated envp array. */
builtin_ctx_t builtin_ctx_from_envp(const char *const *envp);

#endif /* SHELL_BUILTIN_EXEC_H */
