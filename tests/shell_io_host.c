#include <assert.h>
#include <string.h>
#include "syscall_abi.h"

long write_bytes_fd(int fd, const char *s, size_t n);
long puts_err(const char *s);
void file_error(long error);
void file_error_err(long error);

static const char data[] = "abcdef";
static long replies[4];
static size_t calls, offset;
static int expected_fd;

static bool capture_mode;
static int capture_fd;
static char captured[128];
static size_t captured_len;

long call(long nr, uintptr_t fd, uintptr_t buf, uintptr_t count) {
    if (capture_mode) {
        assert(nr == SYS_WRITE && (int)fd == capture_fd);
        assert(captured_len + count < sizeof(captured));
        memcpy(captured + captured_len, (const void *)buf, count);
        captured_len += count;
        captured[captured_len] = '\0';
        return (long)count;
    }
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

static void test_file_error(long code, int fd, const char *expected) {
    capture_mode = true;
    capture_fd = fd;
    captured_len = 0;
    memset(captured, 0, sizeof(captured));
    if (fd == 1) file_error(code);
    else file_error_err(code);
    assert(strcmp(captured, expected) == 0);
    capture_mode = false;
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

    /* Verify distinct error strings for Phase C (RO vs Tainted vs Missing vs Generic) */
    test_file_error(SYSCALL_EROFS, 1, "Read-only filesystem.\n");
    test_file_error(SYSCALL_EROFS, 2, "Read-only filesystem.\n");
    test_file_error(SYSCALL_EIO, 1, "I/O error.\n");
    test_file_error(SYSCALL_EIO, 2, "I/O error.\n");
    test_file_error(SYSCALL_ENOENT, 1, "No such file or directory.\n");
    test_file_error(SYSCALL_ENOENT, 2, "No such file or directory.\n");
    test_file_error(SYSCALL_EINVAL, 1, "File operation failed.\n");
    test_file_error(SYSCALL_EINVAL, 2, "File operation failed.\n");
    test_file_error(-999, 1, "File operation failed.\n");
    test_file_error(-999, 2, "File operation failed.\n");
    return 0;
}
