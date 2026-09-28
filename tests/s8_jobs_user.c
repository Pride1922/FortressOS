/* Disposable Ring 3 S8 Phase 4 fixture. Exercises the job-launch ABI at the
 * syscall level: staged spawn + SPAWN_SETPGROUP grouping, repeated fast exits,
 * GROUP_CANCEL teardown, the terminal
 * handoff-before-release ordering, and post-cycle descriptor ownership.
 *
 * The runner injects a UART byte for the READER stage. No disk writes. */
#include "syscall_abi.h"
#include "signal_abi.h"
#include "terminal.h"

static long call4(long n, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    register uintptr_t r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return n;
}
#define call(n,a,b,c) call4(n,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),0)

static void check(bool ok, unsigned line) {
    if (ok) return;
    static const char msg[] = "S8 JOBS FAIL ";
    static char num[4];
    num[0]='0'+line/100%10; num[1]='0'+line/10%10;
    num[2]='0'+line%10; num[3]='\n';
    call(SYS_WRITE, 2, msg, sizeof(msg)-1);
    call(SYS_WRITE, 2, num, sizeof(num));
    call(SYS_EXIT, 1, 0, 0);
    __builtin_trap();
}
#define require(x) check((x), __LINE__)

static void text(const char *s) {
    size_t n = 0; while (s[n]) ++n;
    require(call(SYS_WRITE, 1, s, n) == (long)n);
}

static long waitfor(long pid, uint64_t *status, unsigned options) {
    long r;
    do { r = call(SYS_WAITPID, pid, status, options); } while (r == SYSCALL_EINTR);
    return r;
}
static void reap(long pid, unsigned sig) {
    static uint64_t status;
    status = ~0ULL;
    long got = waitfor(pid, &status, 0);
    require(got == pid);
    require(sig ? WIFSIGNALED(status) && WTERMSIG(status) == sig
                : WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Spawn /bin/shell in a given mode. pgid==0 makes this child the group leader
 * (SPAWN_SETPGROUP with reserved2==0); pgid>0 joins an existing group. */
static long spawn_staged(const char *mode, uint64_t pgid) {
    static const char *args[3];
    static spawn_opts_t opts;
    args[0] = "/bin/shell"; args[1] = mode; args[2] = 0;
    opts = (spawn_opts_t) { .size = 64, .version = 2,
                          .flags = SPAWN_SETPGROUP | SPAWN_STAGED,
                          .reserved2 = pgid, .argv = (uintptr_t)args };
    long pid = call(SYS_SPAWN_EXT, args[0], &opts, sizeof(opts));
    require(pid > 0);
    return pid;
}

/* ---- child modes ---- */
static void child(const char *mode) {
    if (mode[0] == 'x') {                 /* immediate exit */
        call(SYS_EXIT, 0, 0, 0);
    }
    if (mode[0] == 'r') {
        require(call(SYS_WRITE, 4, "S", 1) == 1);
        require(call(SYS_DUP2, 0, 31, 0) == 31);
        require(call(SYS_WRITE, 4, "B", 1) == 1);
        static char c;
        long r;
        for (int tries = 0; tries < 300; ++tries) {
            r = call(SYS_INPUT_READ, &c, 1, 10);
            if (r == 1) { require(c == 'J'); break; }
            if (r == INPUT_LOST || r == SYSCALL_EINTR || r == 0) continue;
            break;
        }
        static char tag;
        tag = (r == 1) ? 'R' : (r == 0 ? 'T' : 'N');
        require(call(SYS_WRITE, 4, &tag, 1) == 1);
        call(SYS_EXIT, 0, 0, 0);
    }
    if (mode[0] == 'n') {                 /* background reader */
        static signal_action_t ign = { .handler = SIG_IGN };
        require(call(SYS_SIGACTION, SIGTTIN, &ign, 0) == 0);
        require(call(SYS_DUP2, 0, 31, 0) == 31);
        static char c;
        long r = call(SYS_INPUT_READ, &c, 1, 10);
        /* With SIGTTIN ignored and NOT foreground, the read returns EIO (real
         * error), not INPUT_LOST. Report tag N. */
        require(r == SYSCALL_EIO);
        require(call(SYS_WRITE, 4, "N", 1) == 1);
        call(SYS_EXIT, 0, 0, 0);
    }
    if (mode[0] == 'p') {                 /* partial: read then trap */
        static char c;
        (void)call(SYS_READ, 0, &c, 1);
        call(SYS_WRITE, 4, "P", 1);
        for (;;) __asm__ volatile("pause");
    }
    call(SYS_EXIT, 0, 0, 0);
}


void shell_main(int argc, const char **argv) {
    if (argc > 1) { child(argv[1]); return; }

    long own = call(SYS_GETPGRP, 0, 0, 0);
    require(own > 0);
    /* Like the real shell and Phase 3 fixture, permit background reclaim. */
    static signal_action_t ignore_ttou = { .handler = SIG_IGN };
    require(call(SYS_SIGACTION, SIGTTOU, &ignore_ttou, 0) == 0);

    /* Reject a bad GROUP_RELEASE action up front (ABI check). */
    require(call(SYS_GROUP_RELEASE, 0, GROUP_CANCEL + 1, 0) == SYSCALL_EINVAL);

    /* ---------------- REGISTER: fast-exit after release ---------------- */
    /* A child spawned staged, released, and exiting immediately must always be
    * reaped by the parent. Repeated to stress the register/release window. */
    for (unsigned i = 0; i < 32; ++i) {
        static int fds[2];
        require(call(SYS_PIPE, fds, 0, 0) == 0 && fds[0] == 3 && fds[1] == 4);
        long pid = spawn_staged("x", 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
        require(call(SYS_GROUP_RELEASE, pid, GROUP_RELEASE, 0) == 0);
        reap(pid, 0);
    }
    text("S8 JOBS REGISTER\n");

    /* ---------------- PARTIAL: cancel tears down a multi-member group ------ */
    for (unsigned i = 0; i < 8; ++i) {
        static int lead_fds[2];
        require(call(SYS_PIPE, lead_fds, 0, 0) == 0 && lead_fds[0] == 3 && lead_fds[1] == 4);
        long lead = spawn_staged("p", 0);          /* group leader */
        static int mid_fds[2];
        require(call(SYS_PIPE, mid_fds, 0, 0) == 0 && mid_fds[0] == 5 && mid_fds[1] == 6);
        long mid = spawn_staged("p", lead);        /* joins leader's group */
        require(mid != lead);
        /* Cancel before release: the whole staged group is destroyed. Cancelled
        * members never run and produce no waitable child record, so the parent
        * must NOT waitpid for them — a wait would correctly return ECHILD. */
        require(call(SYS_GROUP_RELEASE, lead, GROUP_CANCEL, 0) == 0);
        static uint64_t st;
        require(call(SYS_WAITPID, (uintptr_t)-1, (uintptr_t)&st, WNOHANG) == SYSCALL_ECHILD);
        require(call(SYS_WAITPID, (uintptr_t)-1, (uintptr_t)&st,
                    WNOHANG | WUNTRACED | WCONTINUED) == SYSCALL_ECHILD);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
        require(call(SYS_CLOSE, 5, 0, 0) == 0);
        require(call(SYS_CLOSE, 6, 0, 0) == 0);
    }
    text("S8 JOBS PARTIAL\n");

    /* ---------------- READER: handoff ordering ---------------- */
    /* Correct order: handoff (TCSETPGRP to the child group) BEFORE release.
     * Child probes first prove execution and fd recreation; then the child
     * must receive the runner's UART byte (tag 'R'). */
    {
        require(call(SYS_FCNTL, 0, F_DUPFD_CLOEXEC, 31) == 31);
        static int fds[2];
        require(call(SYS_PIPE, fds, 0, 0) == 0 && fds[0] == 3 && fds[1] == 4);
        long child = spawn_staged("r", 0);
        require(call(SYS_TCSETPGRP, 31, child, 0) == 0);
        require(call(SYS_GROUP_RELEASE, child, GROUP_RELEASE, 0) == 0);
        text("S8 JOBS READER\n");
        static char s1, b1;
        require(call(SYS_READ, 3, &s1, 1) == 1);
        require(s1 == 'S');
        text("S8 JOBS STARTED\n");
        require(call(SYS_READ, 3, &b1, 1) == 1);
        require(b1 == 'B');
        text("S8 JOBS DUPED\n");
        static char tag;
        require(call(SYS_READ, 3, &tag, 1) == 1);
        require(tag == 'R');
        text("S8 JOBS RECEIVED\n");
        reap(child, 0);
        text("S8 JOBS REAPED\n");
        require(call(SYS_TCSETPGRP, 31, own, 0) == 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
    }

    /* Wrong order: release BEFORE handoff. The child reads while the shell
     * still owns the foreground group, so it must NOT consume the byte; it
     * reports tag 'N'. This proves the ordering is load-bearing. */
    {
        static int fds[2];
        require(call(SYS_PIPE, fds, 0, 0) == 0 && fds[0] == 3 && fds[1] == 4);
        long child = spawn_staged("n", 0);
        require(call(SYS_GROUP_RELEASE, child, GROUP_RELEASE, 0) == 0);
        /* Pipe read synchronizes with the child's completed EIO assertion. */
        static char tag;
        require(call(SYS_READ, 3, &tag, 1) == 1);
        require(tag == 'N');                       /* did NOT receive the byte */
        reap(child, 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
    }
    text("S8 JOBS NOHANDOFF\n");

    /* ---------------- FDOWN: descriptor ownership after job cycles -------- */
    require(call(SYS_TCGETPGRP, 31, 0, 0) == own);
    require(call(SYS_FCNTL, 31, F_GETFD, 0) == FD_CLOEXEC);
    for (long fd = 0; fd < 3; ++fd)
        require(call(SYS_TCGETPGRP, fd, 0, 0) == own);
    for (long fd = 3; fd < 31; ++fd) {
        static char c;
        /* Reading a closed fd returns EBADF; if anything is open here it would
         * not, so this asserts descriptors 3..30 are unused. */
        long r = call(SYS_READ, fd, &c, 1);
        require(r == SYSCALL_EBADF);
    }

    text("S8 JOBS FDOWN\n");
    text("S8 JOBS PASS\n");
    for (;;) __asm__ volatile("pause"); /* runner owns bounded teardown */
}
