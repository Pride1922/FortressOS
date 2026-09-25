#include "complete.h"
#include "builtins.h"
#include "io.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "terminal.h"

#define MAX_CANDIDATES 256
#define CAND_POOL_SIZE (64 * 1024)

static char cand_pool[CAND_POOL_SIZE];
static const char *candidates[MAX_CANDIDATES];
static size_t cand_count = 0;
static size_t cand_pool_used = 0;

static inline bool str_equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline bool str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return false;
        str++; prefix++;
    }
    return true;
}

static inline size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void cand_clear(void) {
    cand_count = 0;
    cand_pool_used = 0;
}

static void cand_add(const char *str) {
    if (cand_count >= MAX_CANDIDATES) return;
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
            /* Lexicographical comparison */
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
static char comp_cmd_name[64];
static char comp_dent_buf[VFS_MAX_PATH + 2];
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

int shell_do_completion(line_editor_t *e) {
    if (!e || e->blocked || e->search || e->paste) return 0;

    size_t cur = e->cursor;
    size_t start = cur;
    while (start > 0) {
        char c = e->text[start - 1];
        if (c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' || c == '!') break;
        start--;
    }

    size_t prefix_len = cur - start;
    if (prefix_len >= sizeof(comp_prefix)) prefix_len = sizeof(comp_prefix) - 1;
    for (size_t i = 0; i < prefix_len; i++) comp_prefix[i] = e->text[start + i];
    comp_prefix[prefix_len] = '\0';

    /* Determine if cursor is at the first token (command name) */
    size_t cmd_scan = start;
    while (cmd_scan > 0 && (e->text[cmd_scan - 1] == ' ' || e->text[cmd_scan - 1] == '\t')) {
        cmd_scan--;
    }
    bool is_cmd_name = (cmd_scan == 0 || e->text[cmd_scan - 1] == ';' || e->text[cmd_scan - 1] == '&' ||
                        e->text[cmd_scan - 1] == '|' || e->text[cmd_scan - 1] == '!');

    /* Find the active command name if completing arguments */
    comp_cmd_name[0] = '\0';
    if (!is_cmd_name) {
        size_t cs = cmd_scan;
        while (cs > 0 && e->text[cs - 1] != ';' && e->text[cs - 1] != '&' &&
               e->text[cs - 1] != '|' && e->text[cs - 1] != '!') {
            cs--;
        }
        while (cs < start && (e->text[cs] == ' ' || e->text[cs] == '\t')) cs++;
        size_t ce = cs;
        while (ce < start && e->text[ce] != ' ' && e->text[ce] != '\t') ce++;
        size_t cn_len = ce - cs;
        if (cn_len >= sizeof(comp_cmd_name)) cn_len = sizeof(comp_cmd_name) - 1;
        for (size_t i = 0; i < cn_len; i++) comp_cmd_name[i] = e->text[cs + i];
        comp_cmd_name[cn_len] = '\0';
    }

    cand_clear();

    if (is_cmd_name) {
        /* Complete builtins */
        for (size_t i = 0; i < builtin_count(); i++) {
            const char *name = builtin_name(i);
            if (str_starts_with(name, comp_prefix)) {
                cand_add(name);
            }
        }
        /* Complete /bin/ executables */
        long fd = call(SYS_OPEN, (uintptr_t)"/bin", 0, 0);
        if (fd >= 0) {
            while (call(SYS_READDIR, fd, (uintptr_t)&comp_dent, 0) == 1) {
                if (comp_dent.name[0] != '.' && str_starts_with(comp_dent.name, comp_prefix)) {
                    cand_add(comp_dent.name);
                }
            }
            (void)call(SYS_CLOSE, fd, 0, 0);
        }
    } else {
        bool only_dirs = str_equal(comp_cmd_name, "cd");

        if (str_equal(comp_cmd_name, "layout")) {
            if (str_starts_with("us", comp_prefix)) cand_add("us");
            if (str_starts_with("azerty", comp_prefix)) cand_add("azerty");
        } else if (str_equal(comp_cmd_name, "help") || str_equal(comp_cmd_name, "type") || str_equal(comp_cmd_name, "command")) {
            for (size_t i = 0; i < builtin_count(); i++) {
                const char *name = builtin_name(i);
                if (str_starts_with(name, comp_prefix)) cand_add(name);
            }
            long fd = call(SYS_OPEN, (uintptr_t)"/bin", 0, 0);
            if (fd >= 0) {
                while (call(SYS_READDIR, fd, (uintptr_t)&comp_dent, 0) == 1) {
                    if (comp_dent.name[0] != '.' && str_starts_with(comp_dent.name, comp_prefix)) {
                        cand_add(comp_dent.name);
                    }
                }
                (void)call(SYS_CLOSE, fd, 0, 0);
            }
        } else {
            /* General file / directory path completion */
            /* Split prefix into dir_part and file_prefix */
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
