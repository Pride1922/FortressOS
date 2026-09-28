#include "pipeline.h"
#include "program.h"
#include "expand.h"
#include "vars.h"
#include "redir.h"
#include "builtins.h"
#include "terminal.h"

#define RUNNER_PATH "/bin/sh-builtin"

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
    bool is_runner; /* True when dispatched via /bin/sh-builtin */
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

    /* Classify stage: empty, forbidden builtin, child-safe builtin, or external. */
    if (!expanded.argc || !expanded.argv[0][0]) {
        puts_err("pipeline: unsupported stage\n");
        return 1;
    }
    bool is_builtin = builtin_find(expanded.argv[0]) != CMD_UNKNOWN;
    bool is_safe    = builtin_is_child_safe(expanded.argv[0]);
    if (is_builtin && !is_safe) {
        puts_err("pipeline: builtin '");
        puts_err(expanded.argv[0]);
        puts_err("' cannot run as a pipeline stage\n");
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

    /* Checked envp snapshot: fail preflight on overflow rather than truncating. */
    int envc = vars_build_envp_checked(stage->env, stage->envp);
    if (envc < 0) {
        puts_err("pipeline: exported environment exceeds 32-entry limit\n");
        return 1;
    }
    size_t env_used = 0;
    for (int i = 0; i < envc; i++) {
        size_t n = length(stage->envp[i]) + 1;
        if (n > MAX_ENV_STRLEN || n > MAX_TOTAL_ENVP_LEN - env_used)
            return program_error(SYSCALL_E2BIG);
        env_used += n;
    }

    stage->is_runner = is_safe;
    if (is_safe) {
        /* Child-safe builtins run via the fixed runner path; skip PATH resolution. */
        const char *rpath = RUNNER_PATH;
        size_t rlen = length(rpath);
        for (size_t i = 0; i <= rlen; i++) stage->path[i] = rpath[i];
        stage->resolve_status = 0;
    } else {
        /* External program: resolve via PATH, fail at launch position. */
        stage->resolve_status = program_resolve(stage->argv[0], stage->path);
    }

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
            /* Emit a specific diagnostic when the builtin runner binary is missing. */
            if (stage->is_runner && stage->pid == SYSCALL_ENOENT) {
                puts_err("pipeline: builtin dispatcher missing (" RUNNER_PATH ")\n");
                result = 127;
            } else {
                result = program_error(stage->pid);
            }
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

/* ---- Job-aware pipeline launch (Phase 4) ---- */

#include "jobs.h"
#include "ui.h"

/* 4-argument syscall wrapper for SYS_TERMATTR: it needs R10, so it bypasses the
 * 3-argument call() shim. This is a real `syscall` instruction, and on a Linux
 * host syscall 34 is pause(2), which blocks. Host fixtures therefore compile
 * this file with SHELL_TERMATTR_HOST_TEST and supply shell_termattr_call()
 * (the same seam io.c uses with SHELL_IO_HOST_TEST around call()). */
#ifdef SHELL_TERMATTR_HOST_TEST
extern long shell_termattr_call(long nr, uintptr_t a, uintptr_t b,
                                uintptr_t c, uintptr_t d);
#define call4 shell_termattr_call
#else
static long call4(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    register uintptr_t r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif

/* BSS-static terminal attribute save area for foreground job handoff. */
static terminal_attrs_t s_saved_attrs;

static int execute_pipeline_job(parse_tree_t *tree, int first, int count,
                                int status, bool background, const char *cmd_text) {
    /* 1. Reserve job slot before any spawn. */
    int slot = jobs_alloc();
    if (slot < 0) {
        puts_err("fortress: job table full\n");
        return 1;
    }
    jobs_set_cmd(slot, cmd_text ? cmd_text : "");
    jobs_set_foreground(slot, !background);

    /* 2. Block SIGCHLD while registering members (masked register). */
    uint64_t old_mask = 0;
    uint64_t chld_mask = SIGNAL_BIT(SIGCHLD);
    (void)call(SYS_SIGPROCMASK, SIG_BLOCK, (uintptr_t)&chld_mask, (uintptr_t)&old_mask);

    /* 3. Preflight: expand, resolve, build fd actions. */
    uint32_t reserved;
    int result = pipeline_preflight(tree, first, count, status, &reserved);
    if (result) {
        (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);
        jobs_free(slot);
        return result;
    }

    /* 4. Create pipes. */
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
        (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);
        jobs_free(slot);
        if (error == SYSCALL_EMFILE || error == SYSCALL_ENOMEM)
            return program_error(error);
        puts_err("Unable to create pipeline.\n");
        return 1;
    }

    /* 5. Spawn all stages as STAGED with SETPGROUP. */
    int launched = 0;
    long pgid = 0;
    result = 0;

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
        stage->pid = program_launch_job(stage->path, stage->argv, stage->envp,
                                        stage->actions, stage->action_count, pgid);
        if (stage->pid < 0) {
            if (stage->is_runner && stage->pid == SYSCALL_ENOENT) {
                puts_err("pipeline: builtin dispatcher missing (" RUNNER_PATH ")\n");
                result = 127;
            } else {
                result = program_error(stage->pid);
            }
            break;
        }
        if (i == 0) pgid = stage->pid;  /* First child's PID = new group's PGID */
        jobs_add_member(slot, stage->pid);
        launched++;
    }

    /* Close parent pipe copies immediately (before wait or return). */
    close_pipes(count);

    /* 6. Handle launch failure: cancel all staged members. */
    if (result) {
        if (pgid > 0) {
            (void)call(SYS_GROUP_RELEASE, (uintptr_t)pgid, GROUP_CANCEL, 0);
            /* Reap cancelled members. */
            for (int i = 0; i < launched; i++) {
                uint64_t ws = 0;
                (void)program_waitpid(stages[i].pid, &ws, 0);
            }
        }
        (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);
        jobs_free(slot);
        return result;
    }

    jobs_set_pgid(slot, pgid);

    if (background) {
        /* 7a. Background: release gate, unblock SIGCHLD, print [N] PID. */
        (void)call(SYS_GROUP_RELEASE, (uintptr_t)pgid, GROUP_RELEASE, 0);
        (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);
        jobs_print_launch(slot);
        return 0;
    }

    /* 7b. Foreground: save terminal attrs, hand terminal to job, release, wait. */
    int term_fd = shell_get_terminal_fd();
    (void)call4(SYS_TERMATTR, (uintptr_t)term_fd, TERM_GET,
                (uintptr_t)&s_saved_attrs, sizeof(s_saved_attrs));

    long handoff = call(SYS_TCSETPGRP, (uintptr_t)term_fd, (uintptr_t)pgid, 0);
    if (handoff < 0) {
        /* Terminal handoff failed: cancel everything. */
        (void)call(SYS_GROUP_RELEASE, (uintptr_t)pgid, GROUP_CANCEL, 0);
        for (int i = 0; i < launched; i++) {
            uint64_t ws = 0;
            (void)program_waitpid(stages[i].pid, &ws, 0);
        }
        (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);
        jobs_free(slot);
        puts_err("fortress: terminal handoff failed\n");
        return 1;
    }

    /* Release the staged gate. */
    (void)call(SYS_GROUP_RELEASE, (uintptr_t)pgid, GROUP_RELEASE, 0);

    /* Unblock SIGCHLD now that registration is complete. */
    (void)call(SYS_SIGPROCMASK, SIG_SETMASK, (uintptr_t)&old_mask, 0);

    /* 8. Wait for the job (foreground wait loop). */
    int fg_status = 0;
    bool job_stopped = false;

    for (;;) {
        uint64_t ws = 0;
        int pid = program_waitpid(-(int)pgid, &ws, WUNTRACED | WCONTINUED);
        if (pid <= 0) {
            if (pid == (int)SYSCALL_ECHILD) break;  /* All children collected */
            continue;
        }

        (void)jobs_update_member(pid, ws);

        const job_t *job = jobs_get(slot);
        if (!job) break;

        if (job->state == JOB_STATE_DONE) {
            fg_status = job->last_status;
            break;
        }
        if (job->state == JOB_STATE_STOPPED) {
            /* Job suspended. Shell reclaims terminal and returns. */
            job_stopped = true;
            break;
        }
        /* Otherwise keep waiting (some members still running). */
    }

    /* 9. Reclaim terminal and restore attributes. */
    long shell_pgid = call(SYS_GETPGRP, 0, 0, 0);
    (void)call(SYS_TCSETPGRP, (uintptr_t)term_fd, (uintptr_t)shell_pgid, 0);
    (void)call4(SYS_TERMATTR, (uintptr_t)term_fd, TERM_SET,
                (uintptr_t)&s_saved_attrs, sizeof(s_saved_attrs));

    if (job_stopped) {
        /* Move to background as stopped — Phase 5 will handle fg/bg. */
        jobs_set_foreground(slot, false);
        puts("[");
        put_dec((size_t)jobs_get(slot)->job_id);
        puts("]  Stopped                 ");
        puts(cmd_text ? cmd_text : "");
        puts("\n");
        return 128 + SIGTSTP;  /* Shell status convention for suspended job */
    }

    /* Done: mark notified and free after displaying if needed. */
    jobs_free(slot);
    return fg_status;
}

int execute_command_list_job(parse_tree_t *tree, int status,
                             int (*single)(parse_cmd_t *, int),
                             const char *cmd_text) {
    bool background = tree->background;
    enum cmd_op incoming = CMD_OP_NONE;

    for (int first = 0; first < tree->cmd_count;) {
        int last = first;
        while (last + 1 < tree->cmd_count && tree->cmds[last].next_op == CMD_OP_PIPE) last++;

        /* The & applies to the last pipeline in the command list. Check for
         * CMD_OP_BG marker: if the last command's next_op is BG, this pipeline
         * is the backgrounded one. */
        bool is_bg_pipeline = (tree->cmds[last].next_op == CMD_OP_BG) && background;

        bool run = incoming == CMD_OP_NONE || incoming == CMD_OP_SEMI ||
                   (incoming == CMD_OP_AND && status == 0) ||
                   (incoming == CMD_OP_OR && status != 0);

        if (run) {
            int pipeline_count = last - first + 1;

            if (is_bg_pipeline) {
                /* Background: route through job-aware launcher. */
                status = execute_pipeline_job(tree, first, pipeline_count,
                                             status, true, cmd_text);
                if (tree->cmds[first].negate) status = status == 0 ? 1 : 0;
            } else if (pipeline_count > 1) {
                /* Foreground multi-stage pipeline: use job-aware launcher
                 * for proper group creation and terminal handoff. */
                status = execute_pipeline_job(tree, first, pipeline_count,
                                             status, false, cmd_text);
                if (tree->cmds[first].negate) status = status == 0 ? 1 : 0;
            } else {
                /* Single foreground command: fast path via callback. */
                status = single(&tree->cmds[first], status);
            }
        }
        incoming = tree->cmds[last].next_op;
        if (incoming == CMD_OP_BG) incoming = CMD_OP_NONE;  /* BG terminates */
        first = last + 1;
    }
    return status;
}
