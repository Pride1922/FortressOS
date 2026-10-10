#ifndef SHELL_BUILTINS_H
#define SHELL_BUILTINS_H

#include "types.h"

enum builtin {
    CMD_UNKNOWN = 0,
    CMD_HELP,
    CMD_CD,
    CMD_PWD,
    CMD_TYPE,
    CMD_COMMAND,
    CMD_TRUE,
    CMD_FALSE,
    CMD_LS,
    CMD_VIEW,
    CMD_EDIT,
    CMD_MKDIR,
    CMD_RM,
    CMD_MV,
    CMD_CP,
    CMD_TOUCH,
    CMD_SYNC,
    CMD_ECHO,
    CMD_RUN,
    CMD_LAYOUT,
    CMD_REBOOT,
    CMD_SHUTDOWN,
    CMD_POWEROFF,
    CMD_EXIT,
    CMD_DMESG,
    CMD_HISTORY,
    CMD_PROMPT,
    CMD_TERMINAL,
    CMD_SET,
    CMD_UNSET,
    CMD_EXPORT,
    CMD_ENV,
    CMD_ALIAS,
    CMD_UNALIAS,
    CMD_JOBS, CMD_FG, CMD_BG, CMD_KILL,
    CMD_VERSION,
    CMD_CLEAR,
    CMD_PRINTF,
    CMD_UMASK
};

enum builtin builtin_find(const char *name);
void builtin_help(const char *topic);
size_t builtin_count(void);
const char *builtin_name(size_t index);
/* True iff name is safe to run as a pipeline stage via /bin/sh-builtin.
 * Default-deny: any new builtin must explicitly set child_safe = true. */
bool builtin_is_child_safe(const char *name);

#endif /* SHELL_BUILTINS_H */
