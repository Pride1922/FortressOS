#ifndef SHELL_ALIAS_H
#define SHELL_ALIAS_H

#include "types.h"

#define MAX_ALIASES 32
#define MAX_ALIAS_NAME 32
#define MAX_ALIAS_VAL 256
#define MAX_ALIAS_DEPTH 16

typedef struct {
    char name[MAX_ALIAS_NAME];
    char value[MAX_ALIAS_VAL];
    bool used;
} shell_alias_t;

void alias_init(void);
const char *alias_get(const char *name);
int alias_set(const char *name, const char *value);
int alias_unset(const char *name);
void alias_print_all(void);

/* Expand first word of simple command line if it matches an alias.
 * Returns true if an expansion took place, false otherwise.
 * Cycles and recursion are bounded to MAX_ALIAS_DEPTH. */
bool alias_expand_line(const char *line, char *out_buf, size_t out_cap);

#endif /* SHELL_ALIAS_H */
