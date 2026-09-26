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
    CMD_CAT,
    CMD_EDIT,
    CMD_MKDIR,
    CMD_RM,
    CMD_MV,
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
    CMD_VERSION
};

enum builtin builtin_find(const char *name);
void builtin_help(const char *topic);
size_t builtin_count(void);
const char *builtin_name(size_t index);

#endif /* SHELL_BUILTINS_H */
