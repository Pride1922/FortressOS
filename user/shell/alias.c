#include "alias.h"
#include "io.h"

static shell_alias_t g_aliases[MAX_ALIASES];

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

void alias_init(void) {
    for (int i = 0; i < MAX_ALIASES; i++) {
        g_aliases[i].used = false;
        g_aliases[i].name[0] = '\0';
        g_aliases[i].value[0] = '\0';
    }
}

const char *alias_get(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (g_aliases[i].used && str_eq(g_aliases[i].name, name)) {
            return g_aliases[i].value;
        }
    }
    return 0;
}

int alias_set(const char *name, const char *value) {
    if (!name || !*name || !value) return -1;
    if (str_len(name) >= MAX_ALIAS_NAME) return -1;

    for (int i = 0; i < MAX_ALIASES; i++) {
        if (g_aliases[i].used && str_eq(g_aliases[i].name, name)) {
            str_copy(g_aliases[i].value, value, MAX_ALIAS_VAL);
            return 0;
        }
    }

    for (int i = 0; i < MAX_ALIASES; i++) {
        if (!g_aliases[i].used) {
            g_aliases[i].used = true;
            str_copy(g_aliases[i].name, name, MAX_ALIAS_NAME);
            str_copy(g_aliases[i].value, value, MAX_ALIAS_VAL);
            return 0;
        }
    }

    return -1;
}

int alias_unset(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (g_aliases[i].used && str_eq(g_aliases[i].name, name)) {
            g_aliases[i].used = false;
            g_aliases[i].name[0] = '\0';
            g_aliases[i].value[0] = '\0';
            return 0;
        }
    }
    return 0;
}

void alias_print_all(void) {
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (g_aliases[i].used) {
            puts("alias ");
            puts(g_aliases[i].name);
            puts("='");
            puts(g_aliases[i].value);
            puts("'\n");
        }
    }
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

static char s_alias_tmp_buf[1024];

bool alias_expand_line(const char *line, char *out_buf, size_t out_cap) {
    if (!line || !out_buf || out_cap == 0) return false;

    str_copy(out_buf, line, out_cap);

    const char *seen_aliases[MAX_ALIAS_DEPTH];
    int depth = 0;
    bool expanded_any = false;

    while (depth < MAX_ALIAS_DEPTH) {
        /* Skip leading whitespace */
        size_t start = 0;
        while (out_buf[start] && is_ws(out_buf[start])) start++;
        if (!out_buf[start]) break;

        /* Do not expand if quoted */
        if (out_buf[start] == '\'' || out_buf[start] == '"' || out_buf[start] == '\\') {
            break;
        }

        /* Extract first token */
        size_t end = start;
        while (out_buf[end] && !is_ws(out_buf[end]) && out_buf[end] != ';' &&
               out_buf[end] != '&' && out_buf[end] != '|' &&
               out_buf[end] != '\'' && out_buf[end] != '"') {
            end++;
        }

        size_t token_len = end - start;
        if (token_len == 0 || token_len >= MAX_ALIAS_NAME) break;

        char first_word[MAX_ALIAS_NAME];
        for (size_t i = 0; i < token_len; i++) first_word[i] = out_buf[start + i];
        first_word[token_len] = '\0';

        /* Check cycle detection */
        bool already_seen = false;
        for (int k = 0; k < depth; k++) {
            if (str_eq(seen_aliases[k], first_word)) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) break;

        const char *val = alias_get(first_word);
        if (!val) break;

        seen_aliases[depth++] = first_word;
        expanded_any = true;

        /* Reassemble: prefix (leading whitespace) + val + suffix */
        size_t p = 0;
        for (size_t i = 0; i < start && p + 1 < sizeof(s_alias_tmp_buf); i++) {
            s_alias_tmp_buf[p++] = out_buf[i];
        }
        for (size_t i = 0; val[i] && p + 1 < sizeof(s_alias_tmp_buf); i++) {
            s_alias_tmp_buf[p++] = val[i];
        }
        for (size_t i = end; out_buf[i] && p + 1 < sizeof(s_alias_tmp_buf); i++) {
            s_alias_tmp_buf[p++] = out_buf[i];
        }
        s_alias_tmp_buf[p] = '\0';

        str_copy(out_buf, s_alias_tmp_buf, out_cap);
    }

    return expanded_any;
}
