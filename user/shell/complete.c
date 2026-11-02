#include "complete.h"
#include "builtins.h"
#include "alias.h"
#include "vars.h"
#include "io.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "terminal.h"

#if defined(__GNUC__)
__attribute__((weak)) const char *vars_get(const char *name);
__attribute__((weak)) size_t alias_count(void);
__attribute__((weak)) const char *alias_name(size_t index);
#endif

#define MAX_CANDIDATES 256
#define CAND_POOL_SIZE (64 * 1024)
#define MAX_COMP_WORDS 16

static char cand_pool[CAND_POOL_SIZE];
static const char *candidates[MAX_CANDIDATES];
static size_t cand_count = 0;
static size_t cand_pool_used = 0;

static inline bool str_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline bool str_starts_with(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (*str != *prefix) return false;
        str++; prefix++;
    }
    return true;
}

static inline size_t str_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void cand_clear(void) {
    cand_count = 0;
    cand_pool_used = 0;
}

static void cand_add(const char *str) {
    if (!str || cand_count >= MAX_CANDIDATES) return;
    for (size_t i = 0; i < cand_count; i++) {
        if (str_equal(candidates[i], str)) return;
    }
    size_t n = str_len(str) + 1;
    if (cand_pool_used + n > CAND_POOL_SIZE) return;
    char *dest = cand_pool + cand_pool_used;
    for (size_t i = 0; i < n; i++) dest[i] = str[i];
    cand_pool_used += n;
    candidates[cand_count++] = dest;
}

static void cand_sort(void) {
    for (size_t i = 0; i < cand_count; i++) {
        for (size_t j = i + 1; j < cand_count; j++) {
            const char *a = candidates[i];
            const char *b = candidates[j];
            while (*a && *a == *b) { a++; b++; }
            if ((unsigned char)*a > (unsigned char)*b) {
                const char *tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }
}

static size_t common_prefix_len(void) {
    if (cand_count == 0) return 0;
    size_t len = 0;
    for (;;) {
        char c = candidates[0][len];
        if (!c) return len;
        for (size_t i = 1; i < cand_count; i++) {
            if (candidates[i][len] != c) return len;
        }
        len++;
    }
}

static char comp_prefix[VFS_MAX_PATH];
static char comp_dir_open[VFS_MAX_PATH];
static char comp_dir_prefix[VFS_MAX_PATH];
static char comp_file_prefix[VFS_MAX_PATH];
static char comp_dent_buf[VFS_MAX_PATH + 2];
static char comp_words[MAX_COMP_WORDS][64];
static vfs_dirent_t comp_dent;

static void collect_dir_entries(const char *dir_path, const char *dir_prefix, const char *file_prefix, bool only_dirs) {
    long fd = call(SYS_OPEN, (uintptr_t)dir_path, 0, 0);
    if (fd < 0) return;

    while (call(SYS_READDIR, fd, (uintptr_t)&comp_dent, 0) == 1) {
        if (comp_dent.name[0] == '.' && file_prefix[0] != '.') continue;
        if (str_starts_with(comp_dent.name, file_prefix)) {
            bool is_dir = (comp_dent.type == 2 /* VFS_DIRECTORY */);
            if (only_dirs && !is_dir) continue;

            size_t dp_len = str_len(dir_prefix);
            size_t nm_len = str_len(comp_dent.name);
            if (dp_len + nm_len + 2 < sizeof(comp_dent_buf)) {
                for (size_t i = 0; i < dp_len; i++) comp_dent_buf[i] = dir_prefix[i];
                for (size_t i = 0; i < nm_len; i++) comp_dent_buf[dp_len + i] = comp_dent.name[i];
                size_t total = dp_len + nm_len;
                if (is_dir) {
                    comp_dent_buf[total++] = '/';
                }
                comp_dent_buf[total] = '\0';
                cand_add(comp_dent_buf);
            }
        }
    }
    (void)call(SYS_CLOSE, fd, 0, 0);
}

static void collect_command_candidates(const char *prefix, bool allow_builtins) {
    /* Builtins */
    if (allow_builtins) {
        for (size_t i = 0; i < builtin_count(); i++) {
            const char *name = builtin_name(i);
            if (str_starts_with(name, prefix)) cand_add(name);
        }
        if (alias_count && alias_name) {
            size_t ac = alias_count();
            for (size_t i = 0; i < ac; i++) {
                const char *an = alias_name(i);
                if (an && str_starts_with(an, prefix)) cand_add(an);
            }
        }
    }

    /* Scan directories in $PATH */
    const char *path = (vars_get != NULL) ? vars_get("PATH") : NULL;
    if (!path || !*path) path = "/bin:/sbin:/usr/bin";

    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != ':') n++;
        if (n > 0 && n < sizeof(comp_dir_open)) {
            for (size_t i = 0; i < n; i++) comp_dir_open[i] = path[i];
            comp_dir_open[n] = '\0';

            long fd = call(SYS_OPEN, (uintptr_t)comp_dir_open, 0, 0);
            if (fd >= 0) {
                while (call(SYS_READDIR, fd, (uintptr_t)&comp_dent, 0) == 1) {
                    if (comp_dent.name[0] != '.' && str_starts_with(comp_dent.name, prefix)) {
                        cand_add(comp_dent.name);
                    }
                }
                (void)call(SYS_CLOSE, fd, 0, 0);
            }
        }
        path += n;
        if (*path == ':') path++;
    }
}

int shell_do_completion(line_editor_t *e) {
    if (!e || e->blocked || e->search || e->paste) return 0;

    size_t cur = e->cursor;
    size_t start = cur;
    while (start > 0) {
        char c = e->text[start - 1];
        if (c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' ||
            c == '!' || c == '(' || c == ')' || c == '`') break;
        start--;
    }

    size_t prefix_len = cur - start;
    if (prefix_len >= sizeof(comp_prefix)) prefix_len = sizeof(comp_prefix) - 1;
    for (size_t i = 0; i < prefix_len; i++) comp_prefix[i] = e->text[start + i];
    comp_prefix[prefix_len] = '\0';

    /* Find segment start (after sequential, logical, pipeline or subshell separator) */
    size_t seg_start = start;
    while (seg_start > 0) {
        char c = e->text[seg_start - 1];
        if (c == ';' || c == '&' || c == '|' || c == '!' || c == '(' || c == '`') break;
        seg_start--;
    }

    /* Extract words prior to start in this segment */
    size_t word_count = 0;
    size_t scan = seg_start;
    while (scan < start && word_count < MAX_COMP_WORDS) {
        while (scan < start && (e->text[scan] == ' ' || e->text[scan] == '\t')) scan++;
        if (scan >= start) break;
        size_t w_start = scan;
        while (scan < start && e->text[scan] != ' ' && e->text[scan] != '\t') scan++;
        size_t w_len = scan - w_start;
        if (w_len >= sizeof(comp_words[word_count])) w_len = sizeof(comp_words[word_count]) - 1;
        for (size_t i = 0; i < w_len; i++) comp_words[word_count][i] = e->text[w_start + i];
        comp_words[word_count][w_len] = '\0';
        word_count++;
    }

    /* Skip leading environment variable assignments (e.g. NAME=VAL) */
    size_t cmd_idx = 0;
    while (cmd_idx < word_count) {
        const char *w = comp_words[cmd_idx];
        bool is_assign = false;
        if ((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= 'A' && w[0] <= 'Z') || w[0] == '_') {
            size_t j = 1;
            while ((w[j] >= 'a' && w[j] <= 'z') || (w[j] >= 'A' && w[j] <= 'Z') ||
                   (w[j] >= '0' && w[j] <= '9') || w[j] == '_') j++;
            if (w[j] == '=') is_assign = true;
        }
        if (is_assign) cmd_idx++;
        else break;
    }

    bool is_cmd_name = false;
    bool sudo_command = false;

    if (cmd_idx == word_count) {
        is_cmd_name = true;
    } else {
        const char *first_cmd = comp_words[cmd_idx];
        /* Check for sudo wrapper */
        if (str_equal(first_cmd, "sudo") || str_equal(first_cmd, "/bin/sudo") ||
            str_equal(first_cmd, "/sbin/sudo")) {
            cmd_idx++;
            while (cmd_idx < word_count) {
                if (comp_words[cmd_idx][0] == '-') {
                    if (str_equal(comp_words[cmd_idx], "-u")) {
                        cmd_idx += 2;
                    } else {
                        cmd_idx++;
                    }
                } else {
                    break;
                }
            }
            if (cmd_idx == word_count) {
                is_cmd_name = true;
                sudo_command = true;
            }
        } else if (str_equal(first_cmd, "time") || str_equal(first_cmd, "/bin/time")) {
            cmd_idx++;
            while (cmd_idx < word_count && comp_words[cmd_idx][0] == '-') cmd_idx++;
            if (cmd_idx == word_count) {
                is_cmd_name = true;
            }
        } else if (str_equal(first_cmd, "which") || str_equal(first_cmd, "/bin/which")) {
            if (comp_prefix[0] != '-') {
                is_cmd_name = true;
                sudo_command = true; /* which locates binaries, not shell builtins */
            }
        } else if (str_equal(first_cmd, "help") || str_equal(first_cmd, "type") ||
                   str_equal(first_cmd, "command")) {
            is_cmd_name = true;
        }
    }

    cand_clear();

    bool has_slash = false;
    for (size_t i = 0; i < prefix_len; i++) {
        if (comp_prefix[i] == '/') has_slash = true;
    }

    if (is_cmd_name && !has_slash) {
        collect_command_candidates(comp_prefix, !sudo_command);
    } else {
        bool only_dirs = false;

        /* Extract active bare command name if any */
        const char *cname = (cmd_idx < word_count) ? comp_words[cmd_idx] : "";
        size_t last_s = 0;
        for (size_t i = 0; cname[i]; i++) {
            if (cname[i] == '/') last_s = i + 1;
        }
        cname += last_s;

        size_t arg_idx = (word_count > cmd_idx) ? (word_count - cmd_idx) : 0;
        const char *prev_arg = (word_count > cmd_idx + 1) ? comp_words[word_count - 1] : "";

        if (str_equal(cname, "cd")) {
            only_dirs = true;
        } else if (!has_slash && str_equal(cname, "layout") && arg_idx == 1) {
            static const char *const layout_cmds[] = { "us", "azerty", NULL };
            for (size_t i = 0; layout_cmds[i]; i++) {
                if (str_starts_with(layout_cmds[i], comp_prefix)) cand_add(layout_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "service")) {
            if (arg_idx == 1) {
                static const char *const service_cmds[] = {
                    "list", "status", "check", "start", "stop", "restart", "reload", "enable", "disable", NULL
                };
                for (size_t i = 0; service_cmds[i]; i++) {
                    if (str_starts_with(service_cmds[i], comp_prefix)) cand_add(service_cmds[i]);
                }
            } else if (arg_idx == 2 &&
                       (str_equal(prev_arg, "start") || str_equal(prev_arg, "stop") ||
                        str_equal(prev_arg, "restart") || str_equal(prev_arg, "status") ||
                        str_equal(prev_arg, "check") || str_equal(prev_arg, "reload") ||
                        str_equal(prev_arg, "enable") || str_equal(prev_arg, "disable"))) {
                /* Complete configured service names from /etc/watchtower */
                long s_fd = call(SYS_OPEN, (uintptr_t)"/etc/watchtower", 0, 0);
                if (s_fd >= 0) {
                    while (call(SYS_READDIR, s_fd, (uintptr_t)&comp_dent, 0) == 1) {
                        if (comp_dent.name[0] == '.') continue;
                        size_t nlen = str_len(comp_dent.name);
                        if (nlen > 8 && str_equal(comp_dent.name + nlen - 8, ".service")) {
                            comp_dent.name[nlen - 8] = '\0';
                            if (str_starts_with(comp_dent.name, comp_prefix)) {
                                cand_add(comp_dent.name);
                            }
                        }
                    }
                    (void)call(SYS_CLOSE, s_fd, 0, 0);
                }
            }
        } else if (!has_slash && str_equal(cname, "wifi") && arg_idx == 1) {
            static const char *const wifi_cmds[] = {
                "scan", "list", "status", "connect", "static", "save", "forget", "auto", "disconnect", NULL
            };
            for (size_t i = 0; wifi_cmds[i]; i++) {
                if (str_starts_with(wifi_cmds[i], comp_prefix)) cand_add(wifi_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "pkg") && arg_idx == 1) {
            static const char *const pkg_cmds[] = {
                "install", "upgrade", "remove", "installed", "info", "status", "verify",
                "inspect", "update", "fetch", "get", "base-status", "confirm-base", "recover-base", NULL
            };
            for (size_t i = 0; pkg_cmds[i]; i++) {
                if (str_starts_with(pkg_cmds[i], comp_prefix)) cand_add(pkg_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "history") && arg_idx == 1) {
            static const char *const history_cmds[] = { "clear", "save", "load", NULL };
            for (size_t i = 0; history_cmds[i]; i++) {
                if (str_starts_with(history_cmds[i], comp_prefix)) cand_add(history_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "terminal") && arg_idx == 1) {
            static const char *const term_cmds[] = { "local", "serial", "mirror", "plain", NULL };
            for (size_t i = 0; term_cmds[i]; i++) {
                if (str_starts_with(term_cmds[i], comp_prefix)) cand_add(term_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "prompt") && arg_idx == 1) {
            static const char *const prompt_cmds[] = { "default", "cwd", NULL };
            for (size_t i = 0; prompt_cmds[i]; i++) {
                if (str_starts_with(prompt_cmds[i], comp_prefix)) cand_add(prompt_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "dmesg") && arg_idx == 1) {
            static const char *const dmesg_cmds[] = { "tail", "-n", NULL };
            for (size_t i = 0; dmesg_cmds[i]; i++) {
                if (str_starts_with(dmesg_cmds[i], comp_prefix)) cand_add(dmesg_cmds[i]);
            }
        } else if (!has_slash && str_equal(cname, "kill") && arg_idx == 1) {
            static const char *const kill_cmds[] = {
                "-9", "-15", "-TERM", "-KILL", "-INT", "-HUP", "-STOP", "-CONT", "-QUIT",
                "%1", "%2", "%+", "%-", NULL
            };
            for (size_t i = 0; kill_cmds[i]; i++) {
                if (str_starts_with(kill_cmds[i], comp_prefix)) cand_add(kill_cmds[i]);
            }
        } else if (!has_slash && (str_equal(cname, "jobs") || str_equal(cname, "fg") ||
                                  str_equal(cname, "bg")) && arg_idx == 1) {
            static const char *const job_cmds[] = { "%1", "%2", "%+", "%-", NULL };
            for (size_t i = 0; job_cmds[i]; i++) {
                if (str_starts_with(job_cmds[i], comp_prefix)) cand_add(job_cmds[i]);
            }
        }

        /* If no subcommand candidates matched, default to file/directory path completion */
        if (cand_count == 0) {
            int last_slash = -1;
            for (int i = (int)prefix_len - 1; i >= 0; i--) {
                if (comp_prefix[i] == '/') { last_slash = i; break; }
            }

            if (last_slash >= 0) {
                if (last_slash == 0) {
                    comp_dir_open[0] = '/'; comp_dir_open[1] = '\0';
                    comp_dir_prefix[0] = '/'; comp_dir_prefix[1] = '\0';
                } else {
                    for (int i = 0; i < last_slash; i++) comp_dir_open[i] = comp_prefix[i];
                    comp_dir_open[last_slash] = '\0';
                    for (int i = 0; i <= last_slash; i++) comp_dir_prefix[i] = comp_prefix[i];
                    comp_dir_prefix[last_slash + 1] = '\0';
                }
                size_t f_idx = 0;
                for (size_t i = (size_t)last_slash + 1; i < prefix_len; i++) {
                    comp_file_prefix[f_idx++] = comp_prefix[i];
                }
                comp_file_prefix[f_idx] = '\0';
            } else {
                comp_dir_open[0] = '.'; comp_dir_open[1] = '\0';
                comp_dir_prefix[0] = '\0';
                for (size_t i = 0; i < prefix_len; i++) comp_file_prefix[i] = comp_prefix[i];
                comp_file_prefix[prefix_len] = '\0';
            }

            collect_dir_entries(comp_dir_open, comp_dir_prefix, comp_file_prefix, only_dirs);
        }
    }

    if (cand_count == 0) {
        return 0; /* no match */
    }

    cand_sort();

    if (cand_count == 1) {
        /* Single unique match */
        const char *match = candidates[0];
        size_t mlen = str_len(match);
        bool is_dir = (mlen > 0 && match[mlen - 1] == '/');

        /* Shift remainder of text */
        size_t rem = e->len - cur;
        size_t extra = is_dir ? 0 : 1;
        if (start + mlen + extra + rem >= LINE_CAP) return 0;

        for (size_t i = rem; i > 0; i--) {
            e->text[start + mlen + extra + i - 1] = e->text[cur + i - 1];
        }
        for (size_t i = 0; i < mlen; i++) {
            e->text[start + i] = match[i];
        }
        if (!is_dir) {
            e->text[start + mlen] = ' ';
        }
        e->cursor = start + mlen + extra;
        e->len = start + mlen + extra + rem;
        e->text[e->len] = '\0';
        return 1;
    }

    /* Multiple matches */
    size_t c_len = common_prefix_len();
    if (c_len > prefix_len) {
        /* Extend text to common prefix */
        size_t rem = e->len - cur;
        if (start + c_len + rem >= LINE_CAP) return 0;

        for (size_t i = rem; i > 0; i--) {
            e->text[start + c_len + i - 1] = e->text[cur + i - 1];
        }
        for (size_t i = 0; i < c_len; i++) {
            e->text[start + i] = candidates[0][i];
        }
        e->cursor = start + c_len;
        e->len = start + c_len + rem;
        e->text[e->len] = '\0';
        return 1;
    }

    /* Common prefix cannot be extended: display candidate list */
    puts("\n");
    for (size_t i = 0; i < cand_count; i++) {
        puts(candidates[i]);
        puts("  ");
        if ((i + 1) % 4 == 0) puts("\n");
    }
    if (cand_count % 4 != 0) puts("\n");
    return 2; /* Needs full redraw */
}
