#include <assert.h>
#include <string.h>
#include "syscall_abi.h"

long write_bytes_fd(int fd, const char *s, size_t n);
long puts_err(const char *s);
static const char data[] = "abcdef";
static long replies[4];
static size_t calls, offset;
static int expected_fd;

long call(long nr, uintptr_t fd, uintptr_t buf, uintptr_t count) {
    assert(nr == SYS_WRITE && fd == (uintptr_t)expected_fd);
    assert(calls < 4 && count == sizeof(data) - 1 - offset);
    assert(memcmp((const void *)buf, data + offset, count) == 0);
    long result = replies[calls++];
    if (result > 0) offset += (size_t)result;
    return result;
}

static void reset(long first, long second) {
    calls = offset = 0;
    expected_fd = 2;
    replies[0] = first;
    replies[1] = second;
}

int main(void) {
    reset(2, 4);
    assert(puts_err(data) == 6 && calls == 2 && offset == 6);
    reset(2, SYSCALL_EBADF);
    assert(puts_err(data) == SYSCALL_EBADF && calls == 2);
    reset(SYSCALL_EBADF, 0);
    assert(puts_err(data) == SYSCALL_EBADF && calls == 1);
    reset(0, 0);
    assert(puts_err(data) == SYSCALL_EIO && calls == 1);
    reset(2, 0);
    assert(puts_err(data) == SYSCALL_EIO && calls == 2);
    reset(0, 0);
    assert(write_bytes_fd(2, data, 0) == 0 && calls == 0);
    return 0;
}
