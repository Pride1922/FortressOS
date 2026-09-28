/* S8 Phase 4 host fixture: actual parser/executor/job-table with mocked
 * syscalls. Exercises execute_command_list_job, the masked-SIGCHLD register
 * window, the staged-launch gate, foreground/background launch order, failure
 * unwinding, and job-state aggregation.
 *
 * This is orchestration coverage. It does not prove IRQ exclusion, real
 * scheduling, or kernel group-release semantics. */
#include <assert.h>
#include <string.h>   /* angle form resolves to src/include/string.h (no strcat) */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "parser.h"
#include "pipeline.h"
#include "program.h"
#include "jobs.h"
#include "jobctl.h"
#include "builtins.h"
#include "signal_abi.h"
#include "syscall_abi.h"
#include "terminal.h"

/* Events recorded by the mocked syscalls and the print stubs. Declared before
 * the stubs because puts() records EV_PRINT_LAUNCH. */
enum ev {
    EV_SIGPROCMASK_BLOCK,
    EV_SIGPROCMASK_RESTORE,
    EV_SPAWN_STAGED,       /* stage spawn, arg = stage index */
    EV_GROUP_RELEASE,
    EV_GROUP_CANCEL,
    EV_TERMATTR_GET,
    EV_TCSETPGRP_JOB,      /* handoff to job pgid */
    EV_TCSETPGRP_SHELL,    /* reclaim to shell pgid */
    EV_TERMATTR_SET,
    EV_WAITPID,
    EV_PRINT_LAUNCH,
    EV_KILL,
    EV_CLOSE_PIPES
};

/* ---- fixture stubs required at host link time ---- */
/* Recorder state is defined below with the call-sequence recorder. */
static void ev(int kind, long a);

static char diagnostic[4096];
static void dputs(const char *s) {
    size_t used = strlen(diagnostic);
    assert(used + strlen(s) < sizeof(diagnostic));
    memcpy(diagnostic + used, s, strlen(s) + 1);
}
/* Matches io.h's hosted declaration. <stdio.h> is deliberately not included so
 * this definition cannot conflict with libc's int puts(const char *).
 * jobs_print_launch() reaches this fixture only through puts/put_dec, never
 * through call(), so record its leading "[" as EV_PRINT_LAUNCH to keep launch
 * ordering observable. */
int puts(const char *s) {
    if (s[0] == '[' && s[1] == '\0') ev(EV_PRINT_LAUNCH, 0);
    dputs(s);
    fputs(s, stderr);
    return (int)strlen(s);
}
void put_dec(size_t v) {
    char b[24]; int i = 0;
    if (!v) { dputs("0"); return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i) { char c[2] = { b[--i], 0 }; dputs(c); }
}
long puts_err(const char *s) { dputs(s); return (long)strlen(s); }
void file_error(long e) { (void)e; }
void file_error_err(long e) { (void)e; }
size_t length(const char *s) { return strlen(s); }
bool equal(const char *a, const char *b) { return !strcmp(a, b); }
int shell_get_terminal_fd(void) { return 31; }

/* ---- call-sequence recording ---- */
static struct { int kind; long a; } evlog[256];
static int evn = 0;
static void ev(int kind, long a) {
    assert(evn < (int)(sizeof(evlog)/sizeof(evlog[0])));
    evlog[evn].kind = kind; evlog[evn].a = a; evn++;
}
static int ev_index(int kind) {
    for (int i = 0; i < evn; i++) if (evlog[i].kind == kind) return i;
    return -1;
}
static int ev_count(int kind) {
    int n = 0; for (int i = 0; i < evn; i++) if (evlog[i].kind == kind) n++;
    return n;
}

/* ---- syscall mock state ---- */
#define SHELL_PGID 0x5eedL         /* what the shell's SYS_GETPGRP returns */
static uint64_t cur_mask = 0;      /* signal mask the shell believes is set */
static int spawn_fail_at = -1;     /* 0-based stage index to fail, or -1 */
static long spawn_fail_err = SYSCALL_ENOENT;
static int handoff_fails = 0;
static int next_pid = 1000;
static int spawn_count = 0;

/* Scripted waitpid reports. Each entry is returned once; selector <0 = group. */
typedef struct { long pid; uint64_t status; } report_t;
static report_t reports[32]; static int rep_head = 0, rep_tail = 0;
static void push_report(long pid, uint64_t status) {
    reports[rep_tail].pid = pid; reports[rep_tail].status = status;
    rep_tail = (rep_tail + 1) % 32;
}

static int pipes_open = 0;
static bool wait_requires_resume;
static long kill_selector, kill_signal;
static terminal_attrs_t mock_attrs;

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    switch (nr) {
    case SYS_SIGPROCMASK: {
        int how = (int)a; uint64_t *set = (uint64_t *)b; uint64_t *old = (uint64_t *)c;
        if (old) *old = cur_mask;
        if (how == SIG_BLOCK)      { cur_mask |= *set; ev(EV_SIGPROCMASK_BLOCK, (long)*set); }
        else if (how == SIG_SETMASK){ cur_mask = *set; ev(EV_SIGPROCMASK_RESTORE, (long)*set); }
        return 0;
    }
    case SYS_SPAWN_EXT: {
        spawn_opts_t *o = (spawn_opts_t *)b;
        assert(o->size == sizeof(*o));
        assert(o->version == 2);
        assert(o->flags == SPAWN_V2_FLAGS);
        int idx = spawn_count++;
        ev(EV_SPAWN_STAGED, idx);
        if (idx == spawn_fail_at) return spawn_fail_err;
        return next_pid + idx;
    }
    case SYS_GROUP_RELEASE:
        if ((uint32_t)b == GROUP_RELEASE) ev(EV_GROUP_RELEASE, (long)a);
        else                              ev(EV_GROUP_CANCEL, (long)a);
        return 0;
    case SYS_TERMATTR:
        if ((int)b == TERM_GET) { ev(EV_TERMATTR_GET, 0); return 0; }
        ev(EV_TERMATTR_SET, 0); return 0;
    case SYS_TCSETPGRP:
        if (handoff_fails && (long)b != SHELL_PGID) {
            handoff_fails = 0;
            return SYSCALL_EINVAL;
        }
        ev((long)b == SHELL_PGID ? EV_TCSETPGRP_SHELL : EV_TCSETPGRP_JOB, (long)b);
        return 0;
    case SYS_GETPGRP:
        return SHELL_PGID;
    case SYS_KILL:
        kill_selector=(long)a; kill_signal=(long)b; ev(EV_KILL,(long)b);
        if (b==SIGCONT) wait_requires_resume=false;
        return 0;
    case SYS_WAITPID: {
        if (wait_requires_resume) return 0;
        if (rep_head == rep_tail) return SYSCALL_ECHILD;
        report_t r = reports[rep_head];
        rep_head = (rep_head + 1) % 32;
        if (r.pid == 0) return SYSCALL_ECHILD;
        if (r.pid > 0) *(uint64_t *)b = r.status;
        ev(EV_WAITPID, r.pid);
        return r.pid;
    }
    case SYS_PIPE:
        pipes_open++;
        ((int *)a)[0] = 10 + pipes_open * 2;
        ((int *)a)[1] = 11 + pipes_open * 2;
        return 0;
    case SYS_CLOSE:
        ev(EV_CLOSE_PIPES, 0); return 0;
    case SYS_FCNTL:
        return (long)c;
    case SYS_STAT:
        ((vfs_stat_t *)b)->type = VFS_FILE; return 0;
    default:
        assert(!"unexpected syscall in s8_jobs_host");
        return SYSCALL_ENOSYS;
    }
}
/* Production pipeline.c issues SYS_TERMATTR through a real `syscall`
 * instruction; on a Linux host that is syscall 34 = pause(2), which would block
 * the foreground path forever. The runner compiles with
 * -DSHELL_TERMATTR_HOST_TEST, so pipeline.c calls this host-safe stub instead
 * (see the call4 seam in pipeline.c). Terminal attribute save/restore is not
 * part of Phase 4's launch contract, so it is recorded as a no-op. */
long shell_termattr_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    assert(nr==SYS_TERMATTR && a==31 && d==sizeof(terminal_attrs_t));
    if (b==TERM_GET) { *(terminal_attrs_t *)c=mock_attrs; ev(EV_TERMATTR_GET,0); }
    else { mock_attrs=*(terminal_attrs_t *)c; ev(EV_TERMATTR_SET,mock_attrs.input_flags); }
    return 0;
}

/* ---- helper: reset per-case state ---- */
static void reset(void) {
    memset(diagnostic, 0, sizeof(diagnostic));
    evn = 0; cur_mask = 0; spawn_fail_at = -1; spawn_count = 0;
    handoff_fails = 0; rep_head = rep_tail = 0; pipes_open = 0;
    wait_requires_resume=false; kill_selector=kill_signal=0;
    mock_attrs=(terminal_attrs_t){.version=1,.input_flags=TERM_ISIG,.vintr=3,.vsusp=26};
    jobs_init();
}

/* ---- simple exec callback ---- */
static int single(parse_cmd_t *cmd, int status) {
    (void)cmd; (void)status; return 0;
}

/* ================= TESTS ================= */

static void test_parser_amp(void) {
    parse_tree_t t;
    assert(parser_parse("a &", &t) == PARSE_OK && t.background && t.cmd_count == 1);
    assert(parser_parse("a | b &", &t) == PARSE_OK && t.background && t.cmd_count == 2);
    assert(parser_parse("a & b", &t) == PARSE_SYNTAX_ERROR);
    assert(parser_parse("&", &t) == PARSE_SYNTAX_ERROR);
    puts("PASS parser: & on pipeline, rejection of a & b, rejection of bare &\n");
}

static void test_job_table(void) {
    reset();
    int slots[MAX_JOBS];
    for (int i = 0; i < MAX_JOBS; i++) { slots[i] = jobs_alloc(); assert(slots[i] >= 0); }
    assert(jobs_alloc() == -1);
    jobs_free(slots[3]);
    int reused = jobs_alloc();
    assert(reused == slots[3]);
    jobs_set_pgid(reused, 4242);
    assert(jobs_find_by_pgid(4242) == reused);
    jobs_add_member(reused, 777);
    assert(jobs_find_by_pid(777) == reused);
    puts("PASS jobs: allocation, capacity, reuse, find_by_pgid/pid\n");
}

static void test_state_machine(void) {
    reset();
    int s = jobs_alloc();
    jobs_add_member(s, 10); jobs_add_member(s, 11);
    /* one stopped: job still running */
    jobs_update_member(10, 0x7f);            /* WIFSTOPPED */
    assert(jobs_get(s)->state == JOB_STATE_RUNNING);
    /* second stopped: job stopped */
    jobs_update_member(11, 0x7f);
    assert(jobs_get(s)->state == JOB_STATE_STOPPED);
    /* continue both: running */
    jobs_update_member(10, 0xffff);          /* WIFCONTINUED */
    assert(jobs_get(s)->state == JOB_STATE_RUNNING);
    /* exit both: done, last stage status captured */
    jobs_update_member(10, (0 << 8));        /* exited 0 */
    jobs_update_member(11, (7 << 8));        /* exited 7 */
    assert(jobs_get(s)->state == JOB_STATE_DONE);
    assert(jobs_get(s)->last_status == 7);
    puts("PASS jobs: running/stopped/done aggregation, last-stage status\n");
}

static void test_background_launch_order(void) {
    reset();
    parse_tree_t t;
    assert(parser_parse("a | b &", &t) == PARSE_OK);
    int r = execute_command_list_job(&t, 0, single, "a | b &");
    assert(r == 0);
    /* order: BLOCK -> SPAWN(0) -> SPAWN(1) -> RELEASE -> RESTORE -> PRINT */
    int b = ev_index(EV_SIGPROCMASK_BLOCK);
    int sp0 = ev_index(EV_SPAWN_STAGED);
    int rel = ev_index(EV_GROUP_RELEASE);
    int rst = ev_index(EV_SIGPROCMASK_RESTORE);
    int prn = ev_index(EV_PRINT_LAUNCH);
    assert(b >= 0 && sp0 > b && rel > sp0 && rst > rel && prn > rst);
    assert(ev_count(EV_GROUP_RELEASE) == 1);
    assert(ev_count(EV_GROUP_CANCEL) == 0);
    assert(strstr(diagnostic, "[") && strstr(diagnostic, "] "));
    puts("PASS launch: background order BLOCK/SPAWN/RELEASE/RESTORE/PRINT\n");
}

static void test_foreground_launch_order(void) {
    reset();
    parse_tree_t t;
    assert(parser_parse("a | b", &t) == PARSE_OK);
    push_report(1000, (0 << 8));
    push_report(1001, (0 << 8));
    push_report(0, 0);                        /* ECHILD terminates wait loop */
    int r = execute_command_list_job(&t, 0, single, "a | b");
    assert(r == 0);
    /* Order (TERMATTR GET/SET are routed to the fixture's host-safe stub via
     * SHELL_TERMATTR_HOST_TEST, so they are not observable here and are not
     * Phase 4's contract):
     *   BLOCK -> SPAWN(0) -> SPAWN(1) -> TCSETPGRP(job)
     *         -> RELEASE -> RESTORE -> ... -> TCSETPGRP(shell) */
    int b   = ev_index(EV_SIGPROCMASK_BLOCK);
    int sp0 = ev_index(EV_SPAWN_STAGED);
    int ho  = ev_index(EV_TCSETPGRP_JOB);
    int rel = ev_index(EV_GROUP_RELEASE);
    int rst = ev_index(EV_SIGPROCMASK_RESTORE);
    int rc  = ev_index(EV_TCSETPGRP_SHELL);
    assert(b >= 0 && sp0 > b);
    assert(ho > sp0);                 /* handoff happens after staging */
    assert(rel > ho);                 /* handoff BEFORE release (critical) */
    assert(rst > rel);                /* unblock AFTER release */
    assert(rc > rst);                 /* reclaim after the wait */
    assert(ev_count(EV_GROUP_CANCEL) == 0);
    puts("PASS launch: foreground handoff-before-release, reclaim order\n");
}

static void test_masked_register_window(void) {
    reset();
    parse_tree_t t;
    assert(parser_parse("a | b &", &t) == PARSE_OK);
    int r = execute_command_list_job(&t, 0, single, "a | b &");
    assert(r == 0);
    int b = ev_index(EV_SIGPROCMASK_BLOCK), rst = ev_index(EV_SIGPROCMASK_RESTORE);
    assert(b >= 0 && rst > b);
    puts("PASS launch: SIGCHLD blocked across register window, restored after release\n");
}

static void test_spawn_failure_unwinding(void) {
    for (int fail = 0; fail < 3; fail++) {
        reset();
        spawn_fail_at = fail;
        parse_tree_t t;
        assert(parser_parse("a | b | c &", &t) == PARSE_OK);
        int r = execute_command_list_job(&t, 0, single, "a | b | c &");
        assert(r != 0);
        assert(ev_count(EV_GROUP_CANCEL) == (fail == 0 ? 0 : 1));
        assert(ev_count(EV_SIGPROCMASK_RESTORE) == 1);
        int s = jobs_alloc(); assert(s >= 0);
    }
    puts("PASS launch: spawn failure unwinds with GROUP_CANCEL + mask restore + free\n");
}

static void test_handoff_failure_unwinding(void) {
    reset();
    handoff_fails = 1;
    parse_tree_t t;
    assert(parser_parse("a | b", &t) == PARSE_OK);
    int r = execute_command_list_job(&t, 0, single, "a | b");
    assert(r != 0);
    assert(ev_count(EV_GROUP_CANCEL) == 1);
    assert(ev_count(EV_SIGPROCMASK_RESTORE) == 1);
    assert(strstr(diagnostic, "terminal handoff failed"));
    int s = jobs_alloc(); assert(s >= 0);
    puts("PASS launch: terminal handoff failure cancels group, restores mask, frees\n");
}

static void test_full_table(void) {
    reset();
    for (int i = 0; i < MAX_JOBS; i++) assert(jobs_alloc() >= 0);
    parse_tree_t t;
    assert(parser_parse("a &", &t) == PARSE_OK);
    int r = execute_command_list_job(&t, 0, single, "a &");
    assert(r == 1);
    assert(strstr(diagnostic, "job table full"));
    assert(ev_count(EV_SPAWN_STAGED) == 0);
    puts("PASS launch: full table rejects with diagnostic, no spawn\n");
}

/* ---- additional coverage: & edge cases, reaping notification, bounds ---- */

static void test_background_then_and(void) {
    parse_tree_t t;
    /* A dangling && is incomplete, not a syntax error: the lexer reads &&
     * as one token, and an operator with no following command means "ask for
     * continuation input" (status LEX_INCOMPLETE_OP). Same for "& &&". */
    assert(parser_parse("a | b &&", &t) == PARSE_INCOMPLETE);
    assert(parser_parse("a & &&", &t) == PARSE_INCOMPLETE);
    /* & followed by a pipe is a syntax error. */
    assert(parser_parse("a & | b", &t) == PARSE_SYNTAX_ERROR);
    puts("PASS parser: dangling && incomplete; & | b syntax error\n");
}

static void test_background_then_semi_foreground(void) {
    reset();
    parse_tree_t t;
    assert(parser_parse("a & ; b", &t) == PARSE_OK);
    int r = execute_command_list_job(&t, 0, single, "a & ; b");
    assert(r == 0);
    assert(ev_count(EV_GROUP_RELEASE) == 1);
    assert(ev_count(EV_GROUP_CANCEL) == 0);
    assert(ev_count(EV_SPAWN_STAGED) == 1);
    puts("PASS launch: a & ; b backgrounds a and runs b foreground\n");
}

static void test_reap_notification(void) {
    reset();
    int s = jobs_alloc();
    assert(s >= 0);
    jobs_set_cmd(s, "sleep 1 &");
    jobs_set_foreground(s, false);
    jobs_add_member(s, 4242);
    jobs_set_pgid(s, 4242);
    push_report(4242, (0 << 8));
    push_report(0, 0);
    jobs_reap_children();
    assert(jobs_get(s)->state == JOB_STATE_DONE);
    assert(strstr(diagnostic, "Done"));
    assert(strstr(diagnostic, "["));
    puts("PASS jobs: reap prints Done notification for background job\n");
}

static void test_foreground_no_launch_print(void) {
    reset();
    parse_tree_t t;
    assert(parser_parse("a | b", &t) == PARSE_OK);
    push_report(1000, (0 << 8));
    push_report(1001, (0 << 8));
    push_report(0, 0);
    int r = execute_command_list_job(&t, 0, single, "a | b");
    assert(r == 0);
    assert(ev_count(EV_PRINT_LAUNCH) == 0);
    puts("PASS launch: foreground pipeline emits no [N] launch line\n");
}

static void test_cmd_text_bounds(void) {
    reset();
    int s = jobs_alloc();
    assert(s >= 0);
    char big[200];
    memset(big, 0x78, sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    jobs_set_cmd(s, big);
    const job_t *j = jobs_get(s);
    assert(strlen(j->cmd_text) == MAX_JOB_CMD_LEN - 1);
    assert(j->cmd_text[MAX_JOB_CMD_LEN - 1] == 0);
    puts("PASS jobs: command text truncated to bound and NUL-terminated\n");
}

static void test_prompt_reap(void) {
    reset();
    assert(!jobs_reap_prompt());
    assert(diagnostic[0] == 0); /* Empty timeout scans stay quiet. */
    int slot = jobs_alloc();
    jobs_add_member(slot, 4242);
    jobs_set_cmd(slot, "idle-delay &");
    push_report(SYSCALL_EINTR, 0);
    push_report(4242, 0);
    assert(jobs_reap_prompt());
    assert(diagnostic[0] == '\n');
    assert(strstr(diagnostic, "Done"));
    assert(!jobs_get(slot)->in_use); /* Idle GC frees notified metadata too. */
    size_t n = strlen(diagnostic);
    assert(!jobs_reap_prompt());
    assert(strlen(diagnostic) == n); /* No repeated notification/repaint. */
    assert(cur_mask == 0); /* Passive drain never changes signal masks. */
    puts("PASS jobs: prompt drain retries EINTR, separates output, GC, quiet rescan\n");
}

static int make_job(long pid) {
    int slot=jobs_alloc(); assert(slot>=0);
    jobs_set_pgid(slot,pid); jobs_add_member(slot,pid);
    jobs_set_cmd(slot,"worker"); jobs_set_foreground(slot,false);
    return slot;
}
static void test_jobctl(void) {
    reset();
    int a=make_job(40), b=make_job(41);
    assert(jobs_resolve("%1")==a && jobs_resolve("%2")==b);
    assert(jobs_resolve("%+")==b && jobs_resolve("%-")==a && jobs_resolve("%%")==b);
    const char *invalid[]={"%","%0","%10000","%999999999999999999999","%x","1","%+x"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) assert(jobs_resolve(invalid[i])==-2);
    assert(jobs_resolve("%99")==-1);
    jobs_update_member(40,(SIGTSTP<<8)|0x7f);
    assert(jobs_resolve(NULL)==a && jobs_resolve("%-")==b); /* stopped priority */
    const char *list[]={"jobs"}; assert(!jobctl_exec(1,list));
    assert(strstr(diagnostic,"[1]+ Stopped") && strstr(diagnostic,"[2]- Running"));
    const char *bg[]={"bg"}; assert(!jobctl_exec(1,bg));
    assert(kill_selector==-40 && kill_signal==SIGCONT && ev_count(EV_TCSETPGRP_JOB)==0);
    assert(jobs_get(a)->state==JOB_STATE_RUNNING && jobs_resolve(NULL)==a);
    const char *kill[]={"kill","%-","KILL"}; assert(!jobctl_exec(3,kill));
    assert(kill_selector==-41 && kill_signal==SIGKILL);
    const char *bad[]={"fg","%99"}; assert(jobctl_exec(2,bad)==1);
    kill[2]="999999"; assert(jobctl_exec(3,kill)==1);
    kill[2]="SIGTERM"; assert(!jobctl_exec(3,kill) && kill_signal==SIGTERM);
    assert(!jobctl_exec(2,kill) && kill_signal==SIGTERM);
    assert(!builtin_is_child_safe("jobs") && !builtin_is_child_safe("fg") &&
           !builtin_is_child_safe("bg") && !builtin_is_child_safe("kill"));
    jobs_free(a); assert(jobs_resolve(NULL)==b && jobs_resolve("%1")==-1);
    /* Reusing a middle slot in a full table must not evict the oldest job
     * from current/previous selection when the newer entries are removed. */
    reset();
    int slots[MAX_JOBS];
    for (int i=0;i<MAX_JOBS;++i) slots[i]=make_job(100+i);
    jobs_free(slots[3]);
    int reused=make_job(200);
    jobs_free(reused);
    for (int i=1;i<MAX_JOBS;++i) if (i!=3) jobs_free(slots[i]);
    assert(jobs_resolve(NULL)==slots[0]);
    puts("PASS jobctl: specs, current/previous, listing, bg/group kill, rejection\n");
}
static void test_fg_shared_controller(void) {
    reset();
    int slot=make_job(40); jobs_add_member(slot,41);
    jobs_update_member(40,(SIGTSTP<<8)|0x7f);
    assert(jobs_get(slot)->state==JOB_STATE_RUNNING); /* all-member rule */
    jobs_update_member(41,(SIGTSTP<<8)|0x7f);
    terminal_attrs_t attrs=mock_attrs; attrs.input_flags=0;
    jobs_save_attrs(slot,&attrs);
    wait_requires_resume=true;
    push_report(40,0); push_report(41,7<<8);
    const char *fg[]={"fg"};
    assert(jobctl_exec(1,fg)==7);
    assert(!jobs_get(slot)->in_use && mock_attrs.input_flags==TERM_ISIG);
    assert(ev_index(EV_TCSETPGRP_JOB)<ev_index(EV_TERMATTR_SET));
    assert(ev_index(EV_TERMATTR_SET)<ev_index(EV_KILL));
    assert(ev_index(EV_KILL)<ev_index(EV_WAITPID));
    assert(ev_index(EV_WAITPID)<ev_index(EV_TCSETPGRP_SHELL));
    assert(ev_count(EV_GROUP_RELEASE)==0 && ev_count(EV_SIGPROCMASK_BLOCK)==0);
    reset(); slot=make_job(40); handoff_fails=1;
    assert(pipeline_foreground(slot,false,0)==1 && jobs_get(slot)->in_use);
    assert(ev_count(EV_KILL)==0 && ev_count(EV_GROUP_CANCEL)==0);
    reset(); slot=make_job(40);
    jobs_update_member(40,137<<8); jobs_print_state(slot,false);
    assert(strstr(diagnostic,"Done(137)") && !strstr(diagnostic,"Terminated"));
    reset(); slot=make_job(40);
    jobs_update_member(40,SIGKILL); jobs_print_state(slot,false);
    assert(strstr(diagnostic,"Terminated(signal 9)"));
    puts("PASS fg: shared handoff/attrs/CONT/wait/reclaim, error and signal status\n");
}

int main(void) {
    test_parser_amp();
    test_job_table();
    test_state_machine();
    test_background_launch_order();
    test_foreground_launch_order();
    test_masked_register_window();
    test_spawn_failure_unwinding();
    test_handoff_failure_unwinding();
    test_full_table();
    test_background_then_and();
    test_background_then_semi_foreground();
    test_reap_notification();
    test_foreground_no_launch_print();
    test_cmd_text_bounds();
    test_prompt_reap();
    test_jobctl();
    test_fg_shared_controller();
    puts("PASS s8-jobs-host: parser, job table, state machine, launch order, mask, unwinding\n");
    return 0;
}
