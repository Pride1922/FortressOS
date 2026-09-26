/* Test-only external producer/relay/verifier; packaged only in the S7 test ISO. */
#include "syscall_abi.h"
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
        if (w <= 0) return w == SYSCALL_EPIPE ? 141 : 1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
int pipeline_fixture_main(int argc, char **argv) {
    if (argc < 2) return 2;
    int amount = argc > 2 ? number(argv[2]) : 300123;
    if (amount < 0) return 2;
    if (eq(argv[1], "status")) return amount;
    /* All private pipe fds and the terminal UI descriptor must be swept. */
    for (int fd = 3; fd < 32; fd++)
        if (call(SYS_FCNTL, fd, F_GETFD, 0) != SYSCALL_EBADF) return 40;
    if (eq(argv[1], "early")) return 0;
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
