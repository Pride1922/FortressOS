/* Test-only external producer/relay/verifier; packaged only in the S7 test ISO. */
#include "syscall_abi.h"
#include "vfs.h"
static unsigned char buffer[4096];
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
static bool eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int number(const char *s) {
    unsigned n = 0;
    if (!*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9' || n > 100000) return -1;
        n = n * 10 + (*s++ - '0');
    }
    return n <= 1000000 ? (int)n : -1;
}
static int write_all(const unsigned char *p, size_t n) {
    while (n) {
        long w = call(SYS_WRITE, 1, (uintptr_t)p, n);
        if (w <= 0) return 1; /* Default SIGPIPE terminates before returning. */
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* Observe both real child statuses; the shell's last-stage status cannot prove
 * that an upstream tool saw EPIPE. Every failure closes before blocking waits. */
static int observe_stream_status(unsigned mode) {
    int fds[2];
    if (mode > 2 || call(SYS_PIPE, (uintptr_t)fds, VFS_O_CLOEXEC, 0) < 0) return 50;
    const char *producer[] = {"/bin/cat", "/mnt/s7payload", NULL};
    const char *consumer[] = {mode == 2 ? "/bin/tail" : "/bin/head", "-c", mode == 1 ? "1" : "0", NULL};
    spawn_fd_action_t actions[2] = {{.type = SPAWN_FD_ACTION_DUP2, .src_fd = fds[1], .dst_fd = 1}};
    spawn_opts_t opts = {.size = sizeof(opts), .version = 1, .argv = (uintptr_t)producer,
                         .fd_actions = (uintptr_t)actions, .action_count = 1};
    long prod = call(SYS_SPAWN_EXT, (uintptr_t)producer[0], (uintptr_t)&opts, sizeof(opts));
    long cons = -1;
    if (prod >= 0) {
        actions[0].src_fd = fds[0]; actions[0].dst_fd = 0;
        if (mode == 1) {
            actions[1] = (spawn_fd_action_t){.type = SPAWN_FD_ACTION_OPEN, .dst_fd = 1,
                .flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, .mode = 0644,
                .path = (uintptr_t)"/mnt/s7headbyte"};
            opts.action_count = 2;
        }
        opts.argv = (uintptr_t)consumer;
        cons = call(SYS_SPAWN_EXT, (uintptr_t)consumer[0], (uintptr_t)&opts, sizeof(opts));
    }
    call(SYS_CLOSE, fds[0], 0, 0); call(SYS_CLOSE, fds[1], 0, 0);
    int64_t ps = -1, cs = -1;
    bool failed = prod < 0 || cons < 0;
    if (prod >= 0 && call(SYS_WAIT, prod, (uintptr_t)&ps, 0) < 0) failed = true;
    if (cons >= 0 && call(SYS_WAIT, cons, (uintptr_t)&cs, 0) < 0) failed = true;
    if (failed || ps != (mode == 2 ? 0 : 141) || cs != 0) return 51;
    static const unsigned char pass[] = "STREAM STATUS OK\n";
    return write_all(pass, sizeof(pass) - 1);
}
int pipeline_fixture_main(int argc, char **argv) {
    if (argc < 2) return 2;
    int amount = argc > 2 ? number(argv[2]) : 300123;
    if (amount < 0) return 2;
    if (eq(argv[1], "status")) return amount;
    if (eq(argv[1], "observe")) return observe_stream_status((unsigned)amount);
    /* All private pipe fds and the terminal UI descriptor must be swept. */
    for (int fd = 3; fd < 32; fd++)
        if (call(SYS_FCNTL, fd, F_GETFD, 0) != SYSCALL_EBADF) return 40;
    if (eq(argv[1], "early")) return 0;
    if (eq(argv[1], "sample")) {
        static const unsigned char sample[] = {'A', 0, 255, '\t', '\r', 'B'};
        return write_all(sample, sizeof(sample));
    }
    if (eq(argv[1], "text")) {
        static const unsigned char text[] = "one two\nthree\n\nlast";
        return write_all(text, sizeof(text) - 1);
    }
    if (eq(argv[1], "produce")) {
        unsigned total = 0;
        while (total < (unsigned)amount) {
            unsigned n = (unsigned)amount - total;
            if (n > sizeof(buffer)) n = sizeof(buffer);
            for (unsigned i = 0; i < n; i++) buffer[i] = (total + i) % 251;
            int error = write_all(buffer, n);
            if (error) return error;
            total += n;
        }
        return 0;
    }
    bool verify = eq(argv[1], "verify");
    if (!verify && !eq(argv[1], "relay")) return 2;
    unsigned total = 0;
    for (;;) {
        long n = call(SYS_READ, 0, (uintptr_t)buffer, sizeof(buffer));
        if (n < 0) return 1;
        if (!n) break;
        if (verify) {
            for (long i = 0; i < n; i++)
                if (buffer[i] != (total + (unsigned)i) % 251) return 41;
        } else {
            int error = write_all(buffer, (size_t)n);
            if (error) return error;
        }
        total += (unsigned)n;
    }
    if (!verify) return 0;
    if (total != (unsigned)amount) return 42;
    static const unsigned char pass[] = "PIPELINE BYTES OK\n";
    return write_all(pass, sizeof(pass) - 1);
}
