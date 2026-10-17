#ifndef SHELL_HISTORY_EXPAND_H
#define SHELL_HISTORY_EXPAND_H
#include "types.h"
/* Exact unquoted sudo !! shorthand only. 0: unchanged, 1: expanded,
 * -1: no previous command, -2: expansion exceeds output capacity. */
int history_expand_sudo(const char *line, const char *previous, char *out, size_t cap);
#endif
