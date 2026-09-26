#include "vars.h"
#include "io.h"

static shell_var_t g_vars[MAX_VARS];

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static void str_copy(char *dest, const char *src, size_t cap) {
    if (!dest || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = '\0';
}

bool vars_is_valid_name(const char *name) {
    if (!name || !*name) return false;
    char c = name[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
        return false;
    }
    for (size_t i = 1; name[i]; i++) {
        c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

bool vars_is_assignment(const char *str, char *name_out, size_t name_cap, const char **val_out) {
    if (!str || !*str) return false;
    size_t eq_idx = 0;
    while (str[eq_idx] && str[eq_idx] != '=') eq_idx++;
    if (str[eq_idx] != '=' || eq_idx == 0) return false;

    if (name_out && name_cap > 0) {
        size_t n = eq_idx < name_cap - 1 ? eq_idx : name_cap - 1;
        for (size_t i = 0; i < n; i++) name_out[i] = str[i];
        name_out[n] = '\0';
        if (!vars_is_valid_name(name_out)) return false;
    } else {
        char tmp[MAX_VAR_NAME];
        size_t n = eq_idx < sizeof(tmp) - 1 ? eq_idx : sizeof(tmp) - 1;
        for (size_t i = 0; i < n; i++) tmp[i] = str[i];
        tmp[n] = '\0';
        if (!vars_is_valid_name(tmp)) return false;
    }

    if (val_out) {
        *val_out = &str[eq_idx + 1];
    }
    return true;
}

void vars_init(void) {
    for (int i = 0; i < MAX_VARS; i++) {
        g_vars[i].used = false;
        g_vars[i].exported = false;
        g_vars[i].name[0] = '\0';
        g_vars[i].value[0] = '\0';
    }
    vars_set("PATH", "/bin", true);
    vars_set("HOME", "/", true);
    vars_set("PS1", "fortress> ", false);
    vars_set("PS2", "> ", false);
}

const char *vars_get(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used && str_eq(g_vars[i].name, name)) {
            return g_vars[i].value;
        }
    }
    return 0;
}

int vars_set(const char *name, const char *value, bool exported) {
    if (!vars_is_valid_name(name)) return -1;
    if (str_len(name) >= MAX_VAR_NAME) return -1;

    /* Check if already present */
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used && str_eq(g_vars[i].name, name)) {
            str_copy(g_vars[i].value, value, MAX_VAR_VAL);
            if (exported) g_vars[i].exported = true;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_VARS; i++) {
        if (!g_vars[i].used) {
            g_vars[i].used = true;
            g_vars[i].exported = exported;
            str_copy(g_vars[i].name, name, MAX_VAR_NAME);
            str_copy(g_vars[i].value, value, MAX_VAR_VAL);
            return 0;
        }
    }

    return -1; /* Table full */
}

int vars_unset(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used && str_eq(g_vars[i].name, name)) {
            g_vars[i].used = false;
            g_vars[i].exported = false;
            g_vars[i].name[0] = '\0';
            g_vars[i].value[0] = '\0';
            return 0;
        }
    }
    return 0;
}

int vars_export(const char *name) {
    if (!name || !*name) return -1;
    char var_name[MAX_VAR_NAME];
    const char *var_val = 0;
    if (vars_is_assignment(name, var_name, sizeof(var_name), &var_val)) {
        return vars_set(var_name, var_val, true);
    }

    if (!vars_is_valid_name(name)) return -1;
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used && str_eq(g_vars[i].name, name)) {
            g_vars[i].exported = true;
            return 0;
        }
    }
    /* If not found, define it empty and export */
    return vars_set(name, "", true);
}

void vars_print_set(void) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used) {
            puts(g_vars[i].name);
            puts("=");
            puts(g_vars[i].value);
            puts("\n");
        }
    }
}

void vars_print_env(void) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_vars[i].used && g_vars[i].exported) {
            puts(g_vars[i].name);
            puts("=");
            puts(g_vars[i].value);
            puts("\n");
        }
    }
}

void vars_scope_begin(local_var_scope_t *scope) {
    if (!scope) return;
    scope->count = 0;
}

int vars_scope_set(local_var_scope_t *scope, const char *name, const char *value) {
    if (!scope || !name || scope->count >= MAX_SAVED_LOCAL_VARS) return -1;

    saved_var_t *s = &scope->saved[scope->count++];
    str_copy(s->name, name, MAX_VAR_NAME);

    const char *old = vars_get(name);
    if (old) {
        s->existed = true;
        str_copy(s->old_value, old, MAX_VAR_VAL);
        s->was_exported = false;
        for (int i = 0; i < MAX_VARS; i++) {
            if (g_vars[i].used && str_eq(g_vars[i].name, name)) {
                s->was_exported = g_vars[i].exported;
                break;
            }
        }
    } else {
        s->existed = false;
        s->old_value[0] = '\0';
        s->was_exported = false;
    }

    return vars_set(name, value, true);
}

void vars_scope_end(local_var_scope_t *scope) {
    if (!scope) return;
    for (int i = scope->count - 1; i >= 0; i--) {
        saved_var_t *s = &scope->saved[i];
        if (s->existed) {
            vars_set(s->name, s->old_value, s->was_exported);
            /* vars_set deliberately preserves export on ordinary assignments.
             * A temporary command scope must restore it exactly instead. */
            for (int j = 0; j < MAX_VARS; j++) {
                if (g_vars[j].used && str_eq(g_vars[j].name, s->name)) {
                    g_vars[j].exported = s->was_exported;
                    break;
                }
            }
        } else {
            vars_unset(s->name);
        }
    }
    scope->count = 0;
}

int vars_build_envp(char env_strings[32][MAX_VAR_NAME + MAX_VAR_VAL + 2],
                    const char *envp_ptrs[33]) {
    int count = 0;
    for (int i = 0; i < MAX_VARS && count < 32; i++) {
        if (g_vars[i].used && g_vars[i].exported) {
            char *buf = env_strings[count];
            size_t nlen = str_len(g_vars[i].name);
            size_t vlen = str_len(g_vars[i].value);
            size_t p = 0;
            for (size_t k = 0; k < nlen && p + 1 < MAX_VAR_NAME + MAX_VAR_VAL + 2; k++) {
                buf[p++] = g_vars[i].name[k];
            }
            if (p + 1 < MAX_VAR_NAME + MAX_VAR_VAL + 2) buf[p++] = '=';
            for (size_t k = 0; k < vlen && p + 1 < MAX_VAR_NAME + MAX_VAR_VAL + 2; k++) {
                buf[p++] = g_vars[i].value[k];
            }
            buf[p] = '\0';
            envp_ptrs[count] = buf;
            count++;
        }
    }
    envp_ptrs[count] = 0;
    return count;
}
