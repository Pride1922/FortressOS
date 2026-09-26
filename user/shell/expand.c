#include "expand.h"
#include "vars.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "io.h"

#define QUOTE_EXPANDED_UNQUOTED 4

typedef struct {
    char *expand_pool;
    size_t expand_pool_cap;
    size_t expand_pool_used;

    char **split_words;
    uint8_t **split_flags;
    bool *split_has_quotes;
    int max_split_words;
    int split_count;

    char (*glob_names)[VFS_MAX_NAME];
    int max_glob_names;
    int glob_count;

    char *exp_chars;
    uint8_t *exp_flags;
    size_t exp_cap;
    bool overflow;
} expand_ctx_t;

static char s_expand_pool[LINE_CAP * 4];
static char *s_split_words[MAX_EXPANDED_ARGS];
static uint8_t *s_split_flags[MAX_EXPANDED_ARGS];
static bool s_split_has_quotes[MAX_EXPANDED_ARGS];
static char s_glob_names[64][VFS_MAX_NAME];
static char s_exp_chars[LINE_CAP * 2];
static uint8_t s_exp_flags[LINE_CAP * 2];

static expand_ctx_t s_cmd_ctx = {
    .expand_pool = s_expand_pool,
    .expand_pool_cap = sizeof(s_expand_pool),
    .expand_pool_used = 0,
    .split_words = s_split_words,
    .split_flags = s_split_flags,
    .split_has_quotes = s_split_has_quotes,
    .max_split_words = MAX_EXPANDED_ARGS,
    .split_count = 0,
    .glob_names = s_glob_names,
    .max_glob_names = 64,
    .glob_count = 0,
    .exp_chars = s_exp_chars,
    .exp_flags = s_exp_flags,
    .exp_cap = sizeof(s_exp_chars),
};

static char s_redir_expand_pool[VFS_MAX_PATH * 2];
static char *s_redir_split_words[4];
static uint8_t *s_redir_split_flags[4];
static bool s_redir_split_has_quotes[4];
static char s_redir_glob_names[4][VFS_MAX_NAME];
static char s_redir_exp_chars[VFS_MAX_PATH * 2];
static uint8_t s_redir_exp_flags[VFS_MAX_PATH * 2];

static expand_ctx_t s_redir_ctx = {
    .expand_pool = s_redir_expand_pool,
    .expand_pool_cap = sizeof(s_redir_expand_pool),
    .expand_pool_used = 0,
    .split_words = s_redir_split_words,
    .split_flags = s_redir_split_flags,
    .split_has_quotes = s_redir_split_has_quotes,
    .max_split_words = 4,
    .split_count = 0,
    .glob_names = s_redir_glob_names,
    .max_glob_names = 4,
    .glob_count = 0,
    .exp_chars = s_redir_exp_chars,
    .exp_flags = s_redir_exp_flags,
    .exp_cap = sizeof(s_redir_exp_chars),
};

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

static void sort_glob_names(expand_ctx_t *ctx, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (str_cmp(ctx->glob_names[j], ctx->glob_names[j + 1]) > 0) {
                char tmp[VFS_MAX_NAME];
                str_copy(tmp, ctx->glob_names[j], sizeof(tmp));
                str_copy(ctx->glob_names[j], ctx->glob_names[j + 1], sizeof(tmp));
                str_copy(ctx->glob_names[j + 1], tmp, sizeof(tmp));
            }
        }
    }
}

static int expand_glob(expand_ctx_t *ctx, const char *word, const uint8_t *qflags, expanded_cmd_t *out_cmd) {
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
        } else ctx->overflow = true;
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
        if (last_slash >= sizeof(dir_part)) ctx->overflow = true;
        size_t n = last_slash < sizeof(dir_part) - 1 ? last_slash : sizeof(dir_part) - 1;
        for (size_t k = 0; k < n; k++) dir_part[k] = word[k];
        dir_part[n] = '\0';
        file_pat = &word[last_slash + 1];
    }

    /* 3. Open directory and collect matches */
    ctx->glob_count = 0;
    long fd = call(SYS_OPEN, (uintptr_t)dir_part, 0, 0);
    if (fd >= 0) {
        vfs_dirent_t entry;
        long r;
        while ((r = call(SYS_READDIR, fd, (uintptr_t)&entry, 0)) == 1) {
            /* Skip hidden files unless pattern explicitly starts with '.' */
            if (entry.name[0] == '.' && file_pat[0] != '.') continue;

            if (glob_match(file_pat, entry.name)) {
                if (ctx->glob_count < ctx->max_glob_names) {
                    str_copy(ctx->glob_names[ctx->glob_count++], entry.name, VFS_MAX_NAME);
                } else ctx->overflow = true;
            }
        }
        (void)call(SYS_CLOSE, fd, 0, 0);
    }

    /* 4. If no matches found: leave pattern literal */
    if (ctx->glob_count == 0) {
        if (out_cmd->argc < MAX_EXPANDED_ARGS) {
            out_cmd->argv[out_cmd->argc++] = (char *)word;
        } else ctx->overflow = true;
        return 0;
    }

    /* 5. Sort matches alphabetically */
    sort_glob_names(ctx, ctx->glob_count);

    /* 6. Add matching paths to out_cmd */
    for (int i = 0; i < ctx->glob_count; i++) {
        if (out_cmd->argc >= MAX_EXPANDED_ARGS) { ctx->overflow = true; break; }

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
        size_t nlen = str_len(ctx->glob_names[i]);
        if (p + nlen >= sizeof(path_buf)) ctx->overflow = true;
        for (size_t k = 0; k < nlen && p + 1 < sizeof(path_buf); k++) {
            path_buf[p++] = ctx->glob_names[i][k];
        }
        path_buf[p] = '\0';

        size_t needed = p + 1;
        if (ctx->expand_pool_used + needed <= ctx->expand_pool_cap) {
            char *dest = &ctx->expand_pool[ctx->expand_pool_used];
            str_copy(dest, path_buf, needed);
            ctx->expand_pool_used += needed;
            out_cmd->argv[out_cmd->argc++] = dest;
        } else ctx->overflow = true;
    }

    return 0;
}

static int expand_command_ctx(expand_ctx_t *ctx, const parse_cmd_t *in_cmd, int64_t last_status, expanded_cmd_t *out_cmd) {
    if (!ctx || !in_cmd || !out_cmd) return -1;

    out_cmd->argc = 0;
    ctx->expand_pool_used = 0;
    ctx->split_count = 0;
    ctx->overflow = false;

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
            while (*home && exp_len + 1 < ctx->exp_cap) {
                ctx->exp_flags[exp_len] = QUOTE_NONE;
                ctx->exp_chars[exp_len++] = *home++;
            }
            if (*home) ctx->overflow = true;
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
                    if (exp_len + (size_t)n >= ctx->exp_cap) ctx->overflow = true;
                    for (int k = 0; k < n && exp_len + 1 < ctx->exp_cap; k++) {
                        ctx->exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        ctx->exp_chars[exp_len++] = num_buf[k];
                    }
                    continue;
                }

                if (word[i] == '$') {
                    i++;
                    if (exp_len + 1 < ctx->exp_cap) {
                        ctx->exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        ctx->exp_chars[exp_len++] = '1';
                    } else ctx->overflow = true;
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
                    } else ctx->overflow = true;
                    i++;
                }
                var_name[vn_len] = '\0';

                const char *val = vars_get(var_name);
                if (val) {
                    while (*val && exp_len + 1 < ctx->exp_cap) {
                        ctx->exp_flags[exp_len] = (qf == QUOTE_DOUBLE) ? QUOTE_DOUBLE : QUOTE_EXPANDED_UNQUOTED;
                        ctx->exp_chars[exp_len++] = *val++;
                    }
                    if (*val) ctx->overflow = true;
                }
                continue;
            }

            /* Regular character */
            if (exp_len + 1 < ctx->exp_cap) {
                ctx->exp_flags[exp_len] = qf;
                ctx->exp_chars[exp_len++] = word[i];
            } else ctx->overflow = true;
            i++;
        }
        ctx->exp_chars[exp_len] = '\0';

        /* Step 3: Word Splitting on QUOTE_EXPANDED_UNQUOTED whitespace */
        if (exp_len == 0) {
            if (has_quotes) {
                /* Retain empty argument */
                if (ctx->split_count < ctx->max_split_words && ctx->expand_pool_used + 1 <= ctx->expand_pool_cap) {
                    char *dest = &ctx->expand_pool[ctx->expand_pool_used];
                    *dest = '\0';
                    ctx->expand_pool_used++;
                    ctx->split_words[ctx->split_count] = dest;
                    ctx->split_flags[ctx->split_count] = 0;
                    ctx->split_has_quotes[ctx->split_count] = true;
                    ctx->split_count++;
                } else ctx->overflow = true;
            }
            continue;
        }

        size_t cur_start = 0;
        bool in_word = false;

        for (size_t k = 0; k < exp_len; k++) {
            if (ctx->exp_flags[k] == QUOTE_EXPANDED_UNQUOTED && is_whitespace(ctx->exp_chars[k])) {
                if (in_word) {
                    size_t wlen = k - cur_start;
                    if (ctx->split_count < ctx->max_split_words &&
                        ctx->expand_pool_used + wlen + 1 + wlen + 1 <= ctx->expand_pool_cap) {
                        char *dest = &ctx->expand_pool[ctx->expand_pool_used];
                        uint8_t *qdest = (uint8_t *)&ctx->expand_pool[ctx->expand_pool_used + wlen + 1];
                        for (size_t j = 0; j < wlen; j++) {
                            dest[j] = ctx->exp_chars[cur_start + j];
                            qdest[j] = ctx->exp_flags[cur_start + j];
                        }
                        dest[wlen] = '\0';
                        qdest[wlen] = 0;
                        ctx->expand_pool_used += (wlen + 1) * 2;

                        ctx->split_words[ctx->split_count] = dest;
                        ctx->split_flags[ctx->split_count] = qdest;
                        ctx->split_has_quotes[ctx->split_count] = has_quotes;
                        ctx->split_count++;
                    } else ctx->overflow = true;
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
            if (ctx->split_count < ctx->max_split_words &&
                ctx->expand_pool_used + wlen + 1 + wlen + 1 <= ctx->expand_pool_cap) {
                char *dest = &ctx->expand_pool[ctx->expand_pool_used];
                uint8_t *qdest = (uint8_t *)&ctx->expand_pool[ctx->expand_pool_used + wlen + 1];
                for (size_t j = 0; j < wlen; j++) {
                    dest[j] = ctx->exp_chars[cur_start + j];
                    qdest[j] = ctx->exp_flags[cur_start + j];
                }
                dest[wlen] = '\0';
                qdest[wlen] = 0;
                ctx->expand_pool_used += (wlen + 1) * 2;

                ctx->split_words[ctx->split_count] = dest;
                ctx->split_flags[ctx->split_count] = qdest;
                ctx->split_has_quotes[ctx->split_count] = has_quotes;
                ctx->split_count++;
            } else ctx->overflow = true;
        }
    }

    /* Step 4: Pathname Expansion (Globbing) on each split word */
    for (int w = 0; w < ctx->split_count; w++) {
        if (ctx->split_has_quotes[w]) {
            /* If the word was an explicitly quoted empty string e.g. "", preserve it without globbing */
            if (ctx->split_words[w][0] == '\0') {
                if (out_cmd->argc < MAX_EXPANDED_ARGS) {
                    out_cmd->argv[out_cmd->argc++] = ctx->split_words[w];
                } else ctx->overflow = true;
                continue;
            }
        }
        expand_glob(ctx, ctx->split_words[w], ctx->split_flags[w], out_cmd);
    }

    out_cmd->argv[out_cmd->argc] = 0;
    return 0;
}

int expand_command(const parse_cmd_t *in_cmd, int64_t last_status, expanded_cmd_t *out_cmd) {
    return expand_command_ctx(&s_cmd_ctx, in_cmd, last_status, out_cmd);
}

int expand_command_checked(const parse_cmd_t *in_cmd, int64_t last_status,
                           expanded_cmd_t *out_cmd) {
    int result = expand_command(in_cmd, last_status, out_cmd);
    if (result || s_cmd_ctx.overflow) {
        out_cmd->argc = 0;
        out_cmd->argv[0] = NULL;
        puts_err("Expansion exceeds shell limits.\n");
        return -1;
    }
    return 0;
}

static parse_cmd_t s_single_redir_cmd;
static expanded_cmd_t s_single_redir_exp;

int expand_redir_target(const char *target, const uint8_t *quote_flags, bool has_quotes,
                        int64_t last_status, char *out_path, size_t out_cap) {
    if (!target || !out_path || out_cap == 0) return -1;

    char *w_argv[2] = { (char *)target, NULL };
    const uint8_t *w_qflags[2] = { quote_flags, NULL };
    bool w_quotes[2] = { has_quotes, false };

    s_single_redir_cmd.argc = 1;
    s_single_redir_cmd.argv[0] = w_argv[0];
    s_single_redir_cmd.argv[1] = NULL;
    s_single_redir_cmd.quote_flags[0] = w_qflags[0];
    s_single_redir_cmd.quote_flags[1] = NULL;
    s_single_redir_cmd.has_quotes[0] = w_quotes[0];
    s_single_redir_cmd.redir_count = 0;
    s_single_redir_cmd.negate = false;
    s_single_redir_cmd.next_op = CMD_OP_NONE;

    if (expand_command_ctx(&s_redir_ctx, &s_single_redir_cmd, last_status, &s_single_redir_exp) != 0 ||
        s_redir_ctx.overflow || s_single_redir_exp.argc != 1) {
        return -1; /* ambiguous redirect or empty */
    }

    size_t len = str_len(s_single_redir_exp.argv[0]);
    if (len >= out_cap) return -1;
    str_copy(out_path, s_single_redir_exp.argv[0], out_cap);
    return 0;
}
