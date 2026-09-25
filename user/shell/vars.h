#ifndef SHELL_VARS_H
#define SHELL_VARS_H

#include "types.h"

#define MAX_VARS 64
#define MAX_VAR_NAME 64
#define MAX_VAR_VAL 256
#define MAX_SAVED_LOCAL_VARS 16

typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VAL];
    bool exported;
    bool used;
} shell_var_t;

typedef struct {
    char name[MAX_VAR_NAME];
    char old_value[MAX_VAR_VAL];
    bool existed;
    bool was_exported;
} saved_var_t;

typedef struct {
    saved_var_t saved[MAX_SAVED_LOCAL_VARS];
    int count;
} local_var_scope_t;

void vars_init(void);
const char *vars_get(const char *name);
int vars_set(const char *name, const char *value, bool exported);
int vars_unset(const char *name);
int vars_export(const char *name);
void vars_print_set(void);
void vars_print_env(void);
bool vars_is_valid_name(const char *name);
bool vars_is_assignment(const char *str, char *name_out, size_t name_cap, const char **val_out);

/* Command-local assignment scoping */
void vars_scope_begin(local_var_scope_t *scope);
int vars_scope_set(local_var_scope_t *scope, const char *name, const char *value);
void vars_scope_end(local_var_scope_t *scope);

/* Build envp pointer vector and strings for SYS_SPAWN_EXT */
int vars_build_envp(char env_strings[32][MAX_VAR_NAME + MAX_VAR_VAL + 2],
                    const char *envp_ptrs[33]);

#endif /* SHELL_VARS_H */
