/* Actual parser/expander/executor; mocked syscalls model descriptor ownership.
 * This is orchestration coverage, not evidence of blocking or SMP behavior. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pipeline.h"
#include "program.h"
#include "vars.h"

static int fds[32], flags[32], baseline[32];
static int pipes_created, launches, waits, opens, singles, fail_pipe, fail_spawn, fail_wait;
static long pipe_error, spawn_error;
static char diagnostic[1024], arguments[8][128], environments[8][512];
static int child_in[8], child_out[8], child_err[8], action_counts[8];
static int64_t statuses[8];
static parse_tree_t tree;

static void append(char *dst, const char *src) {
    memcpy(dst + strlen(dst), src, strlen(src) + 1);
}

size_t length(const char *s) { return strlen(s); }
bool equal(const char *a, const char *b) { return !strcmp(a, b); }
long puts_err(const char *s) {
    assert(strlen(diagnostic) + strlen(s) < sizeof(diagnostic));
    append(diagnostic, s);
    return (long)strlen(s);
}
void file_error_err(long error) { (void)error; }

static int alloc_fd(int minimum, int object, int cloexec) {
    for (int i = minimum; i < 32; i++) if (!fds[i]) {
        fds[i] = object;
        flags[i] = cloexec;
        return i;
    }
    return SYSCALL_EMFILE;
}

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_PIPE) {
        assert(b == VFS_O_CLOEXEC && !c);
        if (++pipes_created == fail_pipe) return pipe_error;
        int *out = (int *)a;
        int rd = alloc_fd(0, 1000 + pipes_created * 2, FD_CLOEXEC);
        if (rd < 0) return rd;
        int wr = alloc_fd(0, 1001 + pipes_created * 2, FD_CLOEXEC);
        if (wr < 0) { fds[rd] = flags[rd] = 0; return wr; }
        out[0] = rd; out[1] = wr;
        return 0;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a]); /* Includes double-close detection. */
        fds[a] = flags[a] = 0;
        return 0;
    }
    if (nr == SYS_FCNTL) {
        assert(a < 32 && fds[a] && b == F_DUPFD_CLOEXEC);
        return alloc_fd((int)c, fds[a], FD_CLOEXEC);
    }
    if (nr == SYS_STAT) {
        if (strstr((const char *)a, "missing")) return SYSCALL_ENOENT;
        ((vfs_stat_t *)b)->type = VFS_FILE;
        return 0;
    }
    if (nr == SYS_SPAWN_EXT) {
        int n = launches++;
        assert(n < 8 && waits == 0);
        if (launches == fail_spawn) return spawn_error;
        spawn_opts_t *opts = (spawn_opts_t *)b;
        assert(c == sizeof(*opts) && opts->size == c && opts->version == 1);
        const char *const *argv = (const char *const *)opts->argv;
        const char *const *envp = (const char *const *)opts->envp;
        assert(argv[0]);
        for (int j = 0; argv[j]; j++) {
            assert(strlen(arguments[n]) + strlen(argv[j]) + 2 < sizeof(arguments[n]));
            append(arguments[n], argv[j]); append(arguments[n], ":");
        }
        for (int j = 0; envp[j]; j++) {
            assert(strlen(environments[n]) + strlen(envp[j]) + 2 < sizeof(environments[n]));
            append(environments[n], envp[j]); append(environments[n], ":");
        }
        int child[32], cf[32];
        memcpy(child, fds, sizeof(child)); memcpy(cf, flags, sizeof(cf));
        spawn_fd_action_t *actions = (spawn_fd_action_t *)opts->fd_actions;
        action_counts[n] = opts->action_count;
        assert(opts->action_count <= 16);
        for (unsigned j = 0; j < opts->action_count; j++) {
            spawn_fd_action_t *act = &actions[j];
            assert(act->dst_fd >= 0 && act->dst_fd < 32);
            int dst = act->dst_fd;
            if (act->type == SPAWN_FD_ACTION_DUP2) {
                assert(act->src_fd >= 0 && act->src_fd < 32);
                if (!child[act->src_fd]) return SYSCALL_EBADF;
                child[dst] = child[act->src_fd]; cf[dst] = 0;
            } else if (act->type == SPAWN_FD_ACTION_OPEN) {
                assert(act->path && *(const char *)act->path);
                child[dst] = 100 + ++opens; cf[dst] = 0;
            } else {
                assert(act->type == SPAWN_FD_ACTION_CLOSE);
                child[dst] = cf[dst] = 0;
            }
        }
        for (int j = 0; j < 32; j++) if (cf[j]) child[j] = 0;
        for (int j = 3; j < 32; j++) assert(child[j] < 1000);
        assert(!child[31]);
        child_in[n] = child[0]; child_out[n] = child[1]; child_err[n] = child[2];
        return 100 + n;
    }
    if (nr == SYS_WAIT) {
        assert(a == (uintptr_t)(100 + waits)); /* Reap in launch order. */
        assert(!memcmp(fds, baseline, sizeof(fds))); /* All parent copies closed. */
        *(int64_t *)b = statuses[waits++];
        return waits == fail_wait ? SYSCALL_ECHILD : 0;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

static int single(parse_cmd_t *cmd, int status) {
    (void)status;
    singles++;
    int result = !strcmp(cmd->argv[0], "false");
    return cmd->negate ? !result : result;
}
static int run(const char *line) {
    assert(parser_parse(line, &tree) == PARSE_OK);
    int result = execute_command_list(&tree, 0, single);
    assert(!memcmp(fds, baseline, sizeof(fds)));
    assert(flags[31] == FD_CLOEXEC);
    return result;
}
static void reset(void) {
    memset(fds, 0, sizeof(fds)); memset(flags, 0, sizeof(flags));
    fds[0] = 1; fds[1] = 2; fds[2] = 3; fds[31] = 4; flags[31] = FD_CLOEXEC;
    memcpy(baseline, fds, sizeof(fds));
    pipes_created = launches = waits = opens = singles = 0;
    fail_pipe = fail_spawn = fail_wait = 0;
    pipe_error = SYSCALL_ENOMEM; spawn_error = SYSCALL_ENOENT;
    memset(statuses, 0, sizeof(statuses));
    memset(arguments, 0, sizeof(arguments)); memset(environments, 0, sizeof(environments));
    diagnostic[0] = 0;
    vars_init();
}

int main(void) {
    reset(); statuses[0] = 141; statuses[1] = 7;
    assert(run("/a | /b") == 7 && launches == 2 && waits == 2);
    assert(child_out[0] == child_in[1] + 1 && !diagnostic[0]);
    reset(); assert(run("/a | /b | /c | /d | /e | /f | /g | /h") == 0);
    assert(launches == 8 && waits == 8);
    reset(); assert(run("false && echo bad | /b ; true") == 0);
    assert(!launches && !pipes_created && singles == 2 && !diagnostic[0]);
    reset(); assert(run("true || /a | echo bad && false") == 1);
    assert(!launches && singles == 2);
    reset(); statuses[1] = 9;
    assert(run("! /a | /b && true") == 0 && singles == 1);
    const char *rejected[] = {"/a > /canary | 'echo' bad", "/a | > /canary",
        "X=local /a | X=value", "/a | $UNDEFINED", "/a | ! /b", "/a | \"\"",
        "/a > $UNDEFINED | /b", "/a | run /b", "/a | command /b"};
    for (unsigned i = 0; i < sizeof(rejected)/sizeof(rejected[0]); i++) {
        reset(); assert(run(rejected[i]) == 1);
        assert(!launches && !opens && !pipes_created && diagnostic[0]);
        assert(!vars_get("X"));
    }
    reset(); vars_set("X", "parent", false);
    assert(run("X=one /a \"$X\" > /one | X=two /b \"$X\" > /two") == 0);
    assert(!strcmp(arguments[0], "/a:one:") && !strcmp(arguments[1], "/b:two:"));
    assert(strstr(environments[0], "X=one:") && strstr(environments[1], "X=two:"));
    assert(!strcmp(vars_get("X"), "parent"));
    static char env[32][MAX_VAR_NAME + MAX_VAR_VAL + 2]; static const char *envp[33];
    int ec = vars_build_envp(env, envp);
    for (int i = 0; i < ec; i++) assert(strncmp(envp[i], "X=", 2));
    reset(); assert(run("/a 2>&1 > /out | /b") == 0);
    assert(child_err[0] >= 1000 && child_out[0] < 1000);
    reset(); assert(run("/a > /out 2>&1 | /b") == 0);
    assert(child_err[0] == child_out[0] && child_out[0] < 1000);
    reset(); fds[0] = fds[1] = baseline[0] = baseline[1] = 0;
    assert(run("/a | /b") == 0 && child_out[0] >= 1000 && child_in[1] >= 1000);
    reset(); assert(run("/a 3>&- | /b") == 0); /* Private handles relocated. */
    reset(); assert(run("/a 1>&3 | /b") == 1); /* Closed user fd stays closed. */
    assert(waits == 0 && strstr(diagnostic, "Bad file descriptor"));
    for (int i = 1; i <= 3; i++) {
        reset(); fail_spawn = i; statuses[0] = statuses[1] = 141;
        assert(run("/a | /b | /c") == 127 && waits == i - 1 && launches == i);
    }
    reset(); fail_spawn = 3; fail_wait = 1;
    assert(run("/a | /b | /c") == 127 && waits == 2);
    reset(); fail_wait = 1;
    assert(run("/a | /b | /c") == 1 && waits == 3);
    for (int i = 1; i <= 2; i++) {
        reset(); fail_pipe = i;
        assert(run("/a | /b | /c") == 1 && !launches && !waits);
    }
    reset(); for (int i = 3; i < 30; i++) fds[i] = baseline[i] = 5;
    assert(run("/a | /b") == 1 && !launches && strstr(diagnostic, "Too many open files"));
    reset(); fds[0] = baseline[0] = 0;
    for (int i = 3; i < 30; i++) fds[i] = baseline[i] = 5;
    assert(run("/a | /b") == 1 && !launches); /* Relocation exhaustion. */
    char line[512] = "/a | /b";
    for (int i = 0; i < 14; i++) append(line, " 2>&1");
    append(line, " | /c");
    reset(); assert(run(line) == 0 && action_counts[1] == 16);
    line[0] = 0;
    append(line, "/a | /b");
    for (int i = 0; i < 15; i++) append(line, " 2>&1");
    append(line, " | /c");
    reset(); assert(run(line) == 1 && !launches && !pipes_created);
    assert(strstr(diagnostic, "too many spawn actions"));
    reset();
    char large[MAX_VAR_VAL];
    memset(large, 'x', sizeof(large) - 1); large[sizeof(large) - 1] = 0;
    vars_set("BIG", large, false);
    line[0] = 0; append(line, "/a > /canary | /b ");
    for (int i = 0; i < 35; i++) append(line, "$BIG");
    assert(run(line) == 1);
    assert(!launches && !pipes_created && !opens && strstr(diagnostic, "Expansion exceeds"));
    reset(); vars_set("BIG", large, false);
    assert(run("/a > /canary | /b $BIG$BIG") == 1 && !launches && !opens);
    assert(strstr(diagnostic, "Argument list too long"));
    reset(); vars_set("BIG", large, true);
    assert(run("/a > /canary | /b") == 1 && !launches && !opens);
    reset();
    assert(run("/a > /canary | /b > $UNDEFINED") == 1 && !launches && !opens);
    reset(); vars_set("PATH", "/tools", false);
    assert(run("a first | b second") == 0);
    assert(!strcmp(arguments[0], "a:first:") && !strcmp(arguments[1], "b:second:"));
    reset(); assert(run("/a | missing") == 127 && launches == 1 && waits == 1);
    reset(); vars_set("CMD", "echo", false);
    assert(run("/a > /canary | \"$CMD\" nope") == 1 && !launches && !opens);
    reset(); fail_spawn = 2; spawn_error = SYSCALL_EROFS;
    assert(run("/a | /b") == 1 && waits == 1 && strstr(diagnostic, "Read-only filesystem"));
    reset(); fail_pipe = 1; pipe_error = SYSCALL_EINVAL;
    assert(run("/a | /b") == 1 && strstr(diagnostic, "Unable to create pipeline"));
    puts("PASS pipeline orchestration: grouping, preflight, lifetime, ordering, scopes and failures");
    return 0;
}
