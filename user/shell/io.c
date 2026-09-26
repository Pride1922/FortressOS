#include "io.h"
#ifndef SHELL_IO_HOST_TEST
long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif
size_t length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#define WRITE_CHUNK 4096

void puts(const char *s);      /* defined below */
void put_dec(size_t val);      /* defined below */

long write_bytes_fd(int fd, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
        long r = call(SYS_WRITE, fd, (uintptr_t)(s + off), chunk);
        if (r < 0) {
            /* Output failure must not recursively write another error. */
            return r;
        }
        if (r == 0 || (size_t)r > chunk) return SYSCALL_EIO;
        off += (size_t)r;
    }
    return (long)off;
}

void write_bytes(const char *s, size_t n) {
    write_bytes_fd(1, s, n);
}

void puts_fd(int fd, const char *s) {
    write_bytes_fd(fd, s, length(s));
}

long puts_err(const char *s) {
    return write_bytes_fd(2, s, length(s));
}

void puts(const char *s) { write_bytes(s, length(s)); }
static const char *file_error_string(long error) {
    if (error == SYSCALL_EROFS) return "Read-only filesystem.\n";
    if (error == SYSCALL_EIO)   return "I/O error.\n";
    if (error == SYSCALL_ENOENT) return "No such file or directory.\n";
    return "File operation failed.\n";
}

void file_error(long error) {
    puts(file_error_string(error));
}

void file_error_err(long error) {
    puts_err(file_error_string(error));
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
