#include "entry_security.h"
/* A standalone Ring 3 program. Only the public syscall ABI crosses into kernel. */
#include "types.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "shell/io.h"
#include "shell/ui.h"
#include "shell/builtins.h"
#include "shell/builtin_exec.h"
#include "shell/lexer.h"
#include "shell/parser.h"
#include "shell/history_persist.h"
#include "shell/fileedit.h"
#include "shell/vars.h"
#include "shell/alias.h"
#include "shell/expand.h"
#include "shell/redir.h"
#include "shell/program.h"
#include "shell/pipeline.h"
#include "shell/jobs.h"
#include "shell/jobctl.h"
#define shutdown_coord_syscall call
#include "tools/shutdown_coord.h"
#include "shell/script.h"
#include "shell/control.h"

static char cmd_buf[CONTROL_SOURCE_CAP];
static char line_input[LINE_CAP];
typedef struct {
    parse_tree_t tree;
    local_var_scope_t local;
    redir_scope_t redirs;
    pipeline_cursor_t cursor;
    script_reader_t reader;
    positional_snapshot_t parameters;
    char path[VFS_MAX_PATH];
    int fd,result,initial_status;
    bool supplied,returning,deferred_assign,deferred_redir,deferred_negate;
} source_context_t;
static source_context_t sources[CONTROL_MAX_SOURCES+1];
static unsigned source_depth;
static int entry_script_fd=-1;
#define parse_tree (sources[source_depth].tree)
#define s_local_scope (sources[source_depth].local)
#define s_parent_scope (sources[source_depth].redirs)
static int source_request(int argc,char **argv);
static int source_return(int argc,char **argv);
static int64_t last_status = 0;
static char current_cwd[VFS_MAX_PATH] = "/";
static char oldpwd[VFS_MAX_PATH] = "";
static expanded_cmd_t s_expanded_cmd;
static char s_env_strings[32][MAX_VAR_NAME + MAX_VAR_VAL + 2];
static const char *s_envp_ptrs[33];
static char s_alias_line[CONTROL_SOURCE_CAP];
static parse_cmd_t s_val_cmd;
static parse_cmd_t s_sub_cmd;
static expanded_cmd_t s_exp_val;
static spawn_fd_action_t s_spawn_actions[MAX_SPAWN_ACTIONS];
static char s_spawn_target_paths[MAX_SPAWN_ACTIONS][VFS_MAX_PATH];

/* Flag-only SIGCHLD handler — sets a scalar flag; the main loop drains
 * the record table via jobs_reap_children(). Never prints or reaps here. */
static volatile unsigned g_sigchld_flag;
static void prompt_sigchld(unsigned sig) { (void)sig; g_sigchld_flag = 1; }

static void update_cwd(void) {
    long n = call(SYS_GETCWD, (uintptr_t)current_cwd, sizeof(current_cwd), 0);
    if (n <= 0) {
        current_cwd[0] = '/';
        current_cwd[1] = '\0';
    }
}

static int spawn_program(const char *path, const char **argv, const spawn_fd_action_t *actions, uint32_t action_count) {
    (void)vars_build_envp(s_env_strings, s_envp_ptrs);

    return pipeline_run_program(path,argv,s_envp_ptrs,actions,action_count);
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
        else if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) puts_err("cd: permission denied\n");
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
    if(equal(cmd,"shift")) {
        unsigned n=1;
        if(argc>2) return 2;
        if(argc==2) {
            n=0;if(!argv[1][0]) return 2;
            for(const char *p=argv[1];*p;p++) {
                if(*p<'0' || *p>'9' || n>31) return 2;
                n=n*10+(unsigned)(*p-'0');
            }
        }
        return vars_shift(n);
    }
    enum builtin b = builtin_find(cmd);
    if(b==CMD_BREAK || b==CMD_CONTINUE) return control_request(b==CMD_CONTINUE,argc,argv);

    if (b == CMD_JOBS || b == CMD_FG || b == CMD_BG || b == CMD_KILL)
        return jobctl_exec(argc,(const char *const *)argv);

    if (b == CMD_HELP) {
        /* Route through shared handler: byte-identical with pipeline output. */
        builtin_ctx_t ctx = {0};
        const char *help_argv[2] = {"help", argc > 1 ? argv[1] : NULL};
        int hargc = argc > 1 ? 2 : 1;
        return builtin_exec(hargc, help_argv, &ctx);
    }
    if (b == CMD_CD) {
        return cd_cmd(argc, argv);
    }
    if (b == CMD_UMASK) {
        uint32_t mask=0;
        if (argc>2) { puts("umask: expected one octal mask\n");return 1; }
        if (argc==2) {
            if (!argv[1][0]) return 1;
            for (const char *p=argv[1];*p;p++) {
                if (*p<'0' || *p>'7' || mask>0777u/8u) {puts("umask: invalid octal mask\n");return 1;}
                mask=mask*8u+(unsigned)(*p-'0');
            }
            return call(SYS_UMASK,mask,0,0)<0 ? 1 : 0;
        }
        long old=call(SYS_UMASK,UMASK_QUERY,0,0);if (old<0) return 1;
        char output[]={'0','0'+((old>>6)&7),'0'+((old>>3)&7),'0'+(old&7),'\n'};
        return write_bytes_fd(1,output,sizeof(output))<0 ? 1 : 0;
    }
    if (b == CMD_PWD) {
        builtin_ctx_t ctx = {0};
        const char *pwd_argv[1] = {"pwd"};
        return builtin_exec(1, pwd_argv, &ctx);
    }
    if (b == CMD_TYPE) {
        return type_cmd(argc, argv);
    }
    if (b == CMD_COMMAND) {
        if (argc < 2) return 0;
        return execute_simple_command(argc - 1, argv + 1, actions, action_count);
    }
    if (b == CMD_TEST || b == CMD_TRUE || b == CMD_FALSE || b == CMD_ECHO || b == CMD_PRINTF || b == CMD_ENV ||
        b == CMD_VERSION || b == CMD_CLEAR || b == CMD_LS || b == CMD_VIEW) {
        /* Shared handlers: route through builtin_exec for byte-identical output. */
        (void)vars_build_envp(s_env_strings, s_envp_ptrs);
        builtin_ctx_t ctx = builtin_ctx_from_envp(s_envp_ptrs);
        return builtin_exec(argc, (const char *const *)argv, &ctx);
    }
    if (b == CMD_SOURCE) return source_request(argc,argv);
    if (b == CMD_RETURN) return source_return(argc,argv);
    if (b == CMD_EXIT) {
        unsigned code=(unsigned)last_status & 255u;
        if(argc>2) {puts_err("exit: too many arguments\n");return 2;}
        if(argc==2) {
            code=0;if(!argv[1][0]) return 2;
            for(const char *p=argv[1];*p;p++) {
                if(*p<'0' || *p>'9' || code>255u/10u) {puts_err("exit: expected 0..255\n");return 2;}
                code=code*10+(unsigned)(*p-'0');if(code>255) return 2;
            }
        }
        if(!shell_script_mode) (void)history_save();
        jobs_shutdown();
        call(SYS_EXIT, (uintptr_t)code, 0, 0);
        return code;
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
            if (r < 0) { puts_err("mkdir: "); file_error_err(r); return 1; }
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
                else { puts_err("rm: "); file_error_err(r); }
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
            if (r < 0) { puts_err("mv: "); file_error_err(r); return 1; }
            return 0;
        }
        puts("Usage: mv <old> <new>\n");
        return 1;
    }
    if (b == CMD_CP) {
        if (argc > 2) {
            const char *dest = argv[argc - 1];
            vfs_stat_t dst_st;
            long dst_r = call(SYS_STAT, (uintptr_t)dest, (uintptr_t)&dst_st, 0);
            bool dst_is_dir = (dst_r == 0 && dst_st.type == VFS_DIRECTORY);
            if (argc > 3 && !dst_is_dir) {
                puts_err("cp: target is not a directory\n");
                return 1;
            }
            int status = 0;
            char target_buf[VFS_MAX_PATH];
            char copy_buf[512];
            for (int i = 1; i < argc - 1; i++) {
                const char *src = argv[i];
                const char *target = dest;
                if (dst_is_dir) {
                    const char *base = src;
                    for (const char *p = src; *p; p++) if (*p == '/') base = p + 1;
                    size_t dlen = length(dest);
                    size_t blen = length(base);
                    bool need_slash = (dlen > 0 && dest[dlen - 1] != '/');
                    if (dlen + (need_slash ? 1 : 0) + blen >= sizeof(target_buf)) {
                        puts_err("cp: path too long\n");
                        status = 1;
                        continue;
                    }
                    size_t pos = 0;
                    for (size_t k = 0; k < dlen; k++) target_buf[pos++] = dest[k];
                    if (need_slash) target_buf[pos++] = '/';
                    for (size_t k = 0; k < blen; k++) target_buf[pos++] = base[k];
                    target_buf[pos] = '\0';
                    target = target_buf;
                }
                if (equal(src, target)) {
                    puts_err("cp: source and destination are identical\n");
                    status = 1;
                    continue;
                }
                vfs_stat_t src_st;
                long sr = call(SYS_STAT, (uintptr_t)src, (uintptr_t)&src_st, 0);
                if (sr < 0) {
                    puts_err("cp: "); file_error_err(sr);
                    status = 1;
                    continue;
                }
                if (src_st.type == VFS_DIRECTORY) {
                    puts_err("cp: omitting directory\n");
                    status = 1;
                    continue;
                }
                long sfd = call(SYS_OPEN, (uintptr_t)src, VFS_O_RDONLY, 0);
                if (sfd < 0) {
                    puts_err("cp: "); file_error_err(sfd);
                    status = 1;
                    continue;
                }
                long dfd = call(SYS_OPEN, (uintptr_t)target, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0);
                if (dfd < 0) {
                    call(SYS_CLOSE, (uintptr_t)sfd, 0, 0);
                    puts_err("cp: "); file_error_err(dfd);
                    status = 1;
                    continue;
                }
                bool copy_ok = true;
                for (;;) {
                    long n = call(SYS_READ, (uintptr_t)sfd, (uintptr_t)copy_buf, sizeof(copy_buf));
                    if (n < 0) {
                        puts_err("cp: read error\n");
                        copy_ok = false;
                        break;
                    }
                    if (n == 0) break;
                    size_t rem = (size_t)n;
                    const char *p = copy_buf;
                    while (rem > 0) {
                        long w = call(SYS_WRITE, (uintptr_t)dfd, (uintptr_t)p, (uintptr_t)rem);
                        if (w <= 0) {
                            puts_err("cp: write error\n");
                            copy_ok = false;
                            break;
                        }
                        p += w;
                        rem -= (size_t)w;
                    }
                    if (!copy_ok) break;
                }
                call(SYS_CLOSE, (uintptr_t)sfd, 0, 0);
                long c = call(SYS_CLOSE, (uintptr_t)dfd, 0, 0);
                if (c < 0 && copy_ok) {
                    puts_err("cp: close error\n");
                    copy_ok = false;
                }
                if (copy_ok && (src_st.mode & 07777)) {
                    long chr = call(SYS_CHMOD, (uintptr_t)target, src_st.mode & 07777, 0);
                    if (chr < 0) {
                        puts_err("cp: preserving permissions for '");
                        puts_err(target);
                        puts_err("' failed: ");
                        file_error_err(chr);
                        status = 1;
                    }
                }
                if (!copy_ok) status = 1;
            }
            return status;
        }
        puts("Usage: cp <source> <dest>\n");
        return 1;
    }
    if (b == CMD_TOUCH) {
        if (argc > 1) {
            int status = 0;
            for (int i = 1; i < argc; i++) {
                long r = call(SYS_OPEN, (uintptr_t)argv[i], VFS_O_CREAT | VFS_O_WRONLY, 0);
                if (r < 0) {
                    puts_err("touch: ");
                    file_error_err(r);
                    status = 1;
                } else {
                    call(SYS_CLOSE, (uintptr_t)r, 0, 0);
                }
            }
            return status;
        }
        puts("Usage: touch <path> ...\n");
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
        long r = shutdown_coordinate_and_reboot(1);
        if (r == -1 || r == -SYSCALL_EPERM) {
            const char *argv[] = {"/sbin/reboot", NULL};
            spawn_opts_t opts = {.size = sizeof(opts), .version = 1, .argv = (uintptr_t)argv};
            long pid = call(SYS_SPAWN_EXT, (uintptr_t)"/sbin/reboot", (uintptr_t)&opts, sizeof(opts));
            if (pid < 0) {
                argv[0] = "/bin/reboot";
                pid = call(SYS_SPAWN_EXT, (uintptr_t)"/bin/reboot", (uintptr_t)&opts, sizeof(opts));
            }
            if (pid >= 0) {
                uint64_t status = 0;
                call(SYS_WAIT, pid, (uintptr_t)&status, 0);
                return (int)status;
            }
        }
        if (r < 0) { puts_err("reboot: "); file_error_err(r); return 1; }
        return 0;
    }
    if (b == CMD_SHUTDOWN || b == CMD_POWEROFF) {
        puts("Shutting down system...\n");
        (void)history_save();
        long r = shutdown_coordinate_and_reboot(2);
        if (r == -1 || r == -SYSCALL_EPERM) {
            const char *argv[] = {"/sbin/shutdown", NULL};
            spawn_opts_t opts = {.size = sizeof(opts), .version = 1, .argv = (uintptr_t)argv};
            long pid = call(SYS_SPAWN_EXT, (uintptr_t)"/sbin/shutdown", (uintptr_t)&opts, sizeof(opts));
            if (pid < 0) {
                argv[0] = "/bin/shutdown";
                pid = call(SYS_SPAWN_EXT, (uintptr_t)"/bin/shutdown", (uintptr_t)&opts, sizeof(opts));
            }
            if (pid >= 0) {
                uint64_t status = 0;
                call(SYS_WAIT, pid, (uintptr_t)&status, 0);
                return (int)status;
            }
        }
        if (r < 0) { puts_err("shutdown: "); file_error_err(r); return 1; }
        return 0;
    }
    if (b == CMD_SYNC) {
        long r = call(SYS_SYNC, 0, 0, 0);
        if (r == 0) { puts("Filesystem synced.\n"); return 0; }
        puts("Sync failed (check USB connection or mount mode).\n");
        return 1;
    }
    if (b == CMD_DMESG) {
        last_status = exec_dmesg(argc, (const char *const *)argv);
        return (int)last_status;
    }
    if (b == CMD_HISTORY) {
        shell_history(argc > 1 ? argv[1] : "");
        return 0;
    }
    if (b == CMD_PROMPT) {
        if (argc > 1) {
            if (equal(argv[1], "default")) {
                shell_set_prompt_template("fortress:<cwd> $ ");
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
    char target_path[VFS_MAX_PATH];
    const char *target = cmd;
    bool has_slash = false;
    for (size_t i = 0; cmd[i]; i++) if (cmd[i] == '/') has_slash = true;
    if (!has_slash) {
        if (program_resolve(cmd, target_path)) {
            puts("Unknown command. Type help.\n");
            return 127;
        }
        target = target_path;
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
    if(equal(argv[idx],"shift")) return true;
    enum builtin b = builtin_find(argv[idx]);
    if (b == CMD_UNKNOWN || b == CMD_RUN) return false;
    return true;
}

static bool script_descriptor(int fd) {
    if(fd<0) return false;
    if(fd==entry_script_fd) return true;
    for(unsigned d=0;d<=source_depth;d++) {
        if(d && fd==sources[d].fd) return true;
        if(d<source_depth && sources[d].deferred_redir)
            for(int j=0;j<sources[d].redirs.count;j++)
                if(fd==sources[d].redirs.saved[j].saved_fd) return true;
    }
    return false;
}
static bool script_redirections_valid(const parse_cmd_t *cmd) {
    /* Reader and suspended-redirection descriptors are interpreter-owned.
     * Reject explicit attempts to overwrite them instead of corrupting a
     * caller's continuation. Ordinary stdin/stdout/stderr remain available. */
    for(int i=0;i<cmd->redir_count;i++) {
        const redir_t *r=&cmd->redirs[i];
        bool reserved=script_descriptor(r->redir_fd);
        if(r->redir_op==REDIR_DUP_IN || r->redir_op==REDIR_DUP_OUT)
            reserved=reserved || script_descriptor(r->redir_dup_fd);
        if(reserved) {puts_err("sh: descriptor reserved by script context\n");return false;}
    }
    return true;
}
static int execute_single_command(parse_cmd_t *cmd, int curr_status) {
    last_status = curr_status;
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

            if(expand_command_checked(&s_val_cmd, curr_status, &s_exp_val)) return 1;
            const char *final_val = s_exp_val.argc > 0 ? s_exp_val.argv[0] : "";
            if(vars_set(assign_name, final_val, false)) return 1;
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

                if(expand_command_checked(&s_val_cmd, curr_status, &s_exp_val)) {vars_scope_end(&s_local_scope);return 1;}
                const char *final_val = s_exp_val.argc > 0 ? s_exp_val.argv[0] : "";
                if(vars_scope_set(&s_local_scope, assign_name, final_val)) {vars_scope_end(&s_local_scope);return 1;}
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

        if(expand_command_checked(&s_sub_cmd, curr_status, &s_expanded_cmd)) {
            if(assign_count) vars_scope_end(&s_local_scope);
            return 1;
        }

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
                    if(control_stop==CONTROL_SOURCE_PENDING) sources[source_depth].deferred_redir=true;
                    else redir_restore_parent(&s_parent_scope);
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

        if(control_stop==CONTROL_SOURCE_PENDING) {
            sources[source_depth].deferred_assign=assign_count>0;
            sources[source_depth].deferred_negate=cmd->negate;
            return 0;
        }
        if (assign_count > 0) {
            vars_scope_end(&s_local_scope);
        }
    }

    if (cmd->negate) {
        curr_status = (curr_status == 0) ? 1 : 0;
    }
    last_status = curr_status;
    return curr_status;
}

static void execute_parse_tree(parse_tree_t *tree, const char *cmd_text) {
    /* A child such as sudo shell may reboot without ever returning here.
     * Flush before launch, while HOME still belongs to the calling shell. */
    for (int i = 0; !shell_script_mode && i < tree->cmd_count; i++) {
        if (!is_parent_builtin(tree->cmds[i].argc, tree->cmds[i].argv)) {
            history_flush_pending();
            break;
        }
    }
    last_status = execute_command_list_resume(tree, (int)last_status,
                     execute_single_command, cmd_text,&sources[source_depth].cursor);
}

static int control_simple(const char *text,int status) {
    last_status=status;
    if(!sources[source_depth].cursor.waiting) {
        enum parse_result pr=parser_parse(text,&parse_tree);
        if(pr!=PARSE_OK && pr!=PARSE_EMPTY) return 2;
        if(pr==PARSE_EMPTY) return 0;
    }
    /* Include pipeline stages: duplicating a reader in a child could advance
     * the caller's shared file offset even though CLOEXEC closes the original. */
    for(int i=0;i<parse_tree.cmd_count;i++)
        if(!script_redirections_valid(&parse_tree.cmds[i])) return 2;
    execute_parse_tree(&parse_tree,text);
    if(last_status==128+SIGINT && pipeline_last_signal()==SIGINT) script_interrupted=SIGINT;
    return (int)last_status;
}
static int source_request(int argc,char **argv) {
    if(argc<2) {puts_err("source: expected file [args]\n");return 2;}
    if(source_depth==CONTROL_MAX_SOURCES) {puts_err("source: nesting exceeds eight\n");return 2;}
    source_context_t *next=&sources[source_depth+1];
    size_t n=length(argv[1]);
    if(!n || n>=sizeof(next->path)) {puts_err("source: invalid path\n");return 2;}
    for(size_t i=0;i<=n;i++) next->path[i]=argv[1][i];
    int fd=(int)call(SYS_OPEN,(uintptr_t)next->path,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if(fd<0) {puts_err("source: cannot open file\n");return 1;}
    stat_ext_v1_t st={0};
    if(call(SYS_FSTAT,fd,(uintptr_t)&st,0)<0 || st.type!=VFS_FILE) {
        call(SYS_CLOSE,fd,0,0);puts_err("source: expected regular file\n");return 1;
    }
    long reader=call(SYS_FCNTL,fd,F_DUPFD_CLOEXEC,20);
    call(SYS_CLOSE,fd,0,0);
    if(reader<0) {puts_err("source: no reader descriptor available\n");return 1;}
    fd=(int)reader;
    next->supplied=argc>2;
    if(next->supplied) {
        vars_save_positionals(&next->parameters);
        if(!vars_positionals(next->parameters.bytes,argc-2,argv+2)) {
            call(SYS_CLOSE,fd,0,0);puts_err("source: argument budget exceeded\n");return 2;
        }
    }
    next->fd=fd;next->result=0;next->initial_status=(int)last_status;next->returning=false;
    next->cursor=(pipeline_cursor_t){0};
    next->deferred_assign=next->deferred_redir=next->deferred_negate=false;
    script_reader_init(&next->reader);
    control_stop=CONTROL_SOURCE_PENDING;return 0;
}
static int source_return(int argc,char **argv) {
    if(!source_depth) {puts_err("return: outside sourced file\n");return 2;}
    if(argc>2) {puts_err("return: expected status 0..255\n");return 2;}
    unsigned status=(unsigned)last_status & 255u;
    if(argc==2) {
        status=0;if(!argv[1][0]) return 2;
        for(const char *p=argv[1];*p;p++) {
            if(*p<'0' || *p>'9' || status>25) {puts_err("return: expected status 0..255\n");return 2;}
            status=status*10+(unsigned)(*p-'0');if(status>255) return 2;
        }
    }
    sources[source_depth].returning=true;
    sources[source_depth].result=(int)status;
    control_stop=CONTROL_RETURN_PENDING;return (int)status;
}
static int source_finish(int status) {
    source_context_t *child=&sources[source_depth];
    if(call(SYS_CLOSE,child->fd,0,0)<0) status=2;
    if(child->supplied) vars_restore_positionals(&child->parameters);
    source_depth--;control_select(source_depth);
    source_context_t *parent=&sources[source_depth];
    if(parent->deferred_redir) redir_restore_parent(&parent->redirs);
    if(parent->deferred_assign) vars_scope_end(&parent->local);
    if(parent->deferred_negate) status=status==0 ? 1 : 0;
    parent->deferred_assign=parent->deferred_redir=parent->deferred_negate=false;
    return status;
}
static void script_signal(unsigned sig) {script_interrupted=sig;}
static int run_control(void) {
    signal_action_t saved,action={.handler=(uintptr_t)script_signal};bool changed=false;
    if(!shell_script_mode) {
        script_interrupted=0;
        if(call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,(uintptr_t)&saved)<0) return 2;
        changed=true;
    }
    pipeline_set_stop_flag(&control_stop);pipeline_set_cancel_flag(&script_interrupted);
    int r=(int)last_status;bool input=false;
    /* Source calls yield through the complete executor stack. This single
     * driver owns all readers and continuations, independent of source depth. */
    for(;;) {
        if(input) {
            source_context_t *ctx=&sources[source_depth];
            int next=script_reader_next(&ctx->reader,ctx->fd,ctx->path,control_compile);
            if(next<=0) {
                r=next<0 ? (script_interrupted ? 128+(int)script_interrupted : 2) : ctx->result;
                r=source_finish(r);input=false;continue;
            }
            /* Comments/blank lines do not change $? before the first command,
             * nor do they count as a command result for an otherwise empty file. */
            if(control_empty()) continue;
            input=false;
        }
        r=control_execute(r,control_simple,&script_interrupted);
        if(r==CONTROL_YIELD) {
            source_depth++;control_select(source_depth);input=true;
            r=sources[source_depth].initial_status;continue;
        }
        if(!source_depth) break;
        /* return is a transfer: its explicit status survives a local ! prefix.
         * A source command's caller may still negate the completed source. */
        if(sources[source_depth].returning && !script_interrupted) r=sources[source_depth].result;
        else sources[source_depth].result=r;
        if(sources[source_depth].returning || script_interrupted) {
            r=source_finish(r);input=false;
        } else input=true;
    }
    sources[0].cursor=(pipeline_cursor_t){0};
    if(changed && call(SYS_SIGACTION,SIGINT,(uintptr_t)&saved,0)<0) r=2;
    return r;
}
static int script_eval(const char *text) {
    int r=control_compile(text);
    if(r==-2) {puts_err("sh: ");puts_err(control_error());puts_err("\n");}
    if(r) return r;
    last_status=run_control();return (int)last_status;
}
static int script_entry(int argc,char **argv,const char *const *envp) {
    int index=1,fd=0,count=0;const char *text=NULL,*name="sh";char **args=NULL;
    bool opened=false;
    if(index<argc && equal(argv[index],"-c")) {
        if(++index>=argc) {puts_err("sh: -c needs command text\n");return 2;}
        text=argv[index++];
        if(index<argc) name=argv[index++];
    } else {
        if(index<argc && equal(argv[index],"--")) index++;
        else if(index<argc && argv[index][0]=='-') {puts_err("sh: unsupported option\n");return 2;}
        if(index<argc) {
            name=argv[index++];fd=(int)call(SYS_OPEN,(uintptr_t)name,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
            if(fd<0) {puts_err("sh: cannot open script\n");return 2;}
            stat_ext_v1_t st={0};
            if(call(SYS_FSTAT,fd,(uintptr_t)&st,0)<0 || st.type!=VFS_FILE) {
                call(SYS_CLOSE,fd,0,0);puts_err("sh: expected regular script file\n");return 2;
            }
            long reader=call(SYS_FCNTL,fd,F_DUPFD_CLOEXEC,20);
            call(SYS_CLOSE,fd,0,0);
            if(reader<0) {puts_err("sh: no reader descriptor available\n");return 2;}
            fd=(int)reader;entry_script_fd=fd;
            opened=true;
        }
    }
    count=argc-index;args=argv+index;
    if(!vars_positionals(name,count,args)) {if(opened) call(SYS_CLOSE,fd,0,0);return 2;}
    vars_init();
    if(!user_entry_secure(envp) && !vars_import(envp)) {if(opened) call(SYS_CLOSE,fd,0,0);return 2;}
    update_cwd();jobs_init();pipeline_set_interactive(false);pipeline_set_cancel_flag(&script_interrupted);
    signal_action_t action={.handler=(uintptr_t)script_signal};
    if(call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0)<0 ||
       call(SYS_SIGACTION,SIGTERM,(uintptr_t)&action,0)<0 ||
       call(SYS_SIGACTION,SIGHUP,(uintptr_t)&action,0)<0) {
        if(opened) call(SYS_CLOSE,fd,0,0);
        return 2;
    }
    action.handler=(uintptr_t)prompt_sigchld;
    if(call(SYS_SIGACTION,SIGCHLD,(uintptr_t)&action,0)<0) {if(opened) call(SYS_CLOSE,fd,0,0);return 2;}
    int status=script_run(fd,text,name,script_eval);
    if(opened && call(SYS_CLOSE,fd,0,0)<0) status=2;
    jobs_shutdown();return status;
}
int shell_main(int argc,char **argv,const char *const *envp) {
    shell_script_mode=argc>1 || call(SYS_TERMCTL,TERM_ISATTY,0,0)!=1;
    if(shell_script_mode) return script_entry(argc,argv,envp);
    if (call(SYS_READ, 0, 0, 1) != -2 || call(SYS_READ, 0, (uintptr_t)"readonly", 1) != -2 ||
        call(SYS_READ, 0, 0, 0) != 0 ||
        call(SYS_INPUT_READ, 0, 1, 0) != -2 ||
        call(SYS_INPUT_READ, (uintptr_t)"readonly", 1, 0) != -2 ||
        call(SYS_INPUT_READ, (uintptr_t)line_input, 1, 1001) != -1 ||
        call(SYS_INPUT_READ, 0, 0, 0) != 0 ||
        call(SYS_TERMCTL, 0, 0, 32) != -2) {
        puts("[FAIL] stdin validation\n");
        return 1;
    }

    update_cwd();
    shell_ui_init();
    vars_init();
    if (!user_entry_secure(envp) && !vars_import(envp)) {puts("shell: invalid entry environment\n");return 1;}
    (void)vars_positionals(argc && argv ? argv[0] : "sh",0,NULL);
    alias_init();
    (void)history_load();
    int term_fd = shell_get_terminal_fd();
    puts_fd(term_fd, "\033[2J\033[H");

    /* Display /etc/motd or fallback OS banner */
    long motd_fd = call(SYS_OPEN, (uintptr_t)"/etc/motd", VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (motd_fd >= 0) {
        char motd_buf[512];
        long n;
        while ((n = call(SYS_READ, (uintptr_t)motd_fd, (uintptr_t)motd_buf, sizeof(motd_buf))) > 0) {
            write_bytes_fd(term_fd, motd_buf, (size_t)n);
        }
        (void)call(SYS_CLOSE, (uintptr_t)motd_fd, 0, 0);
        puts_fd(term_fd, "\n");
    } else {
        puts_fd(term_fd, "========================================================\n  Welcome to FortressOS (x86_64 SMP)\n  \"Security through Isolation and Elegance\"\n========================================================\n\n");
    }
    signal_action_t chld_action = { .handler = (uintptr_t)prompt_sigchld };
    (void)call(SYS_SIGACTION, SIGCHLD, (uintptr_t)&chld_action, 0);

    jobs_init();

    for (;;) {
        /* Reap background children and GC notified slots before prompt. */
        jobs_reap_children();
        jobs_gc();

        shell_set_prompt_state(last_status, current_cwd);
        if (!shell_read_line(line_input, false)) { jobs_shutdown(); return (int)last_status; }
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
            int pr = control_compile(s_alias_line);
            if (pr == -1) {
                if (!shell_read_line(line_input, true)) { jobs_shutdown(); return (int)last_status; }
                if (!line_input[0]) {
                    /* Ctrl+C during continuation cancels input */
                    cmd_buf[0] = '\0';
                    break;
                }
                if (cmd_len + 1 + length(line_input) < sizeof(cmd_buf)) {
                    cmd_buf[cmd_len++] = '\n';
                    size_t k = 0;
                    while (line_input[k]) {
                        cmd_buf[cmd_len++] = line_input[k++];
                    }
                    cmd_buf[cmd_len] = '\0';
                } else {
                    puts_err("sh: compound unit exceeds 16 KiB\n");
                    cmd_buf[0]=0;last_status=2;break;
                }
                continue;
            }
            break;
        }

        if (!cmd_buf[0]) continue;

        (void)alias_expand_line(cmd_buf, s_alias_line, sizeof(s_alias_line));
        int pr = control_compile(s_alias_line);
        if (pr == -2) {
            puts(control_error());
            puts("\n");
            last_status = 2;
            history_autoflush_maybe();
            continue;
        }
        if (pr == 0) {
            last_status=run_control();
        }

        /* Drain any reports generated during execution. */
        jobs_reap_children();
        history_autoflush_maybe();
    }
}
