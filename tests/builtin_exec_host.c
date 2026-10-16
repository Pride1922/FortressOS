/* Host unit tests for builtin_exec (echo -n/-e, printf %b audit, dmesg -a) under ASan/UBSan */
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "types.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "shell/builtin_exec.h"
#include "shell/builtins.h"

static char s_out_buf[16384];
static size_t s_out_len = 0;
static char s_err_buf[4096];
static size_t s_err_len = 0;

static char s_mock_file[16384];
static size_t s_mock_file_len = 0;
static long s_last_open_flags = 0;

void mock_reset(void) {
    s_out_len = 0;
    s_out_buf[0] = '\0';
    s_err_len = 0;
    s_err_buf[0] = '\0';
}

long write_bytes_fd(int fd, const void *buf, size_t n) {
    if (fd == 1) {
        if (s_out_len + n < sizeof(s_out_buf)) {
            memcpy(s_out_buf + s_out_len, buf, n);
            s_out_len += n;
            s_out_buf[s_out_len] = '\0';
        }
        return (long)n;
    } else if (fd == 2) {
        if (s_err_len + n < sizeof(s_err_buf)) {
            memcpy(s_err_buf + s_err_len, buf, n);
            s_err_len += n;
            s_err_buf[s_err_len] = '\0';
        }
        return (long)n;
    }
    return (long)n;
}

size_t length(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
bool equal(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const void *db_group(const void *db, const char *name) { (void)db; (void)name; return NULL; }
const void *db_passwd(const void *db, const char *name) { (void)db; (void)name; return NULL; }
const void *db_group_id(const void *db, uint32_t gid) { (void)db; (void)gid; return NULL; }
const void *db_user_id(const void *db, uint32_t uid) { (void)db; (void)uid; return NULL; }

void puts(const char *s) {
    if (s) write_bytes_fd(1, s, length(s));
}

void put_dec(size_t val) {
    char buf[32];
    int pos = 0;
    if (val == 0) buf[pos++] = '0';
    else {
        char tmp[32];
        int tpos = 0;
        while (val > 0) { tmp[tpos++] = '0' + (val % 10); val /= 10; }
        while (tpos > 0) buf[pos++] = tmp[--tpos];
    }
    buf[pos] = '\0';
    write_bytes_fd(1, buf, pos);
}

void file_error_err(long error) {
    (void)error;
}

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_DMESG) {
        char *buf = (char *)a;
        size_t cap = (size_t)b;
        const char *msg = "[BOOT] Kernel initialized\n[INIT] Starting userspace\n";
        size_t mlen = strlen(msg);
        if (mlen > cap) mlen = cap;
        memcpy(buf, msg, mlen);
        return (long)mlen;
    }
    if (nr == SYS_OPEN) {
        s_last_open_flags = (long)b;
        return 10; /* dummy fd */
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *buf = (const char *)b;
        size_t n = (size_t)c;
        if (fd == 10) {
            if (s_last_open_flags & VFS_O_TRUNC) {
                s_mock_file_len = 0;
            }
            if (s_mock_file_len + n < sizeof(s_mock_file)) {
                memcpy(s_mock_file + s_mock_file_len, buf, n);
                s_mock_file_len += n;
                s_mock_file[s_mock_file_len] = '\0';
            }
            return (long)n;
        }
    }
    if (nr == SYS_CLOSE) {
        return 0;
    }
    return 0;
}

static void print_msg(const char *s) {
    (void)write(1, s, strlen(s));
}

static void test_echo(void) {
    mock_reset();
    /* 1. Default echo */
    const char *args1[] = {"echo", "hello", "world"};
    int rc = builtin_exec(3, args1, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "hello world\n") == 0);

    /* 2. echo -n */
    mock_reset();
    const char *args2[] = {"echo", "-n", "no", "newline"};
    rc = builtin_exec(4, args2, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "no newline") == 0);

    /* 3. echo -e with escapes */
    mock_reset();
    const char *args3[] = {"echo", "-e", "hello\\tworld\\nsecond"};
    rc = builtin_exec(3, args3, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "hello\tworld\nsecond\n") == 0);

    /* 4. echo -ne (combination) */
    mock_reset();
    const char *args4[] = {"echo", "-ne", "line\\nno_tail"};
    rc = builtin_exec(3, args4, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "line\nno_tail") == 0);

    /* 5. echo -e with octal \0101 ('A') and hex \x42 ('B') */
    mock_reset();
    const char *args5[] = {"echo", "-e", "char\\0101and\\x42"};
    rc = builtin_exec(3, args5, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "charAandB\n") == 0);

    /* 6. echo -e with \c (early exit, suppress newline) */
    mock_reset();
    const char *args6[] = {"echo", "-e", "first\\chidden", "ignored"};
    rc = builtin_exec(4, args6, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "first") == 0);

    /* 7. echo -E disables escapes */
    mock_reset();
    const char *args7[] = {"echo", "-e", "-E", "raw\\ntext"};
    rc = builtin_exec(4, args7, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "raw\\ntext\n") == 0);

    print_msg("PASS: exec_echo (-n, -e, -E, escapes, octal, hex, \\c)\n");
}

static void test_printf_b(void) {
    mock_reset();
    /* 1. Basic %b */
    const char *args1[] = {"printf", "Msg: %b\n", "line1\\nline2"};
    int rc = builtin_exec(3, args1, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "Msg: line1\nline2\n") == 0);

    /* 2. %b with octal \0101 ('A') and hex \x43 ('C') */
    mock_reset();
    const char *args2[] = {"printf", "Val: %b\n", "\\0101\\x43"};
    rc = builtin_exec(3, args2, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "Val: AC\n") == 0);

    /* 3. %b with \c */
    mock_reset();
    const char *args3[] = {"printf", "Start:%b:End\n", "foo\\cbar"};
    rc = builtin_exec(3, args3, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "Start:foo") == 0);

    /* 4. %b with width and precision */
    mock_reset();
    const char *args4[] = {"printf", "[%10b]\n", "hello\\nworld"};
    rc = builtin_exec(3, args4, NULL);
    assert(rc == 0);
    /* "hello\nworld" is 11 chars, so width 10 does not pad */
    assert(strcmp(s_out_buf, "[hello\nworld]\n") == 0);

    mock_reset();
    const char *args5[] = {"printf", "[%10b]\n", "hi\\n"};
    rc = builtin_exec(3, args5, NULL);
    assert(rc == 0);
    /* "hi\n" is 3 chars, padded to 10 */
    assert(strcmp(s_out_buf, "[       hi\n]\n") == 0);

    mock_reset();
    const char *args6[] = {"printf", "[%.4b]\n", "abcdef"};
    rc = builtin_exec(3, args6, NULL);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "[abcd]\n") == 0);

    print_msg("PASS: exec_printf %b (octal, hex, \\c, width, precision)\n");
}

static void test_dmesg_append(void) {
    mock_reset();
    s_mock_file_len = 0;
    /* Default dmesg overwrite */
    const char *args1[] = {"dmesg", "/var/log/dmesg.log"};
    int rc = builtin_exec(2, args1, NULL);
    assert(rc == 0);
    assert((s_last_open_flags & VFS_O_TRUNC) != 0);
    assert((s_last_open_flags & VFS_O_APPEND) == 0);
    assert(strstr(s_out_buf, "Saved ") != NULL);
    assert(strstr(s_out_buf, "bytes to /var/log/dmesg.log") != NULL);

    /* dmesg -a append */
    mock_reset();
    const char *args2[] = {"dmesg", "-a", "/var/log/dmesg.log"};
    rc = builtin_exec(3, args2, NULL);
    assert(rc == 0);
    assert((s_last_open_flags & VFS_O_APPEND) != 0);
    assert((s_last_open_flags & VFS_O_TRUNC) == 0);
    assert(strstr(s_out_buf, "Saved ") != NULL);
    assert(strstr(s_out_buf, "bytes (appended) to /var/log/dmesg.log") != NULL);

    /* dmesg --append */
    mock_reset();
    const char *args3[] = {"dmesg", "--append", "/var/log/dmesg.log"};
    rc = builtin_exec(3, args3, NULL);
    assert(rc == 0);
    assert((s_last_open_flags & VFS_O_APPEND) != 0);

    print_msg("PASS: exec_dmesg (-a, --append, VFS_O_APPEND flag)\n");
}

int main(void) {
    test_echo();
    test_printf_b();
    test_dmesg_append();
    print_msg("ALL BUILTIN_EXEC HOST TESTS PASSED!\n");
    return 0;
}
