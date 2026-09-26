/* Ring 3 pipe/spawn probe, installed as /bin/shell only in a disposable ISO. */
#include "syscall_abi.h"
#include "vfs.h"

static unsigned char pages[8192] __attribute__((aligned(4096)));
static const unsigned char readonly[8192] __attribute__((aligned(4096))) = {1};
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
static void require_at(bool ok, unsigned line) {
    if (ok) return;
    static const char msg[] = "PIPE USER FAIL ";
    char number[] = {'0' + line / 100 % 10, '0' + line / 10 % 10, '0' + line % 10, '\n'};
    call(SYS_WRITE, 2, (uintptr_t)msg, sizeof(msg) - 1);
    call(SYS_WRITE, 2, (uintptr_t)number, sizeof(number));
    call(SYS_EXIT, 1, 0, 0);
    __builtin_trap();
}
#define require(x) require_at((x), __LINE__)
static void spawn_tests(int *fds) {
    static const char *args[] = {"/bin/shell", "0", NULL};
    static spawn_fd_action_t action;
    static spawn_opts_t opts;
    for (unsigned same = 0; same < 3; same++) {
        require(call(SYS_PIPE, (uintptr_t)fds, VFS_O_CLOEXEC, 0) == 0);
        require(fds[0] == 3 && fds[1] == 4);
        args[1] = same == 0 ? "0" : same == 1 ? "3" : "s";
        action = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_DUP2,
                                    .src_fd = 3, .dst_fd = same ? 3 : 0};
        opts = (spawn_opts_t){.size = sizeof(opts), .version = 1,
                             .argv = (uintptr_t)args, .fd_actions = same == 2 ? 0 : (uintptr_t)&action,
                             .action_count = same == 2 ? 0 : 1};
        if (same != 2) {
            action.src_fd = 30;
            require(call(SYS_SPAWN_EXT, (uintptr_t)args[0], (uintptr_t)&opts, sizeof(opts)) == SYSCALL_EBADF);
            require(call(SYS_FCNTL, 3, F_GETFD, 0) == FD_CLOEXEC);
            require(call(SYS_FCNTL, 4, F_GETFD, 0) == FD_CLOEXEC);
            action.src_fd = 3;
        }
        long pid = call(SYS_SPAWN_EXT, (uintptr_t)args[0], (uintptr_t)&opts, sizeof(opts));
        require(pid > 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        if (same != 2) {
            for (unsigned block = 0; block < 32; block++) {
                size_t offset = 0;
                while (offset < sizeof(readonly)) {
                    long n = call(SYS_WRITE, 4, (uintptr_t)(readonly + offset), sizeof(readonly) - offset);
                    require(n > 0); offset += (size_t)n;
                }
            }
        }
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
        int64_t status = -1;
        require(call(SYS_WAIT, pid, (uintptr_t)&status, 0) == 0 && status == 0);
    }
}

void shell_main(int argc, const char **argv) {
    if (argc > 1) {
        require(call(SYS_FCNTL, 4, F_GETFD, 0) == SYSCALL_EBADF);
        if (argv[1][0] == 's') {
            require(call(SYS_FCNTL, 3, F_GETFD, 0) == SYSCALL_EBADF);
            return;
        }
        int fd = argv[1][0] == '3' ? 3 : 0;
        require(call(SYS_FCNTL, fd, F_GETFD, 0) == 0);
        if (fd == 0) require(call(SYS_FCNTL, 3, F_GETFD, 0) == SYSCALL_EBADF);
        size_t total = 0;
        for (;;) {
            long n = call(SYS_READ, fd, (uintptr_t)pages, sizeof(pages));
            require(n >= 0);
            if (!n) break;
            for (long i = 0; i < n; i++) require(pages[i] == ((total + i) % 8192 == 0 ? 1 : 0));
            total += (size_t)n;
        }
        require(total == 256 * 1024);
        return;
    }
    int *fds = (int *)(pages + 4092); /* Writable range spans two mapped pages. */
    require(call(SYS_PIPE, 0xfff, 0, 0) == SYSCALL_EFAULT);
    require(call(SYS_PIPE, 0x800000000000ULL - 4, 0, 0) == SYSCALL_EFAULT);
    require(call(SYS_PIPE, 0xffffffff80000000ULL, 0, 0) == SYSCALL_EFAULT);
    require(call(SYS_PIPE, 0x100000000ULL, 0, 0) == SYSCALL_EFAULT);
    require(call(SYS_PIPE, (uintptr_t)(readonly + 4092), 0, 0) == SYSCALL_EFAULT);
    require(call(SYS_PIPE, (uintptr_t)fds, VFS_O_RDWR, 0) == SYSCALL_EINVAL);
    for (unsigned cycle = 0; cycle < 32; cycle++) {
        require(call(SYS_PIPE, (uintptr_t)fds, VFS_O_CLOEXEC, 0) == 0);
        require(fds[0] == 3 && fds[1] == 4);
        require(call(SYS_FCNTL, 3, F_GETFD, 0) == FD_CLOEXEC);
        require(call(SYS_FCNTL, 4, F_GETFD, 0) == FD_CLOEXEC);
        pages[0] = 123;
        require(call(SYS_WRITE, 4, (uintptr_t)pages, 1) == 1);
        require(call(SYS_DUP, 4, 0, 0) == 5);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
        require(call(SYS_READ, 3, (uintptr_t)(pages + 1), 2) == 1 && pages[1] == 123);
        require(call(SYS_FCNTL, 5, F_GETFD, 0) == 0);
        require(call(SYS_CLOSE, 5, 0, 0) == 0);
        require(call(SYS_READ, 3, (uintptr_t)pages, 1) == 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_PIPE, (uintptr_t)fds, 0, 0) == 0);
        require(call(SYS_CLOSE, 3, 0, 0) == 0);
        require(call(SYS_WRITE, 4, (uintptr_t)pages, 1) == SYSCALL_EPIPE);
        require(call(SYS_CLOSE, 4, 0, 0) == 0);
    }
    for (int i = 3; i < 31; i++) require(call(SYS_DUP, 1, 0, 0) == i);
    fds[0] = fds[1] = -99;
    require(call(SYS_PIPE, (uintptr_t)fds, 0, 0) == SYSCALL_EMFILE);
    require(fds[0] == -99 && fds[1] == -99);
    require(call(SYS_DUP, 1, 0, 0) == 31);
    require(call(SYS_PIPE, (uintptr_t)fds, 0, 0) == SYSCALL_EMFILE);
    for (int i = 3; i < 32; i++) require(call(SYS_CLOSE, i, 0, 0) == 0);
    spawn_tests(fds);
    static const char pass[] = "PIPE USER PASS\n";
    call(SYS_WRITE, 1, (uintptr_t)pass, sizeof(pass) - 1);
}
