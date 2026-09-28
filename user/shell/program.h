#ifndef SHELL_PROGRAM_H
#define SHELL_PROGRAM_H
#include "io.h"

/* No wait or diagnostic on launch; caller owns each returned PID. */
long program_launch(const char *path, const char *const *argv,
                    const char *const *envp, const spawn_fd_action_t *actions,
                    uint32_t action_count);
/* V2 staged launch: child is built but does not run until GROUP_RELEASE.
 * pgid==0 on the first member creates a new group; >0 on subsequent members
 * joins that group. Returns child PID or negative error. */
long program_launch_job(const char *path, const char *const *argv,
                        const char *const *envp, const spawn_fd_action_t *actions,
                        uint32_t action_count, long pgid);
int program_error(long error);
int program_wait(long pid, int64_t *status);
/* Wait using SYS_WAITPID with stop/continue awareness. */
int program_waitpid(long pid, uint64_t *status, uint32_t options);
/* Resolve using current scoped PATH; output remains owned by the caller. */
int program_resolve(const char *name, char path[VFS_MAX_PATH]);
#endif
