#ifndef SHELL_EXPAND_H
#define SHELL_EXPAND_H

#include "types.h"
#include "parser.h"

#define MAX_EXPANDED_ARGS 64

typedef struct {
    char *argv[MAX_EXPANDED_ARGS + 1];
    int argc;
} expanded_cmd_t;

/* Execute expansion pipeline on a parsed command:
 * 1. Tilde expansion
 * 2. Parameter expansion ($VAR, ${VAR}, $?, $$)
 * 3. Word splitting on unquoted parameter expansion results
 * 4. Pathname expansion (globbing with *, ?, [...])
 * 5. Quote removal
 *
 * Populates out_cmd. Returns 0 on success, or -1 on error. */
int expand_command(const parse_cmd_t *in_cmd, int64_t last_status, expanded_cmd_t *out_cmd);

/* Glob pattern matching */
bool glob_match(const char *pattern, const char *str);

#endif /* SHELL_EXPAND_H */
