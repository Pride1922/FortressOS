#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"

/* Static BSS storage — strictly conforms to 512-byte stack budget */
static const char *s_env_ptrs[MAX_SPAWN_ENVP + 1];
static size_t s_env_count = 0;
static char s_candidate[VFS_MAX_PATH];
static vfs_stat_t s_st;

static bool key_matches(const char *entry, const char *key_val) {
    if (!entry || !key_val) return false;
    size_t i = 0;
    while (entry[i] && entry[i] != '=' && key_val[i] && key_val[i] != '=') {
        if (entry[i] != key_val[i]) return false;
        i++;
    }
    return (entry[i] == '=' && key_val[i] == '=');
}

static const char *get_path_var(void) {
    for (size_t i = 0; i < s_env_count; i++) {
        const char *e = s_env_ptrs[i];
        if (e && e[0] == 'P' && e[1] == 'A' && e[2] == 'T' && e[3] == 'H' && e[4] == '=') {
            return e + 5;
        }
    }
    return "/bin:/sbin";
}

int env_main(int argc, char **argv, const char *const *envp) {
    if (argc >= 2 && tool_equal(argv[1], "--help")) {
        return tool_write("env",
            "Usage: env [-i] [NAME=VALUE...] [COMMAND [ARG...]]\n"
            "Set environment variables and run command, or print environment.\n"
            "Options:\n"
            "  -i, --ignore-environment   start with an empty environment\n"
            "  --help                     display this help and exit\n", 226);
    }

    s_env_count = 0;
    int arg_idx = 1;
    bool ignore_env = false;

    if (arg_idx < argc && (tool_equal(argv[arg_idx], "-i") || tool_equal(argv[arg_idx], "--ignore-environment"))) {
        ignore_env = true;
        arg_idx++;
    }

    /* Populate initial environment if not -i */
    if (!ignore_env && envp) {
        for (int i = 0; envp[i] && s_env_count < MAX_SPAWN_ENVP; i++) {
            s_env_ptrs[s_env_count++] = envp[i];
        }
    }

    /* Process KEY=VALUE assignments */
    while (arg_idx < argc) {
        const char *arg = argv[arg_idx];
        if (!arg || !*arg) break;

        /* Check for '=' and non-empty key */
        bool has_eq = false;
        for (size_t k = 1; arg[k]; k++) {
            if (arg[k] == '=') {
                has_eq = true;
                break;
            }
        }
        if (!has_eq) break;

        /* Update existing or append new variable */
        bool updated = false;
        for (size_t j = 0; j < s_env_count; j++) {
            if (key_matches(s_env_ptrs[j], arg)) {
                s_env_ptrs[j] = arg;
                updated = true;
                break;
            }
        }
        if (!updated && s_env_count < MAX_SPAWN_ENVP) {
            s_env_ptrs[s_env_count++] = arg;
        }

        arg_idx++;
    }
    s_env_ptrs[s_env_count] = NULL;

    /* If no COMMAND specified, dump environment */
    if (arg_idx >= argc) {
        for (size_t i = 0; i < s_env_count; i++) {
            if (s_env_ptrs[i]) {
                tool_write("env", s_env_ptrs[i], tool_length(s_env_ptrs[i]));
                tool_write("env", "\n", 1);
            }
        }
        return 0;
    }

    /* Resolve and spawn COMMAND */
    const char *cmd = argv[arg_idx];
    const char *target_cmd = cmd;

    bool has_slash = false;
    for (size_t k = 0; cmd[k]; k++) {
        if (cmd[k] == '/') {
            has_slash = true;
            break;
        }
    }

    if (!has_slash) {
        const char *path_var = get_path_var();
        const char *p = path_var;
        bool found = false;

        while (*p && !found) {
            size_t dlen = 0;
            while (p[dlen] && p[dlen] != ':') dlen++;
            size_t nlen = tool_length(cmd);

            if (dlen + 1 + nlen + 1 < VFS_MAX_PATH) {
                size_t k = 0;
                for (; k < dlen; k++) s_candidate[k] = p[k];
                s_candidate[k++] = '/';
                for (size_t j = 0; j <= nlen; j++) s_candidate[k + j] = cmd[j];

                if (tool_syscall(SYS_STAT, (uintptr_t)s_candidate, (uintptr_t)&s_st, 0) == 0 &&
                    s_st.type == VFS_FILE) {
                    target_cmd = s_candidate;
                    found = true;
                    break;
                }
            }

            p += dlen;
            if (*p == ':') p++;
        }

        if (!found) {
            tool_error("env", "command not found", cmd);
            return 127;
        }
    }

    spawn_opts_t opts = {
        .size = sizeof(opts),
        .version = 1,
        .flags = 0,
        .reserved0 = 0,
        .argv = (uintptr_t)&argv[arg_idx],
        .envp = (uintptr_t)s_env_ptrs,
        .cwd = 0,
        .fd_actions = 0,
        .action_count = 0,
        .reserved1 = 0,
        .reserved2 = 0
    };

    long pid = tool_syscall(SYS_SPAWN_EXT, (uintptr_t)target_cmd, (uintptr_t)&opts, sizeof(opts));
    if (pid < 0) {
        return tool_sys_error("env", "cannot execute command", cmd, pid);
    }

    uint64_t status = 0;
    return tool_syscall(SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0) < 0 ? 1 : (int)status;
}
