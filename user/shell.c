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

static char cmd_buf[LINE_CAP * 2];
static char line_input[LINE_CAP];
static parse_tree_t parse_tree;
static int64_t last_status = 0;
static char dmesg_buf[DMESG_SIZE];
static char current_cwd[VFS_MAX_PATH] = "/";
static char oldpwd[VFS_MAX_PATH] = "";

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
    vfs_stat_t st;
    long result = call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); last_status = 1; return; }
    if (st.type != VFS_FILE) { puts("Not a regular file.\n"); last_status = 1; return; }
    long fd = call(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); last_status = 1; return; }
    char buf[512];
    bool newline = true;
    while ((result = call(SYS_READ, fd, (uintptr_t)buf, sizeof(buf))) > 0) {
        for (long i = 0; i < result; i++) {
            unsigned char c = buf[i];
            if (c != '\n' && c != '\t' && (c < 32 || c > 126)) buf[i] = '.';
        }
        newline = buf[result - 1] == '\n';
        write_bytes(buf, result);
    }
    if (!newline) puts("\n");
    if (result < 0) { file_error(result); last_status = 1; }
    else { last_status = 0; }
    (void)call(SYS_CLOSE, fd, 0, 0);
}

static int spawn_program(const char *path, const char **argv) {
    long pid = call(SYS_SPAWN, (uintptr_t)path, (uintptr_t)argv, 0);
    if (pid < 0) {
        switch (pid) {
            case SYSCALL_ENOENT: puts("No such file or directory.\n"); return 127;
            case SYSCALL_ENOEXEC: puts("Invalid executable.\n"); return 126;
            case SYSCALL_ENOMEM: puts("Out of memory or process capacity.\n"); return 1;
            case SYSCALL_EISDIR: puts("Not a regular file.\n"); return 126;
            case SYSCALL_EFBIG: puts("Executable exceeds 4 MiB limit.\n"); return 126;
            case SYSCALL_E2BIG: puts("Argument list too long.\n"); return 1;
            default: puts("Unable to load executable.\n"); return 1;
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
        const char *arg = argv[i];
        while (*arg) {
            if (arg[0] == '$' && arg[1] == '?') {
                if (last_status < 0) { puts("-"); put_dec(0 - (uint64_t)last_status); }
                else put_dec((uint64_t)last_status);
                arg += 2;
            } else {
                char ch[2] = { *arg++, 0 };
                puts(ch);
            }
        }
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
        char bin_path[VFS_MAX_PATH];
        bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
        size_t n = 0;
        while (name[n] && n < sizeof(bin_path) - 6) { bin_path[5 + n] = name[n]; n++; }
        bin_path[5 + n] = '\0';

        vfs_stat_t st;
        if (call(SYS_STAT, (uintptr_t)bin_path, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
            puts(name); puts(" is "); puts(bin_path); puts("\n");
            continue;
        }

        bool has_slash = false;
        for (size_t k = 0; name[k]; k++) { if (name[k] == '/') { has_slash = true; break; } }
        if (has_slash && call(SYS_STAT, (uintptr_t)name, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
            puts(name); puts(" is "); puts(name); puts("\n");
            continue;
        }

        puts(name); puts(": not found\n");
        ret = 1;
    }
    return ret;
}

static int execute_simple_command(int argc, char **argv) {
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
        return execute_simple_command(argc - 1, argv + 1);
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
    if (b == CMD_LS) {
        list(argc > 1 ? argv[1] : ".");
        return (int)last_status;
    }
    if (b == CMD_CAT) {
        if (argc > 1) cat(argv[1]);
        else { puts("Usage: cat /path\n"); last_status = 1; }
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
        return spawn_program(argv[1], (const char **)&argv[1]);
    }

    /* Direct program execution */
    const char *target = cmd;
    bool has_slash = false;
    for (size_t k = 0; cmd[k]; k++) {
        if (cmd[k] == '/') { has_slash = true; break; }
    }

    char bin_path[VFS_MAX_PATH];
    if (!has_slash) {
        bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
        size_t n = 0;
        while (cmd[n] && n < sizeof(bin_path) - 6) { bin_path[5 + n] = cmd[n]; n++; }
        bin_path[5 + n] = '\0';

        vfs_stat_t st;
        if (call(SYS_STAT, (uintptr_t)bin_path, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
            target = bin_path;
        } else {
            puts("Unknown command. Type help.\n");
            return 127;
        }
    }

    if (argc > 32) {
        puts("Too many arguments (max 32).\n");
        return 1;
    }
    return spawn_program(target, (const char **)argv);
}

static void execute_parse_tree(parse_tree_t *tree) {
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
            curr_status = execute_simple_command(cmd->argc, cmd->argv);
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
    (void)history_load();

    puts("\nFortressOS shell (Ring 3)\nType help for commands.\n");

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
            enum parse_result pr = parser_parse(cmd_buf, &parse_tree);
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

        enum parse_result pr = parser_parse(cmd_buf, &parse_tree);
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
