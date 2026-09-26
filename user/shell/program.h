#ifndef SHELL_PROGRAM_H
#define SHELL_PROGRAM_H
#include "io.h"

/* No wait or diagnostic on launch; caller owns each returned PID. */
long program_launch(const char *path, const char *const *argv,
                    const char *const *envp, const spawn_fd_action_t *actions,
                    uint32_t action_count);
int program_error(long error);
int program_wait(long pid, int64_t *status);
/* Resolve using current scoped PATH; output remains owned by the caller. */
int program_resolve(const char *name, char path[VFS_MAX_PATH]);
#endif
