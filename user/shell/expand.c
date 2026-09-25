#include "expand.h"
#include "vars.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "io.h"

#define QUOTE_EXPANDED_UNQUOTED 4

static char s_expand_pool[LINE_CAP * 4];
static size_t s_expand_pool_used;

static char *s_split_words[MAX_EXPANDED_ARGS];
static uint8_t *s_split_flags[MAX_EXPANDED_ARGS];
static bool s_split_has_quotes[MAX_EXPANDED_ARGS];
static int s_split_count;

static char s_glob_names[64][VFS_MAX_NAME];
static int s_glob_count;

static char s_exp_chars[LINE_CAP * 2];
static uint8_t s_exp_flags[LINE_CAP * 2];

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_cmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
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

static inline bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

/* Match glob pattern against str.
 * Handles '*', '?', and '[abc]', '[!abc]', '[a-z]' */
bool glob_match(const char *pattern, const char *str) {
    if (!pattern || !str) return false;

    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return true; /* trailing * matches everything */
            while (*str) {
                if (glob_match(pattern, str)) return true;
                str++;
            }
            return false;
        } else if (*pattern == '?') {
            if (!*str) return false;
            pattern++;
            str++;
        } else if (*pattern == '[') {
            pattern++;
            bool negate = false;
            if (*pattern == '!' || *pattern == '^') {
                negate = true;
                pattern++;
            }
            bool matched = false;
            char last_c = 0;
            while (*pattern && *pattern != ']') {
                if (*pattern == '-' && last_c != 0 && pattern[1] && pattern[1] != ']') {
                    pattern++;
                    char end_c = *pattern++;
                    if (*str >= last_c && *str <= end_c) matched = true;
                    last_c = 0;
                } else {
                    last_c = *pattern++;
                    if (*str == last_c) matched = true;
                }
            }
            if (*pattern == ']') pattern++;
            if (negate ? matched : !matched) return false;
            if (!*str) return false;
            str++;
        } else {
            if (*pattern != *str) return false;
            pattern++;
            str++;
        }
    }
    return *str == '\0';
}

static void sort_glob_names(int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (str_cmp(s_glob_names[j], s_glob_names[j + 1]) > 0) {
                char tmp[VFS_MAX_NAME];
                str_copy(tmp, s_glob_names[j], sizeof(tmp));
                str_copy(s_glob_names[j], s_glob_names[j + 1], sizeof(tmp));
                str_copy(s_glob_names[j + 1], tmp, sizeof(tmp));
            }
        }
    }
}

static int expand_glob(const char *word, const uint8_t *qflags, expanded_cmd_t *out_cmd) {
    /* 1. Check if word contains any unquoted glob characters */
    bool has_glob = false;
    for (size_t i = 0; word[i]; i++) {
        if ((word[i] == '*' || word[i] == '?' || word[i] == '[') &&
            (!qflags || qflags[i] == QUOTE_NONE)) {
            has_glob = true;
            break;
        }
    }
    if (!has_glob) {
        if (out_cmd->argc < MAX_EXPANDED_ARGS) {
            out_cmd->argv[out_cmd->argc++] = (char *)word;
        }
        return 0;
    }

    /* 2. Separate directory and pattern */
    size_t last_slash = (size_t)-1;
    for (size_t i = 0; word[i]; i++) {
        if (word[i] == '/') last_slash = i;
    }

    char dir_part[VFS_MAX_PATH];
    const char *file_pat = word;

    if (last_slash == (size_t)-1) {
        dir_part[0] = '.';
        dir_part[1] = '\0';
        file_pat = word;
    } else if (last_slash == 0) {
        dir_part[0] = '/';
        dir_part[1] = '\0';
        file_pat = &word[1];
    } else {
        size_t n = last_slash < sizeof(dir_part) - 1 ? last_slash : sizeof(dir_part) - 1;
        for (size_t k = 0; k < n; k++) dir_part[k] = word[k];
        dir_part[n] = '\0';
        file_pat = &word[last_slash + 1];
    }

    /* 3. Open directory and collect matches */
    s_glob_count = 0;
    long fd = call(SYS_OPEN, (uintptr_t)dir_part, 0, 0);
    if (fd >= 0) {
        vfs_dirent_t entry;
        long r;
        while ((r = call(SYS_READDIR, fd, (uintptr_t)&entry, 0)) == 1) {
            /* Skip hidden files unless pattern explicitly starts with '.' */
            if (entry.name[0] == '.' && file_pat[0] != '.') continue;

            if (glob_match(file_pat, entry.name)) {
                if (s_glob_count < 64) {
                    str_copy(s_glob_names[s_glob_count++], entry.name, VFS_MAX_NAME);
                }
            }
        }
        (void)call(SYS_CLOSE, fd, 0, 0);
    }

    /* 4. If no matches found: leave pattern literal */
    if (s_glob_count == 0) {
        if (out_cmd->argc < MAX_EXPANDED_ARGS) {
            out_cmd->argv[out_cmd->argc++] = (char *)word;
        }
        return 0;
    }

    /* 5. Sort matches alphabetically */
    sort_glob_names(s_glob_count);

    /* 6. Add matching paths to out_cmd */
    for (int i = 0; i < s_glob_count; i++) {
        if (out_cmd->argc >= MAX_EXPANDED_ARGS) break;

        char path_buf[VFS_MAX_PATH];
        size_t p = 0;
        if (last_slash != (size_t)-1) {
            size_t dlen = str_len(dir_part);
            for (size_t k = 0; k < dlen && p + 1 < sizeof(path_buf); k++) {
                path_buf[p++] = dir_part[k];
            }
            if (p > 0 && path_buf[p - 1] != '/' && p + 1 < sizeof(path_buf)) {
                path_buf[p++] = '/';
            }
        }
        size_t nlen = str_len(s_glob_names[i]);
        for (size_t k = 0; k < nlen && p + 1 < sizeof(path_buf); k++) {
            path_buf[p++] = s_glob_names[i][k];
        }
        path_buf[p] = '\0';

        size_t needed = p + 1;
        if (s_expand_pool_used + needed <= sizeof(s_expand_pool)) {
            char *dest = &s_expand_pool[s_expand_pool_used];
            str_copy(dest, path_buf, needed);
            s_expand_pool_used += needed;
            out_cmd->argv[out_cmd->argc++] = dest;
        }
    }

    return 0;
}

int expand_command(const parse_cmd_t *in_cmd, int64_t last_status, expanded_cmd_t *out_cmd) {
    if (!in_cmd || !out_cmd) return -1;

    out_cmd->argc = 0;
    s_expand_pool_used = 0;
    s_split_count = 0;

    for (int a = 0; a < in_cmd->argc; a++) {
        const char *word = in_cmd->argv[a];
        const uint8_t *qflags = in_cmd->quote_flags[a];
        bool has_quotes = in_cmd->has_quotes[a];

        if (!word) continue;

        /* Step 1 & 2: Tilde & Parameter Expansion into intermediate buffer */
        size_t exp_len = 0;
        size_t i = 0;

        /* Tilde expansion at word start */
        if (word[0] == '~' && (!qflags || qflags[0] == QUOTE_NONE) &&
            (word[1] == '/' || word[1] == '\0')) {
            const char *home = vars_get("HOME");
            if (!home || !*home) home = "/";
            while (*home && exp_len + 1 < sizeof(s_exp_chars)) {
                s_exp_flags[exp_len] = QUOTE_NONE;
                s_exp_chars[exp_len++] = *home++;
            }
            i = 1;
        }

        while (word[i]) {
            uint8_t qf = qflags ? qflags[i] : QUOTE_NONE;

            /* Check for parameter expansion */
            if (word[i] == '$' && (qf == QUOTE_NONE || qf == QUOTE_DOUBLE)) {
                i++; /* skip $ */
                if (word[i] == '?') {
                    i++;
                    char num_buf[32];
                    int n = 0;
                    uint64_t val = (last_status < 0) ? (0 - (uint64_t)last_status) : (uint64_t)last_status;
                    if (val == 0) num_buf[n++] = '0';
                    else {
                        char tmp[32];
                        int t = 0;
                        while (val > 0) {
                            tmp[t++] = '0' + (val % 10);
                            val /= 10;
                        }
                        if (last_status < 0) num_buf[n++] = '-';
                        while (t > 0) num_buf[n++] = tmp[--t];
                    }
                    num_buf[n] = '\0';
                    for (int k = 0; k < n && exp_len + 1 < sizeof(s_exp_chars); k++) {
                        s_exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        s_exp_chars[exp_len++] = num_buf[k];
                    }
                    continue;
                }

                if (word[i] == '$') {
                    i++;
                    if (exp_len + 1 < sizeof(s_exp_chars)) {
                        s_exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        s_exp_chars[exp_len++] = '1';
                    }
                    continue;
                }

                /* Variable name: $VAR or ${VAR} */
                bool braced = false;
                if (word[i] == '{') {
                    braced = true;
                    i++;
                }

                char var_name[MAX_VAR_NAME];
                size_t vn_len = 0;
                while (word[i]) {
                    char c = word[i];
                    if (braced && c == '}') {
                        i++;
                        break;
                    }
                    if (!braced && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '_')) {
                        break;
                    }
                    if (vn_len + 1 < sizeof(var_name)) {
                        var_name[vn_len++] = c;
                    }
                    i++;
                }
                var_name[vn_len] = '\0';

                const char *val = vars_get(var_name);
                if (val) {
                    while (*val && exp_len + 1 < sizeof(s_exp_chars)) {
                        s_exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        s_exp_chars[exp_len++] = *val++;
                    }
                }
                continue;
            }

            /* Regular character */
            if (exp_len + 1 < sizeof(s_exp_chars)) {
                s_exp_flags[exp_len] = qf;
                s_exp_chars[exp_len++] = word[i];
            }
            i++;
        }
        s_exp_chars[exp_len] = '\0';

        /* Step 3: Word Splitting on QUOTE_EXPANDED_UNQUOTED whitespace */
        if (exp_len == 0) {
            if (has_quotes) {
                /* Retain empty argument */
                if (s_split_count < MAX_EXPANDED_ARGS && s_expand_pool_used + 1 <= sizeof(s_expand_pool)) {
                    char *dest = &s_expand_pool[s_expand_pool_used];
                    *dest = '\0';
                    s_expand_pool_used++;
                    s_split_words[s_split_count] = dest;
                    s_split_flags[s_split_count] = 0;
                    s_split_has_quotes[s_split_count] = true;
                    s_split_count++;
                }
            }
            continue;
        }

        size_t cur_start = 0;
        bool in_word = false;

        for (size_t k = 0; k < exp_len; k++) {
            if (s_exp_flags[k] == QUOTE_EXPANDED_UNQUOTED && is_whitespace(s_exp_chars[k])) {
                if (in_word) {
                    size_t wlen = k - cur_start;
                    if (s_split_count < MAX_EXPANDED_ARGS &&
                        s_expand_pool_used + wlen + 1 + wlen + 1 <= sizeof(s_expand_pool)) {
                        char *dest = &s_expand_pool[s_expand_pool_used];
                        uint8_t *qdest = (uint8_t *)&s_expand_pool[s_expand_pool_used + wlen + 1];
                        for (size_t j = 0; j < wlen; j++) {
                            dest[j] = s_exp_chars[cur_start + j];
                            qdest[j] = s_exp_flags[cur_start + j];
                        }
                        dest[wlen] = '\0';
                        qdest[wlen] = 0;
                        s_expand_pool_used += (wlen + 1) * 2;

                        s_split_words[s_split_count] = dest;
                        s_split_flags[s_split_count] = qdest;
                        s_split_has_quotes[s_split_count] = has_quotes;
                        s_split_count++;
                    }
                    in_word = false;
                }
            } else {
                if (!in_word) {
                    cur_start = k;
                    in_word = true;
                }
            }
        }

        if (in_word) {
            size_t wlen = exp_len - cur_start;
            if (s_split_count < MAX_EXPANDED_ARGS &&
                s_expand_pool_used + wlen + 1 + wlen + 1 <= sizeof(s_expand_pool)) {
                char *dest = &s_expand_pool[s_expand_pool_used];
                uint8_t *qdest = (uint8_t *)&s_expand_pool[s_expand_pool_used + wlen + 1];
                for (size_t j = 0; j < wlen; j++) {
                    dest[j] = s_exp_chars[cur_start + j];
                    qdest[j] = s_exp_flags[cur_start + j];
                }
                dest[wlen] = '\0';
                qdest[wlen] = 0;
                s_expand_pool_used += (wlen + 1) * 2;

                s_split_words[s_split_count] = dest;
                s_split_flags[s_split_count] = qdest;
                s_split_has_quotes[s_split_count] = has_quotes;
                s_split_count++;
            }
        }
    }

    /* Step 4: Pathname Expansion (Globbing) on each split word */
    for (int w = 0; w < s_split_count; w++) {
        if (s_split_has_quotes[w]) {
            /* If the word was an explicitly quoted empty string e.g. "", preserve it without globbing */
            if (s_split_words[w][0] == '\0') {
                if (out_cmd->argc < MAX_EXPANDED_ARGS) {
                    out_cmd->argv[out_cmd->argc++] = s_split_words[w];
                }
                continue;
            }
        }
        expand_glob(s_split_words[w], s_split_flags[w], out_cmd);
    }

    out_cmd->argv[out_cmd->argc] = 0;
    return 0;
}
