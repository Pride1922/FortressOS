/* Test-ISO-only Ring 3 fixture. Four rounds of four named boundaries.
 * The host deduplicates aliased addresses; alias cases still roundtrip. */
#include "syscall_abi.h"

volatile uint64_t nmi_caught;
extern void nmi_handler(void);
extern void nmi_self(void);
extern void nmi_timer(void);

static long invoke(long n, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return n;
}
static void require(bool ok) {
    if (ok) return;
    static const char fail[] = "S8 NMI FIXTURE FAIL\n";
    invoke(SYS_WRITE, 2, (uintptr_t)fail, sizeof(fail)-1);
    invoke(SYS_EXIT, 1, 0, 0);
    __builtin_trap();
}
void shell_main(int argc, const char **argv) {
    (void)argv;
    signal_action_t action = {.handler = (uintptr_t)nmi_handler};
    require(invoke(SYS_SIGACTION, SIGTERM, (uintptr_t)&action, 0) == 0);
    if (argc > 1) {
        require(invoke(SYS_WRITE, 4, (uintptr_t)"R", 1) == 1);
        nmi_timer();
        require(nmi_caught == 1);
        return;
    }
    int fd[2];
    require(invoke(SYS_PIPE, (uintptr_t)fd, 0, 0) == 0 && fd[0] == 3 && fd[1] == 4);
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned boundary = 0; boundary < 4; ++boundary) {
            nmi_caught = 0;
            nmi_self();
            require(nmi_caught == 1);
            const char *args[] = {"/bin/shell", "timer", 0};
            spawn_opts_t opts = {.size = 64, .version = 2,
                .flags = SPAWN_SETPGROUP, .argv = (uintptr_t)args};
            long pid = invoke(SYS_SPAWN_EXT, (uintptr_t)args[0], (uintptr_t)&opts, 64);
            require(pid > 0);
            char ready;
            require(invoke(SYS_READ, 3, (uintptr_t)&ready, 1) == 1 && ready == 'R');
            /* Let the child leave its ready syscall and enter the user loop. */
            require(invoke(SYS_INPUT_READ, (uintptr_t)&ready, 1, 100) == 0);
            require(invoke(SYS_KILL, pid, SIGTERM, 0) == 0);
            uint64_t status = ~0ULL;
            require(invoke(SYS_WAITPID, pid, (uintptr_t)&status, 0) == pid && status == 0);
        }
    }
    static const char pass[] = "S8 NMI FIXTURE PASS\n";
    invoke(SYS_WRITE, 1, (uintptr_t)pass, sizeof(pass)-1);
}
