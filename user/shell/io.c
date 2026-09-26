#include "io.h"
long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return nr;
}
size_t length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#define WRITE_CHUNK 4096

void puts(const char *s);      /* defined below */
void put_dec(size_t val);      /* defined below */

void write_bytes_fd(int fd, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
        long r = call(SYS_WRITE, fd, (uintptr_t)(s + off), chunk);
        if (r < 0) {
            /* Output failure must not recursively write another error. */
            return;
        }
        off += (size_t)r;
        if (r == 0) break;  /* avoid infinite loop on buggy drivers */
    }
}

void write_bytes(const char *s, size_t n) {
    write_bytes_fd(1, s, n);
}

void puts_fd(int fd, const char *s) {
    write_bytes_fd(fd, s, length(s));
}

void puts_err(const char *s) {
    puts_fd(2, s);
}

void puts(const char *s) { write_bytes(s, length(s)); }
void file_error(long error) {
    if (error == SYSCALL_EROFS) puts("Read-only filesystem.\n");
    else puts(error == SYSCALL_ENOENT ? "No such file or directory.\n" : "File operation failed.\n");
}

void file_error_err(long error) {
    if (error == SYSCALL_EROFS) puts_err("Read-only filesystem.\n");
    else puts_err(error == SYSCALL_ENOENT ? "No such file or directory.\n" : "File operation failed.\n");
}

void put_dec(size_t val) {
    char buf[24];
    size_t i = 0;
    if (val == 0) { puts("0"); return; }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    buf[i] = 0;
    puts(buf);
}

