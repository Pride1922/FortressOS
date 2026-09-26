/* Ring 3 resource exerciser, packaged only in the resource runner's test ISO. */
#include "syscall_abi.h"
#include "vfs.h"

extern void s6_resource_checkpoint(uint64_t phase, uint64_t cycle);
static const char path[] = "/mnt/s6_resources.txt";
static const char missing[] = "/mnt/s6_missing_directory/out";
static const char *const normal_argv[] = {"/bin/dual_stream", NULL};
static const char *const fault_argv[] = {"/bin/s6_resources", "fault", NULL};
static spawn_fd_action_t actions[5];
static spawn_opts_t opts;

static long invoke(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static void require_at(bool ok, unsigned line) {
    if (!ok) {
        static const char message[] = "S6 RESOURCE USER FAIL\n";
        invoke(SYS_WRITE, 2, (uintptr_t)message, sizeof(message) - 1);
        char number[] = {'0' + (line / 100) % 10, '0' + (line / 10) % 10,
                         '0' + line % 10, '\n'};
        invoke(SYS_WRITE, 2, (uintptr_t)number, sizeof(number));
        invoke(SYS_EXIT, 1, 0, 0);
        __builtin_trap();
    }
}
#define require(ok) require_at((ok), __LINE__)

void shell_main(int argc, const char **argv) {
    (void)argv;
    if (argc > 1) __builtin_trap(); /* Exercise fault/exit/reap, not just SYS_EXIT. */
    long fd = invoke(SYS_OPEN, (uintptr_t)path,
                     VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND, 0644);
    require(fd == 3);
    require(invoke(SYS_DUP, fd, 0, 0) == 4);
    /* Allocate the data block before baseline measurement. */
    require(invoke(SYS_WRITE, fd, (uintptr_t)"seed\n", 5) == 5);

    opts.size = sizeof(opts);
    opts.version = 1;
    opts.fd_actions = (uintptr_t)actions;
    /* One warm-up round followed by three measured rounds, four cases each. */
    for (uint64_t cycle = 0; cycle < 16; cycle++) {
        unsigned kind = cycle % 4;
        actions[0] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_OPEN,
            .dst_fd = 5, .flags = VFS_O_RDONLY, .path = (uintptr_t)path};
        actions[1] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_DUP2,
            .dst_fd = 6, .src_fd = 3};
        actions[2] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_CLOSE, .dst_fd = 4};
        actions[3] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_DUP2,
            .dst_fd = 1, .src_fd = 3};
        actions[4] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_DUP2,
            .dst_fd = 2, .src_fd = 3};
        if (kind == 1) actions[4].src_fd = 30; /* Valid number, unopened source. */
        if (kind == 2) actions[4] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_OPEN,
            .dst_fd = 7, .flags = VFS_O_WRONLY, .path = (uintptr_t)missing};
        opts.action_count = 5;
        opts.argv = (uintptr_t)(kind == 3 ? fault_argv : normal_argv);

        s6_resource_checkpoint(0, cycle);
        long pid = invoke(SYS_SPAWN_EXT, (uintptr_t)(kind == 3 ? fault_argv[0] : normal_argv[0]),
                          (uintptr_t)&opts, sizeof(opts));
        if (kind == 1) require(pid == SYSCALL_EBADF);
        else if (kind == 2) require(pid == SYSCALL_ENOENT);
        else {
            int64_t status = -1;
            require(pid > 0);
            require(invoke(SYS_WAIT, pid, (uintptr_t)&status, 0) == 0);
            require(status == (kind == 3 ? 134 : 0)); /* #UD = vector 6 */
        }
        s6_resource_checkpoint(1, cycle);
    }
    require(invoke(SYS_CLOSE, 4, 0, 0) == 0);
    require(invoke(SYS_CLOSE, 3, 0, 0) == 0);
    static const char done[] = "S6 RESOURCE USER PASS\n";
    invoke(SYS_WRITE, 1, (uintptr_t)done, sizeof(done) - 1);
}
