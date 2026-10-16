#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"

/* Static BSS storage — zero heap, strictly conforms to 512-byte stack budget */
static char s_candidate[VFS_MAX_PATH];
static vfs_stat_t s_st;

static const char *get_path_var(const char *const *envp) {
    if (!envp) return "/bin:/sbin";
    for (int i = 0; envp[i]; i++) {
        const char *e = envp[i];
        if (e[0] == 'P' && e[1] == 'A' && e[2] == 'T' && e[3] == 'H' && e[4] == '=') {
            return e + 5;
        }
    }
    return "/bin:/sbin";
}

int which_main(int argc, char **argv, const char *const *envp) {
    if (argc < 2) {
        return tool_error("which", "missing operand", NULL);
    }
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        return tool_write("which", "Usage: which [-a] filename ...\nLocate a command in PATH.\n", 57);
    }

    bool all_matches = false;
    int arg_start = 1;
    if (tool_equal(argv[1], "-a") || tool_equal(argv[1], "--all")) {
        all_matches = true;
        arg_start = 2;
        if (arg_start >= argc) {
            return tool_error("which", "missing operand after -a", NULL);
        }
    }

    const char *path_var = get_path_var(envp);
    bool overall_success = true;

    for (int i = arg_start; i < argc; i++) {
        const char *name = argv[i];
        if (!name || !*name) continue;

        bool has_slash = false;
        for (size_t k = 0; name[k]; k++) {
            if (name[k] == '/') {
                has_slash = true;
                break;
            }
        }

        bool found = false;
        if (has_slash) {
            if (tool_syscall(SYS_STAT, (uintptr_t)name, (uintptr_t)&s_st, 0) == 0 &&
                s_st.type == VFS_FILE) {
                tool_write("which", name, tool_length(name));
                tool_write("which", "\n", 1);
                found = true;
            }
        } else {
            const char *p = path_var;
            while (*p && (!found || all_matches)) {
                size_t dlen = 0;
                while (p[dlen] && p[dlen] != ':') dlen++;
                size_t nlen = tool_length(name);

                if (dlen + 1 + nlen + 1 < VFS_MAX_PATH) {
                    size_t k = 0;
                    for (; k < dlen; k++) s_candidate[k] = p[k];
                    s_candidate[k++] = '/';
                    for (size_t j = 0; j <= nlen; j++) s_candidate[k + j] = name[j];

                    if (tool_syscall(SYS_STAT, (uintptr_t)s_candidate, (uintptr_t)&s_st, 0) == 0 &&
                        s_st.type == VFS_FILE) {
                        tool_write("which", s_candidate, tool_length(s_candidate));
                        tool_write("which", "\n", 1);
                        found = true;
                        if (!all_matches) break;
                    }
                }

                p += dlen;
                if (*p == ':') p++;
            }
        }

        if (!found) {
            tool_write("which", "which: no ", 10);
            tool_write("which", name, tool_length(name));
            tool_write("which", " in (", 5);
            tool_write("which", path_var, tool_length(path_var));
            tool_write("which", ")\n", 2);
            overall_success = false;
        }
    }

    return overall_success ? 0 : 1;
}
