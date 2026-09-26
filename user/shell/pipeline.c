#include "pipeline.h"
#include "program.h"
#include "expand.h"
#include "vars.h"
#include "redir.h"
#include "builtins.h"

typedef struct {
    char args[MAX_TOTAL_ARGS_LEN];
    const char *argv[MAX_SPAWN_ARGS + 1];
    char env[32][MAX_VAR_NAME + MAX_VAR_VAL + 2];
    const char *envp[33];
    char path[VFS_MAX_PATH];
    char targets[MAX_SPAWN_ACTIONS][VFS_MAX_PATH];
    spawn_fd_action_t actions[MAX_SPAWN_ACTIONS];
    uint32_t action_count;
    int resolve_status;
    long pid;
} pipeline_stage_t;

static pipeline_stage_t stages[MAX_PIPE_STAGES];
static int pipes[MAX_PIPE_STAGES - 1][2];
static local_var_scope_t scope;
static parse_cmd_t subcmd;
static expanded_cmd_t expanded;

static int prepare_stage(parse_cmd_t *cmd, int index, int count, int status) {
    pipeline_stage_t *stage = &stages[index];
    int first = 0;
    char name[MAX_VAR_NAME];
    const char *value;
    while (first < cmd->argc && cmd->quote_flags[first] &&
           cmd->quote_flags[first][0] == QUOTE_NONE &&
           vars_is_assignment(cmd->argv[first], name, sizeof(name), &value)) {
        subcmd.argc = 1;
        subcmd.argv[0] = (char *)value;
        subcmd.argv[1] = NULL;
        subcmd.quote_flags[0] = cmd->quote_flags[first] + (value - cmd->argv[first]);
        subcmd.has_quotes[0] = cmd->has_quotes[first];
        if (expand_command_checked(&subcmd, status, &expanded) != 0) return 1;
        if (vars_scope_set(&scope, name, expanded.argc ? expanded.argv[0] : "") != 0) {
            puts_err("pipeline: too many local assignments or variables\n");
            return 1;
        }
        first++;
    }
    subcmd.argc = cmd->argc - first;
    for (int i = 0; i < subcmd.argc; i++) {
        subcmd.argv[i] = cmd->argv[first + i];
        subcmd.quote_flags[i] = cmd->quote_flags[first + i];
        subcmd.has_quotes[i] = cmd->has_quotes[first + i];
    }
    subcmd.argv[subcmd.argc] = NULL;
    if (expand_command_checked(&subcmd, status, &expanded) != 0) return 1;
    if (!expanded.argc || !expanded.argv[0][0] ||
        builtin_find(expanded.argv[0]) != CMD_UNKNOWN) {
        puts_err("pipeline: unsupported stage\n");
        return 1;
    }
    if (expanded.argc > MAX_SPAWN_ARGS) return program_error(SYSCALL_E2BIG);

    /* Expansion results alias one shared arena. Copy before expanding another stage. */
    size_t used = 0;
    for (int i = 0; i < expanded.argc; i++) {
        size_t n = length(expanded.argv[i]) + 1;
        if (n > MAX_ARG_STRLEN || n > sizeof(stage->args) - used)
            return program_error(SYSCALL_E2BIG);
        stage->argv[i] = stage->args + used;
        for (size_t j = 0; j < n; j++) stage->args[used++] = expanded.argv[i][j];
    }
    stage->argv[expanded.argc] = NULL;
    int envc = vars_build_envp(stage->env, stage->envp);
    size_t env_used = 0;
    for (int i = 0; i < envc; i++) {
        size_t n = length(stage->envp[i]) + 1;
        if (n > MAX_ENV_STRLEN || n > MAX_TOTAL_ENVP_LEN - env_used)
            return program_error(SYSCALL_E2BIG);
        env_used += n;
    }
    /* Missing PATH commands fail at their launch position, just like missing
     * explicit paths. All expansions/preflight still finish before any spawn. */
    stage->resolve_status = program_resolve(stage->argv[0], stage->path);

    uint32_t wiring = (index > 0) + (index + 1 < count);
    if ((uint32_t)cmd->redir_count + wiring > MAX_SPAWN_ACTIONS) {
        puts_err("pipeline: too many spawn actions (max 16)\n");
        return 1;
    }
    uint32_t redirs;
    if (redir_build_spawn_actions(cmd->redirs, cmd->redir_count, status,
                                  stage->actions, &redirs, stage->targets) != 0) return 1;
    for (uint32_t i = redirs; i > 0; i--)
        stage->actions[i - 1 + wiring] = stage->actions[i - 1];
    stage->action_count = wiring + redirs;
    return 0;
}

static int pipeline_preflight(parse_tree_t *tree, int first, int count, int status,
                              uint32_t *reserved) {
    /* Private descriptors must not masquerade as explicit user redirection fds. */
    *reserved = 7u | (1u << 31);
    for (int i = 0; i < count; i++) {
        parse_cmd_t *cmd = &tree->cmds[first + i];
        if (i && cmd->negate) {
            puts_err("pipeline: negation is only supported before the first stage\n");
            return 1;
        }
        vars_scope_begin(&scope);
        int result = prepare_stage(cmd, i, count, status);
        vars_scope_end(&scope);
        if (result) return result;
        for (int j = 0; j < cmd->redir_count; j++) {
            *reserved |= 1u << cmd->redirs[j].redir_fd;
            if (cmd->redirs[j].redir_op == REDIR_DUP_IN ||
                cmd->redirs[j].redir_op == REDIR_DUP_OUT)
                *reserved |= 1u << cmd->redirs[j].redir_dup_fd;
        }
    }
    return 0;
}

static void close_pipes(int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < 2; j++) {
            if (pipes[i][j] >= 0) {
                (void)call(SYS_CLOSE, pipes[i][j], 0, 0);
                pipes[i][j] = -1;
            }
        }
    }
}

static long relocate(int fd, uint32_t reserved) {
    if (!(reserved & (1u << fd))) return fd;
    int min = 3;
    while (min < 32) {
        long next = call(SYS_FCNTL, fd, F_DUPFD_CLOEXEC, min);
        if (next < 0) return next;
        if (!(reserved & (1u << next))) {
            (void)call(SYS_CLOSE, fd, 0, 0);
            return next;
        }
        (void)call(SYS_CLOSE, next, 0, 0);
        min = (int)next + 1;
    }
    return SYSCALL_EMFILE;
}

static int execute_pipeline(parse_tree_t *tree, int first, int count, int status) {
    uint32_t reserved;
    int result = pipeline_preflight(tree, first, count, status, &reserved);
    if (result) return result;
    for (int i = 0; i < count - 1; i++) pipes[i][0] = pipes[i][1] = -1;
    long error = 0;
    for (int i = 0; i < count - 1; i++) {
        error = call(SYS_PIPE, (uintptr_t)pipes[i], VFS_O_CLOEXEC, 0);
        if (error < 0) break;
        for (int j = 0; j < 2; j++) {
            error = relocate(pipes[i][j], reserved);
            if (error < 0) break;
            pipes[i][j] = (int)error;
        }
        if (error < 0) break;
    }
    if (error < 0) {
        close_pipes(count);
        if (error == SYSCALL_EMFILE || error == SYSCALL_ENOMEM) return program_error(error);
        puts_err("Unable to create pipeline.\n");
        return 1;
    }

    int launched = 0;
    for (int i = 0; i < count; i++) {
        pipeline_stage_t *stage = &stages[i];
        uint32_t n = 0;
        if (i > 0) stage->actions[n++] = (spawn_fd_action_t){
            .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 0, .src_fd = pipes[i - 1][0]};
        if (i + 1 < count) stage->actions[n++] = (spawn_fd_action_t){
            .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 1, .src_fd = pipes[i][1]};
        if (stage->resolve_status) {
            result = program_error(stage->resolve_status == 127 ? SYSCALL_ENOENT : SYSCALL_ENOEXEC);
            break;
        }
        stage->pid = program_launch(stage->path, stage->argv, stage->envp,
                                     stage->actions, stage->action_count);
        if (stage->pid < 0) {
            result = program_error(stage->pid);
            break;
        }
        launched++;
    }
    /* Both success and failure close every parent copy before the first wait.
     * No cancellation API exists: failed launches require cooperative peers. */
    close_pipes(count);
    int wait_error = 0;
    int64_t child_status = 0;
    for (int i = 0; i < launched; i++) {
        int64_t collected = 0;
        if (program_wait(stages[i].pid, &collected)) wait_error = 1;
        if (i == count - 1) child_status = collected;
    }
    if (result) return result;
    return wait_error ? 1 : (int)child_status;
}

int execute_command_list(parse_tree_t *tree, int status,
                         int (*single)(parse_cmd_t *, int)) {
    enum cmd_op incoming = CMD_OP_NONE;
    for (int first = 0; first < tree->cmd_count;) {
        int last = first;
        while (last + 1 < tree->cmd_count && tree->cmds[last].next_op == CMD_OP_PIPE) last++;
        bool run = incoming == CMD_OP_NONE || incoming == CMD_OP_SEMI ||
                   (incoming == CMD_OP_AND && status == 0) ||
                   (incoming == CMD_OP_OR && status != 0);
        if (run) {
            if (first == last) status = single(&tree->cmds[first], status);
            else {
                status = execute_pipeline(tree, first, last - first + 1, status);
                if (tree->cmds[first].negate) status = status == 0 ? 1 : 0;
            }
        }
        incoming = tree->cmds[last].next_op;
        first = last + 1;
    }
    return status;
}
