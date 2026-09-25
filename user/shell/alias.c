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

static char s_alias_tmp_buf[4096];
static char s_seen_aliases[MAX_ALIAS_DEPTH][MAX_ALIAS_NAME];

bool alias_expand_line(const char *line, char *out_buf, size_t out_cap) {
    if (!line || !out_buf || out_cap == 0) return false;

    str_copy(out_buf, line, out_cap);

    bool expanded_any = false;
    size_t cursor = 0;
    bool eligible = true;
    int depth = 0;

    while (out_buf[cursor]) {
        /* 1. Skip leading whitespace */
        while (out_buf[cursor] && is_ws(out_buf[cursor])) {
            cursor++;
        }
        if (!out_buf[cursor]) break;

        /* 2. Check for command separators: ;, &&, ||, |, & */
        if (out_buf[cursor] == ';') {
            cursor++;
            eligible = true;
            depth = 0;
            continue;
        }
        if (out_buf[cursor] == '&') {
            cursor++;
            if (out_buf[cursor] == '&') cursor++;
            eligible = true;
            depth = 0;
            continue;
        }
        if (out_buf[cursor] == '|') {
            cursor++;
            if (out_buf[cursor] == '|') cursor++;
            eligible = true;
            depth = 0;
            continue;
        }

        /* 3. If not in a command position eligible for alias expansion, skip token */
        if (!eligible) {
            while (out_buf[cursor] && !is_ws(out_buf[cursor]) &&
                   out_buf[cursor] != ';' && out_buf[cursor] != '&' && out_buf[cursor] != '|') {
                if (out_buf[cursor] == '\'') {
                    cursor++;
                    while (out_buf[cursor] && out_buf[cursor] != '\'') cursor++;
                    if (out_buf[cursor] == '\'') cursor++;
                } else if (out_buf[cursor] == '"') {
                    cursor++;
                    while (out_buf[cursor] && out_buf[cursor] != '"') {
                        if (out_buf[cursor] == '\\' && out_buf[cursor + 1]) cursor++;
                        cursor++;
                    }
                    if (out_buf[cursor] == '"') cursor++;
                } else if (out_buf[cursor] == '\\') {
                    cursor++;
                    if (out_buf[cursor]) cursor++;
                } else {
                    cursor++;
                }
            }
            continue;
        }

        /* 4. Eligible position: check if quoted or escaped */
        if (out_buf[cursor] == '\'' || out_buf[cursor] == '"' || out_buf[cursor] == '\\') {
            /* Quoting (e.g. \cmd, 'cmd', "cmd") suppresses alias expansion.
             * Preserve the backslash/quotes so the lexer strips escape syntax
             * and recognizes it as a quoted/escaped command name. */
            eligible = false;
            depth = 0;
            if (out_buf[cursor] == '\\') {
                cursor++;
                if (out_buf[cursor]) cursor++;
            } else if (out_buf[cursor] == '\'') {
                cursor++;
                while (out_buf[cursor] && out_buf[cursor] != '\'') cursor++;
                if (out_buf[cursor] == '\'') cursor++;
            } else if (out_buf[cursor] == '"') {
                cursor++;
                while (out_buf[cursor] && out_buf[cursor] != '"') {
                    if (out_buf[cursor] == '\\' && out_buf[cursor + 1]) cursor++;
                    cursor++;
                }
                if (out_buf[cursor] == '"') cursor++;
            }
            continue;
        }

        /* 5. Extract unquoted command word */
        size_t start = cursor;
        size_t end = start;
        while (out_buf[end] && !is_ws(out_buf[end]) &&
               out_buf[end] != ';' && out_buf[end] != '&' && out_buf[end] != '|' &&
               out_buf[end] != '\'' && out_buf[end] != '"' && out_buf[end] != '\\') {
            end++;
        }

        size_t token_len = end - start;
        if (token_len == 0 || token_len >= MAX_ALIAS_NAME) {
            eligible = false;
            depth = 0;
            cursor = end;
            continue;
        }

        char word[MAX_ALIAS_NAME];
        for (size_t i = 0; i < token_len; i++) word[i] = out_buf[start + i];
        word[token_len] = '\0';

        /* 6. Check cycle detection */
        bool cycle = false;
        for (int k = 0; k < depth; k++) {
            if (str_eq(s_seen_aliases[k], word)) {
                cycle = true;
                break;
            }
        }
        if (cycle || depth >= MAX_ALIAS_DEPTH) {
            eligible = false;
            depth = 0;
            cursor = end;
            continue;
        }

        /* 7. Check if word is an alias */
        const char *val = alias_get(word);
        if (!val) {
            eligible = false;
            depth = 0;
            cursor = end;
            continue;
        }

        /* Record word in active expansion chain */
        str_copy(s_seen_aliases[depth], word, MAX_ALIAS_NAME);
        depth++;
        expanded_any = true;

        /* Check if alias value ends in a blank (<space> or <tab>) */
        size_t vlen = str_len(val);
        bool ends_with_blank = (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'));

        /* If alias ends with a blank, consume any whitespace following the alias word
         * to avoid doubling whitespace between the alias and the next word. */
        if (ends_with_blank) {
            while (out_buf[end] && is_ws(out_buf[end])) {
                end++;
            }
        }

        /* Splice replacement: out_buf[0..start-1] + val + out_buf[end..] */
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

        if (ends_with_blank) {
            /* POSIX §2.3.1: If the alias value ends with a blank, the next command word
             * following the alias is also eligible for alias substitution.
             * Advance cursor past the inserted alias text, leave eligible = true,
             * and reset depth = 0 for the next command word. */
            cursor = start + vlen;
            eligible = true;
            depth = 0;
        } else {
            /* The replacement does not end in a blank. The first word of the replacement
             * may itself be an alias, so re-check at 'start' within the same cycle chain. */
            cursor = start;
            eligible = true;
        }
    }

    return expanded_any;
}
