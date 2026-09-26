/* A standalone Ring 3 program. Only the public syscall ABI crosses into kernel. */
#include "types.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "shell/io.h"
#include "shell/ui.h"
#include "shell/builtins.h"
#include "shell/lexer.h"
#include "shell/parser.h"
#include "shell/history_persist.h"
#include "shell/fileedit.h"
#include "shell/vars.h"
#include "shell/alias.h"
#include "shell/expand.h"
#include "shell/redir.h"

static char cmd_buf[LINE_CAP * 2];
static char line_input[LINE_CAP];
static parse_tree_t parse_tree;
static int64_t last_status = 0;
static char dmesg_buf[DMESG_SIZE];
static char current_cwd[VFS_MAX_PATH] = "/";
static char oldpwd[VFS_MAX_PATH] = "";
static local_var_scope_t s_local_scope;
static expanded_cmd_t s_expanded_cmd;
static char s_env_strings[32][MAX_VAR_NAME + MAX_VAR_VAL + 2];
static const char *s_envp_ptrs[33];
static char s_alias_line[LINE_CAP * 2];
static parse_cmd_t s_val_cmd;
static parse_cmd_t s_sub_cmd;
static expanded_cmd_t s_exp_val;
static spawn_fd_action_t s_spawn_actions[MAX_SPAWN_ACTIONS];
static char s_spawn_target_paths[MAX_SPAWN_ACTIONS][VFS_MAX_PATH];
static redir_scope_t s_parent_scope;

static void update_cwd(void) {
    long n = call(SYS_GETCWD, (uintptr_t)current_cwd, sizeof(current_cwd), 0);
    if (n <= 0) {
        current_cwd[0] = '/';
        current_cwd[1] = '\0';
    }
}

static int parse_int(const char *s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return sign * val;
}

static void list(const char *path) {
    if (!path || !*path) path = ".";
    vfs_stat_t st;
    long result = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); last_status = 1; return; }
    if (st.type != VFS_DIRECTORY) { puts(path); puts("\n"); last_status = 0; return; }
    long fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); last_status = 1; return; }
    vfs_dirent_t entry;
    while ((result = call(SYS_READDIR, fd, (uintptr_t)&entry, 0)) == 1) {
        puts(entry.name);
        puts(entry.type == VFS_DIRECTORY ? "/\n" : "\n");
    }
    if (result < 0) { file_error(result); last_status = 1; }
    else { last_status = 0; }
    (void)call(SYS_CLOSE, fd, 0, 0);
}

static void cat(const char *path) {
    long fd = 0, result;
    if (path) {
        vfs_stat_t st;
        result = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
        if (result < 0) { file_error_err(result); last_status = 1; return; }
        if (st.type != VFS_FILE) { puts_err("Not a regular file.\n"); last_status = 1; return; }
        fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
        if (fd < 0) { file_error_err(fd); last_status = 1; return; }
    }
    char buf[512];
    bool newline = true;
    while ((result = call(SYS_READ, fd, (uintptr_t)buf, sizeof(buf))) > 0) {
        for (long i = 0; i < result; i++) {
            unsigned char c = buf[i];
            if (c != '\n' && c != '\t' && (c < 32 || c > 126)) buf[i] = '.';
        }
        newline = buf[result - 1] == '\n';
        long written = write_bytes_fd(1, buf, (size_t)result);
        if (written < 0) { result = written; break; }
    }
    if (result >= 0 && !newline) result = write_bytes_fd(1, "\n", 1);
    if (result < 0) { file_error_err(result); last_status = 1; }
    else { last_status = 0; }
    if (path) (void)call(SYS_CLOSE, fd, 0, 0);
}

static int spawn_program(const char *path, const char **argv, const spawn_fd_action_t *actions, uint32_t action_count) {
    (void)vars_build_envp(s_env_strings, s_envp_ptrs);

    spawn_opts_t opts;
    for (size_t i = 0; i < sizeof(opts); i++) ((char *)&opts)[i] = 0;
    opts.size = sizeof(spawn_opts_t);
    opts.version = 1;
    opts.flags = 0;
    opts.argv = (uint64_t)argv;
    opts.envp = (uint64_t)s_envp_ptrs;
    opts.cwd = 0; /* inherit */
    opts.fd_actions = (action_count > 0) ? (uint64_t)actions : 0;
    opts.action_count = action_count;

    long pid = call(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts));
    if (pid < 0) {
        switch (pid) {
            case SYSCALL_ENOENT: puts_err("No such file or directory.\n"); return 127;
            case SYSCALL_ENOEXEC: puts_err("Invalid executable.\n"); return 126;
            case SYSCALL_ENOMEM: puts_err("Out of memory or process capacity.\n"); return 1;
            case SYSCALL_EISDIR: puts_err("Not a regular file.\n"); return 126;
            case SYSCALL_EFBIG: puts_err("Executable exceeds 4 MiB limit.\n"); return 126;
            case SYSCALL_E2BIG: puts_err("Argument list too long.\n"); return 1;
            case SYSCALL_EBADF: puts_err("Bad file descriptor in redirection.\n"); return 1;
            case SYSCALL_EINVAL: puts_err("Invalid redirection or spawn arguments.\n"); return 1;
            case SYSCALL_EROFS: puts_err("Read-only filesystem.\n"); return 1;
            case SYSCALL_EIO: puts_err("I/O error.\n"); return 1;
            default: puts_err("Unable to load executable.\n"); return 1;
        }
    }
    int64_t status;
    if (call(SYS_WAIT, pid, (uintptr_t)&status, 0) < 0) {
        puts("Unable to wait for child process.\n");
        return 1;
    }
    if (status >= 128 && status < 160) {
        puts("[PROCESS] Faulted (exception vector ");
        put_dec((uint64_t)status - 128);
        puts(")\n");
    } else if (status) {
        puts("[PROCESS] Exit status ");
        if (status < 0) { puts("-"); put_dec(0 - (uint64_t)status); }
        else put_dec((uint64_t)status);
        puts("\n");
    }
    return (int)status;
}

static void echo_cmd(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) puts(" ");
        puts(argv[i]);
    }
    puts("\n");
    last_status = 0;
}

static void dmesg_cmd(const char *arg) {
    long n = call(SYS_DMESG, (uintptr_t)dmesg_buf, DMESG_SIZE, 0);
    if (n < 0) {
        puts("dmesg: kernel log unavailable (");
        put_dec(-n);
        puts(")\n");
        last_status = 1;
        return;
    }

    if (*arg) {
        long fd = call(SYS_OPEN, (uintptr_t)arg, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
        if (fd < 0) { file_error(fd); last_status = 1; return; }

        size_t total = (size_t)n, off = 0;
        bool err = false;
        while (off < total) {
            size_t chunk = total - off;
            if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
            long w = call(SYS_WRITE, fd, (uintptr_t)(dmesg_buf + off), chunk);
            if (w <= 0) { err = true; break; }
            off += (size_t)w;
        }
        (void)call(SYS_CLOSE, fd, 0, 0);

        if (err) {
            puts("dmesg: write failed, file may be incomplete\n");
            last_status = 1;
        } else {
            puts("Saved ");
            put_dec(total);
            puts(" bytes to ");
            puts(arg);
            puts("\n");
            last_status = 0;
        }
        return;
    }

    put_dec((uint64_t)n);
    puts(" bytes\n");
    write_bytes(dmesg_buf, (size_t)n);
    if (dmesg_buf[n - 1] != '\n') puts("\n");
    last_status = 0;
}

static int cd_cmd(int argc, char **argv) {
    const char *target = "/";
    if (argc > 1 && argv[1][0]) {
        if (equal(argv[1], "-")) {
            if (!oldpwd[0]) {
                puts("cd: OLDPWD not set\n");
                return 1;
            }
            target = oldpwd;
        } else {
            target = argv[1];
        }
    }

    char prev_cwd[VFS_MAX_PATH];
    for (size_t i = 0; i < sizeof(prev_cwd); i++) prev_cwd[i] = current_cwd[i];

    long r = call(SYS_CHDIR, (uintptr_t)target, 0, 0);
    if (r < 0) {
        if (r == SYSCALL_ENOENT) puts("cd: no such file or directory\n");
        else if (r == SYSCALL_ENOTDIR) puts("cd: not a directory\n");
        else puts("cd: cannot change directory\n");
        return 1;
    }

    for (size_t i = 0; i < sizeof(oldpwd); i++) oldpwd[i] = prev_cwd[i];
    update_cwd();

    if (argc > 1 && equal(argv[1], "-")) {
        puts(current_cwd);
        puts("\n");
    }
    return 0;
}

static int pwd_cmd(void) {
    update_cwd();
    puts(current_cwd);
    puts("\n");
    return 0;
}

static int type_cmd(int argc, char **argv) {
    if (argc < 2) {
        puts("type: missing operand\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (builtin_find(name) != CMD_UNKNOWN) {
            puts(name); puts(" is a shell builtin\n");
            continue;
        }

        const char *al = alias_get(name);
        if (al) {
            puts(name); puts(" is an alias for '"); puts(al); puts("'\n");
            continue;
        }

        bool has_slash = false;
        for (size_t k = 0; name[k]; k++) { if (name[k] == '/') { has_slash = true; break; } }

        vfs_stat_t st;
        if (has_slash) {
            if (call(SYS_STAT, (uintptr_t)name, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
                puts(name); puts(" is "); puts(name); puts("\n");
                continue;
            }
        } else {
            const char *path_var = vars_get("PATH");
            if (!path_var || !*path_var) path_var = "/bin";
            bool found = false;
            const char *p = path_var;
            while (*p) {
                size_t dlen = 0;
                while (p[dlen] && p[dlen] != ':') dlen++;

                char candidate[VFS_MAX_PATH];
                size_t nlen = length(name);
                if (dlen + 1 + nlen + 1 < sizeof(candidate)) {
                    for (size_t k = 0; k < dlen; k++) candidate[k] = p[k];
                    candidate[dlen] = '/';
                    for (size_t k = 0; k < nlen; k++) candidate[dlen + 1 + k] = name[k];
                    candidate[dlen + 1 + nlen] = '\0';

                    if (call(SYS_STAT, (uintptr_t)candidate, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
                        puts(name); puts(" is "); puts(candidate); puts("\n");
                        found = true;
                        break;
                    }
                }
                p += dlen;
                if (*p == ':') p++;
            }
            if (found) continue;
        }

        puts(name); puts(": not found\n");
        ret = 1;
    }
    return ret;
}

static int execute_simple_command(int argc, char **argv, const spawn_fd_action_t *actions, uint32_t action_count) {
    if (argc == 0 || !argv || !argv[0] || !argv[0][0]) return 0;

    const char *cmd = argv[0];
    enum builtin b = builtin_find(cmd);

    if (b == CMD_HELP) {
        builtin_help(argc > 1 ? argv[1] : 0);
        return 0;
    }
    if (b == CMD_CD) {
        return cd_cmd(argc, argv);
    }
    if (b == CMD_PWD) {
        return pwd_cmd();
    }
    if (b == CMD_TYPE) {
        return type_cmd(argc, argv);
    }
    if (b == CMD_COMMAND) {
        if (argc < 2) return 0;
        return execute_simple_command(argc - 1, argv + 1, actions, action_count);
    }
    if (b == CMD_TRUE) {
        return 0;
    }
    if (b == CMD_FALSE) {
        return 1;
    }
    if (b == CMD_EXIT) {
        int code = (argc > 1) ? parse_int(argv[1]) : (int)last_status;
        (void)history_save();
        call(SYS_EXIT, (uintptr_t)code, 0, 0);
        return code;
    }
    if (b == CMD_ECHO) {
        echo_cmd(argc, argv);
        return 0;
    }
    if (b == CMD_SET) {
        vars_print_set();
        return 0;
    }
    if (b == CMD_UNSET) {
        for (int i = 1; i < argc; i++) vars_unset(argv[i]);
        return 0;
    }
    if (b == CMD_EXPORT) {
        if (argc == 1) {
            vars_print_env();
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            vars_export(argv[i]);
        }
        return 0;
    }
    if (b == CMD_ENV) {
        vars_print_env();
        return 0;
    }
    if (b == CMD_ALIAS) {
        if (argc == 1) {
            alias_print_all();
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char aname[MAX_ALIAS_NAME];
            const char *aval = 0;
            if (vars_is_assignment(argv[i], aname, sizeof(aname), &aval)) {
                alias_set(aname, aval);
            } else {
                const char *v = alias_get(argv[i]);
                if (v) {
                    puts("alias "); puts(argv[i]); puts("='"); puts(v); puts("'\n");
                } else {
                    puts("alias: "); puts(argv[i]); puts(": not found\n");
                    return 1;
                }
            }
        }
        return 0;
    }
    if (b == CMD_UNALIAS) {
        if (argc < 2) {
            puts("unalias: missing operand\n");
            return 1;
        }
        for (int i = 1; i < argc; i++) {
            alias_unset(argv[i]);
        }
        return 0;
    }
    if (b == CMD_VERSION) {
        puts("FortressOS v0.1.0-smp (x86_64) — Engineered by Pride1922\n");
        puts("Freestanding C11/NASM Preemptive Microkernel with Limine v8 Bootloader\n");
        return 0;
    }
    if (b == CMD_LS) {
        list(argc > 1 ? argv[1] : ".");
        return (int)last_status;
    }
    if (b == CMD_CAT) {
        cat(argc > 1 ? argv[1] : NULL);
        return (int)last_status;
    }
    if (b == CMD_EDIT) {
        if (argc > 1) {
            editor_load(argv[1]);
            if (editor_ready()) editor_loop();
            return 0;
        }
        puts("Usage: edit /path\n");
        return 1;
    }
    if (b == CMD_MKDIR) {
        if (argc > 1) {
            long r = call(SYS_MKDIR, (uintptr_t)argv[1], 0755, 0);
            if (r < 0) { puts("mkdir: cannot create directory\n"); return 1; }
            return 0;
        }
        puts("Usage: mkdir <path>\n");
        return 1;
    }
    if (b == CMD_RM) {
        if (argc > 1) {
            long r = call(SYS_UNLINK, (uintptr_t)argv[1], 0, 0);
            if (r < 0) {
                if (r == SYSCALL_ENOTEMPTY) puts("rm: directory not empty\n");
                else puts("rm: cannot remove\n");
                return 1;
            }
            return 0;
        }
        puts("Usage: rm <path>\n");
        return 1;
    }
    if (b == CMD_MV) {
        if (argc > 2) {
            long r = call(SYS_RENAME, (uintptr_t)argv[1], (uintptr_t)argv[2], 0);
            if (r < 0) { puts("mv: cannot rename\n"); return 1; }
            return 0;
        }
        puts("Usage: mv <old> <new>\n");
        return 1;
    }
    if (b == CMD_LAYOUT) {
        if (argc > 1) {
            if (equal(argv[1], "azerty")) {
                (void)call(SYS_KBD_LAYOUT, 1, 0, 0);
                puts("Keyboard layout set to Belgian AZERTY.\n");
                return 0;
            } else if (equal(argv[1], "us")) {
                (void)call(SYS_KBD_LAYOUT, 0, 0, 0);
                puts("Keyboard layout set to US QWERTY.\n");
                return 0;
            } else {
                puts("Usage: layout [us | azerty]\n");
                return 1;
            }
        }
        long curr = call(SYS_KBD_LAYOUT, (uintptr_t)-1, 0, 0);
        if (curr == 1) puts("Active keyboard layout: Belgian AZERTY\n");
        else puts("Active keyboard layout: US QWERTY\n");
        return 0;
    }
    if (b == CMD_REBOOT) {
        puts("Restarting system...\n");
        (void)history_save();
        (void)call(SYS_REBOOT, 1, 0, 0);
        return 0;
    }
    if (b == CMD_SHUTDOWN || b == CMD_POWEROFF) {
        puts("Shutting down system...\n");
        (void)history_save();
        (void)call(SYS_REBOOT, 2, 0, 0);
        return 0;
    }
    if (b == CMD_SYNC) {
        long r = call(SYS_SYNC, 0, 0, 0);
        if (r == 0) { puts("Filesystem synced.\n"); return 0; }
        puts("Sync failed (check USB connection or mount mode).\n");
        return 1;
    }
    if (b == CMD_DMESG) {
        dmesg_cmd(argc > 1 ? argv[1] : "");
        return (int)last_status;
    }
    if (b == CMD_HISTORY) {
        shell_history(argc > 1 ? argv[1] : "");
        return 0;
    }
    if (b == CMD_PROMPT) {
        if (argc > 1) {
            if (equal(argv[1], "default")) {
                shell_set_prompt_template("fortress> ");
            } else if (equal(argv[1], "cwd")) {
                shell_set_prompt_template("fortress:<cwd> $ ");
            } else {
                shell_set_prompt_template(argv[1]);
            }
        } else {
            puts("Prompt template: ");
            puts(shell_get_prompt_template());
            puts("\n");
        }
        return 0;
    }
    if (b == CMD_TERMINAL) {
        shell_terminal(argc > 1 ? argv[1] : "");
        return 0;
    }
    if (b == CMD_RUN) {
        if (argc < 2) {
            puts("Usage: run /path [arg...]\n");
            return 1;
        }
        if (argc - 1 > 32) {
            puts("Too many arguments (max 32).\n");
            return 1;
        }
        return spawn_program(argv[1], (const char **)&argv[1], actions, action_count);
    }

    /* Direct program execution */
    const char *target = cmd;
    bool has_slash = false;
    for (size_t k = 0; cmd[k]; k++) {
        if (cmd[k] == '/') { has_slash = true; break; }
    }

    char target_path[VFS_MAX_PATH];
    if (!has_slash) {
        const char *path_var = vars_get("PATH");
        if (!path_var || !*path_var) path_var = "/bin";
        bool found = false;
        const char *p = path_var;
        while (*p) {
            size_t dlen = 0;
            while (p[dlen] && p[dlen] != ':') dlen++;

            size_t clen = length(cmd);
            if (dlen + 1 + clen + 1 < sizeof(target_path)) {
                for (size_t k = 0; k < dlen; k++) target_path[k] = p[k];
                target_path[dlen] = '/';
                for (size_t k = 0; k < clen; k++) target_path[dlen + 1 + k] = cmd[k];
                target_path[dlen + 1 + clen] = '\0';

                vfs_stat_t st;
                if (call(SYS_STAT, (uintptr_t)target_path, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
                    target = target_path;
                    found = true;
                    break;
                }
            }
            p += dlen;
            if (*p == ':') p++;
        }

        if (!found) {
            puts("Unknown command. Type help.\n");
            return 127;
        }
    }

    if (argc > 32) {
        puts("Too many arguments (max 32).\n");
        return 1;
    }
    return spawn_program(target, (const char **)argv, actions, action_count);
}

static bool is_parent_builtin(int argc, char **argv) {
    if (argc == 0 || !argv || !argv[0] || !argv[0][0]) return true;
    int idx = 0;
    while (idx < argc && equal(argv[idx], "command")) {
        idx++;
    }
    if (idx >= argc) return true;
    enum builtin b = builtin_find(argv[idx]);
    if (b == CMD_UNKNOWN || b == CMD_RUN) return false;
    return true;
}

static void execute_parse_tree(parse_tree_t *tree) {
    /* Phase 3 interim guard: reject the entire chain before any side effects.
     * Removed in Phase 4 when the executor gains CMD_OP_PIPE handling. */
    if (parser_execution_guard(tree, puts_err)) {
        last_status = 1;
        return;
    }
    int i = 0;
    int curr_status = (int)last_status;

    while (i < tree->cmd_count) {
        parse_cmd_t *cmd = &tree->cmds[i];
        bool should_run = false;

        if (i == 0 || tree->cmds[i - 1].next_op == CMD_OP_SEMI || tree->cmds[i - 1].next_op == CMD_OP_NONE) {
            should_run = true;
        } else if (tree->cmds[i - 1].next_op == CMD_OP_AND) {
            should_run = (curr_status == 0);
        } else if (tree->cmds[i - 1].next_op == CMD_OP_OR) {
            should_run = (curr_status != 0);
        }

        if (should_run) {
            /* Check for leading assignments: NAME=val */
            int assign_count = 0;
            char assign_name[MAX_VAR_NAME];
            const char *assign_val = NULL;
            while (assign_count < cmd->argc &&
                   cmd->quote_flags[assign_count] && cmd->quote_flags[assign_count][0] == QUOTE_NONE &&
                   vars_is_assignment(cmd->argv[assign_count], assign_name, sizeof(assign_name), &assign_val)) {
                assign_count++;
            }

            if (assign_count == cmd->argc && cmd->argc > 0) {
                /* Pure variable assignments */
                for (int k = 0; k < cmd->argc; k++) {
                    (void)vars_is_assignment(cmd->argv[k], assign_name, sizeof(assign_name), &assign_val);
                    char *v_argv[2] = { (char *)assign_val, NULL };
                    const uint8_t *v_qflags[2] = { cmd->quote_flags[k] + (assign_val - cmd->argv[k]), NULL };
                    bool v_has_quotes[2] = { cmd->has_quotes[k], false };
                    s_val_cmd.argc = 1;
                    s_val_cmd.argv[0] = v_argv[0];
                    s_val_cmd.argv[1] = NULL;
                    s_val_cmd.quote_flags[0] = v_qflags[0];
                    s_val_cmd.quote_flags[1] = NULL;
                    s_val_cmd.has_quotes[0] = v_has_quotes[0];
                    s_val_cmd.negate = false;
                    s_val_cmd.next_op = CMD_OP_NONE;

                    expand_command(&s_val_cmd, curr_status, &s_exp_val);
                    const char *final_val = s_exp_val.argc > 0 ? s_exp_val.argv[0] : "";
                    vars_set(assign_name, final_val, false);
                }
                curr_status = 0;
            } else {
                if (assign_count > 0) {
                    vars_scope_begin(&s_local_scope);
                    for (int k = 0; k < assign_count; k++) {
                        (void)vars_is_assignment(cmd->argv[k], assign_name, sizeof(assign_name), &assign_val);
                        char *v_argv[2] = { (char *)assign_val, NULL };
                        const uint8_t *v_qflags[2] = { cmd->quote_flags[k] + (assign_val - cmd->argv[k]), NULL };
                        bool v_has_quotes[2] = { cmd->has_quotes[k], false };
                        s_val_cmd.argc = 1;
                        s_val_cmd.argv[0] = v_argv[0];
                        s_val_cmd.argv[1] = NULL;
                        s_val_cmd.quote_flags[0] = v_qflags[0];
                        s_val_cmd.quote_flags[1] = NULL;
                        s_val_cmd.has_quotes[0] = v_has_quotes[0];
                        s_val_cmd.negate = false;
                        s_val_cmd.next_op = CMD_OP_NONE;

                        expand_command(&s_val_cmd, curr_status, &s_exp_val);
                        const char *final_val = s_exp_val.argc > 0 ? s_exp_val.argv[0] : "";
                        vars_scope_set(&s_local_scope, assign_name, final_val);
                    }
                }

                /* Form remaining command after assignments */
                s_sub_cmd.argc = cmd->argc - assign_count;
                s_sub_cmd.negate = cmd->negate;
                s_sub_cmd.next_op = cmd->next_op;
                for (int k = 0; k < s_sub_cmd.argc; k++) {
                    s_sub_cmd.argv[k] = cmd->argv[assign_count + k];
                    s_sub_cmd.quote_flags[k] = cmd->quote_flags[assign_count + k];
                    s_sub_cmd.has_quotes[k] = cmd->has_quotes[assign_count + k];
                }
                s_sub_cmd.argv[s_sub_cmd.argc] = NULL;
                s_sub_cmd.quote_flags[s_sub_cmd.argc] = NULL;

                expand_command(&s_sub_cmd, curr_status, &s_expanded_cmd);

                if (is_parent_builtin(s_expanded_cmd.argc, s_expanded_cmd.argv)) {
                    if (cmd->redir_count > 0) {
                        if (redir_apply_parent(cmd->redirs, cmd->redir_count, curr_status, &s_parent_scope) != 0) {
                            curr_status = 1;
                        } else {
                            if (s_expanded_cmd.argc > 0) {
                                curr_status = execute_simple_command(s_expanded_cmd.argc, s_expanded_cmd.argv, NULL, 0);
                            } else {
                                curr_status = 0;
                            }
                            redir_restore_parent(&s_parent_scope);
                        }
                    } else {
                        if (s_expanded_cmd.argc > 0) {
                            curr_status = execute_simple_command(s_expanded_cmd.argc, s_expanded_cmd.argv, NULL, 0);
                        } else {
                            curr_status = 0;
                        }
                    }
                } else {
                    uint32_t spawn_action_count = 0;
                    if (cmd->redir_count > 0) {
                        if (redir_build_spawn_actions(cmd->redirs, cmd->redir_count, curr_status,
                                                      s_spawn_actions, &spawn_action_count,
                                                      s_spawn_target_paths) != 0) {
                            curr_status = 1;
                        } else {
                            curr_status = execute_simple_command(s_expanded_cmd.argc, s_expanded_cmd.argv,
                                                                 s_spawn_actions, spawn_action_count);
                        }
                    } else {
                        curr_status = execute_simple_command(s_expanded_cmd.argc, s_expanded_cmd.argv,
                                                             NULL, 0);
                    }
                }

                if (assign_count > 0) {
                    vars_scope_end(&s_local_scope);
                }
            }

            if (cmd->negate) {
                curr_status = (curr_status == 0) ? 1 : 0;
            }
            last_status = curr_status;
        }

        i++;
    }
}

void shell_main(void) {
    if (call(SYS_READ, 0, 0, 1) != -2 || call(SYS_READ, 0, (uintptr_t)"readonly", 1) != -2 ||
        call(SYS_READ, 0, 0, 0) != 0 ||
        call(SYS_INPUT_READ, 0, 1, 0) != -2 ||
        call(SYS_INPUT_READ, (uintptr_t)"readonly", 1, 0) != -2 ||
        call(SYS_INPUT_READ, (uintptr_t)line_input, 1, 1001) != -1 ||
        call(SYS_INPUT_READ, 0, 0, 0) != 0 ||
        call(SYS_TERMCTL, 0, 0, 32) != -2) {
        puts("[FAIL] stdin validation\n");
        return;
    }

    update_cwd();
    shell_ui_init();
    vars_init();
    alias_init();
    (void)history_load();

    puts("\nFortressOS shell (Ring 3) — Crafted by Pride1922\nType help for commands.\n");

    for (;;) {
        shell_set_prompt_state(last_status, current_cwd);
        if (!shell_read_line(line_input, false)) return;
        if (!line_input[0]) continue;

        size_t cmd_len = 0;
        while (line_input[cmd_len]) {
            cmd_buf[cmd_len] = line_input[cmd_len];
            cmd_len++;
        }
        cmd_buf[cmd_len] = '\0';

        /* Loop for continuation prompt if input is incomplete */
        for (;;) {
            (void)alias_expand_line(cmd_buf, s_alias_line, sizeof(s_alias_line));
            enum parse_result pr = parser_parse(s_alias_line, &parse_tree);
            if (pr == PARSE_INCOMPLETE) {
                if (!shell_read_line(line_input, true)) return;
                if (!line_input[0]) {
                    /* Ctrl+C during continuation cancels input */
                    cmd_buf[0] = '\0';
                    break;
                }
                if (cmd_len + 1 + length(line_input) < sizeof(cmd_buf)) {
                    if (parse_tree.status == LEX_INCOMPLETE_BACKSLASH && cmd_len > 0 && cmd_buf[cmd_len - 1] == '\\') {
                        cmd_buf[cmd_len - 1] = ' ';
                    } else if (parse_tree.status != LEX_INCOMPLETE_OP) {
                        cmd_buf[cmd_len++] = '\n';
                    } else {
                        cmd_buf[cmd_len++] = ' ';
                    }
                    size_t k = 0;
                    while (line_input[k]) {
                        cmd_buf[cmd_len++] = line_input[k++];
                    }
                    cmd_buf[cmd_len] = '\0';
                }
                continue;
            }
            break;
        }

        if (!cmd_buf[0]) continue;

        (void)alias_expand_line(cmd_buf, s_alias_line, sizeof(s_alias_line));
        enum parse_result pr = parser_parse(s_alias_line, &parse_tree);
        if (pr == PARSE_SYNTAX_ERROR) {
            puts(parse_tree.error_msg ? parse_tree.error_msg : "syntax error");
            puts("\n");
            last_status = 2;
            continue;
        }
        if (pr == PARSE_OK) {
            execute_parse_tree(&parse_tree);
        }
    }
}
