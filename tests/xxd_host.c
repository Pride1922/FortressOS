/* Host unit test suite for xxd with mocked syscalls under ASan/UBSan */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

typedef struct { bool open; const unsigned char *data; size_t len, pos; } input_t;
static input_t fds[32];
static const unsigned char *file_data[4];
static size_t file_len[4];
static unsigned char output[65536];
static size_t output_len;
static char errors[4096];
static size_t errors_len;
static unsigned char file_out_buf[65536];
static size_t file_out_len;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        const char *name = (const char *)a;
        if (!strcmp(name, "missing")) return SYSCALL_ENOENT;
        ((vfs_stat_t *)b)->type = !strcmp(name, "dir") ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *name = (const char *)a;
        if (!strcmp(name, "out_file")) {
            file_out_len = 0;
            fds[5] = (input_t){true, NULL, 0, 0};
            return 5;
        }
        if (!strcmp(name, "in_file")) {
            fds[4] = (input_t){true, file_data[0], file_len[0], 0};
            return 4;
        }
        return SYSCALL_ENOENT;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        fds[a].open = false;
        return 0;
    }
    if (nr == SYS_READ) {
        if (a >= 32 || !fds[a].open || !fds[a].data) return SYSCALL_EBADF;
        input_t *fd = &fds[a];
        size_t n = fd->len - fd->pos;
        if (n > c) n = c;
        memcpy((void *)b, fd->data + fd->pos, n);
        fd->pos += n;
        return (long)n;
    }
    if (nr == SYS_WRITE) {
        size_t n = c;
        if (a == 1) {
            assert(output_len + n < sizeof(output));
            memcpy(output + output_len, (const void *)b, n);
            output_len += n;
            output[output_len] = 0;
        } else if (a == 2) {
            assert(errors_len + n < sizeof(errors));
            memcpy(errors + errors_len, (const void *)b, n);
            errors_len += n;
            errors[errors_len] = 0;
        } else if (a == 5) {
            assert(file_out_len + n < sizeof(file_out_buf));
            memcpy(file_out_buf + file_out_len, (const void *)b, n);
            file_out_len += n;
            file_out_buf[file_out_len] = 0;
        }
        return (long)n;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

static void reset_io(const void *stdin_bytes, size_t in_len) {
    memset(fds, 0, sizeof(fds));
    fds[0] = (input_t){true, (const unsigned char *)stdin_bytes, in_len, 0};
    fds[1].open = fds[2].open = true;
    for (int i = 0; i < 4; i++) {
        file_data[i] = NULL;
        file_len[i] = 0;
    }
    output_len = errors_len = file_out_len = 0;
    output[0] = errors[0] = file_out_buf[0] = 0;
}

static void reset_io_str(const char *stdin_str) {
    reset_io(stdin_str, stdin_str ? strlen(stdin_str) : 0);
}

int main(void) {
    printf("Running xxd host tests under ASan/UBSan...\n");

    /* 1. Basic hex dump (16 cols, 2-byte grouping, ASCII sidebar) */
    {
        const char *sample = "Hello FortressOS World!";
        reset_io_str(sample);
        char *argv[] = {"xxd", NULL};
        int rc = xxd_main(1, argv);
        assert(rc == 0);
        const char *expected =
            "00000000: 4865 6c6c 6f20 466f 7274 7265 7373 4f53  Hello FortressOS\n"
            "00000010: 2057 6f72 6c64 21                         World!\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 1: Basic hex dump\n");
    }

    /* 2. Custom grouping (-g 1) */
    {
        const char *sample = "ABC";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-c", "8", "-g", "1", NULL};
        int rc = xxd_main(5, argv);
        assert(rc == 0);
        const char *expected = "00000000: 41 42 43                 ABC\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 2: Custom grouping -g 1\n");
    }

    /* 3. Disable grouping (-g 0) */
    {
        const char *sample = "Hello";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-g", "0", NULL};
        int rc = xxd_main(3, argv);
        assert(rc == 0);
        const char *expected = "00000000: 48656c6c6f                        Hello\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 3: Disable grouping -g 0\n");
    }

    /* 4. Limit length (-l 5) */
    {
        const char *sample = "Hello FortressOS World!";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-l", "5", NULL};
        int rc = xxd_main(3, argv);
        assert(rc == 0);
        const char *expected = "00000000: 4865 6c6c 6f                             Hello\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 4: Limit length -l 5\n");
    }

    /* 5. Seek offset (-s 6) */
    {
        const char *sample = "Hello FortressOS World!";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-s", "6", "-l", "8", NULL};
        int rc = xxd_main(5, argv);
        assert(rc == 0);
        const char *expected = "00000006: 466f 7274 7265 7373                      Fortress\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 5: Seek offset -s 6 -l 8\n");
    }

    /* 6. Plain hex dump (-p) */
    {
        const char *sample = "Hello FortressOS World!";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-p", NULL};
        int rc = xxd_main(2, argv);
        assert(rc == 0);
        const char *expected = "48656c6c6f20466f7274726573734f5320576f726c6421\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 6: Plain hex dump -p\n");
    }

    /* 7. Plain hex dump with custom columns (-p -c 5) */
    {
        const char *sample = "Hello World";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-p", "-c", "5", NULL};
        int rc = xxd_main(4, argv);
        assert(rc == 0);
        const char *expected = "48656c6c6f\n20576f726c\n64\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 7: Plain hex dump -p -c 5\n");
    }

    /* 8. Reverse mode standard (-r) */
    {
        const char *hex_input =
            "00000000: 4865 6c6c 6f20 466f 7274 7265 7373 4f53  Hello FortressOS\n"
            "00000010: 2057 6f72 6c64 21                         World!\n";
        reset_io_str(hex_input);
        char *argv[] = {"xxd", "-r", NULL};
        int rc = xxd_main(2, argv);
        assert(rc == 0);
        assert(output_len == 23);
        assert(memcmp(output, "Hello FortressOS World!", 23) == 0);
        printf("  [PASS] Test 8: Reverse mode standard -r\n");
    }

    /* 9. Reverse mode plain (-r -p) */
    {
        const char *hex_input = "48656c6c6f20466f7274726573734f5320576f726c6421\n";
        reset_io_str(hex_input);
        char *argv[] = {"xxd", "-r", "-p", NULL};
        int rc = xxd_main(3, argv);
        assert(rc == 0);
        assert(output_len == 23);
        assert(memcmp(output, "Hello FortressOS World!", 23) == 0);
        printf("  [PASS] Test 9: Reverse mode plain -r -p\n");
    }

    /* 10. Uppercase mode (-u) */
    {
        const char *sample = "test";
        reset_io_str(sample);
        char *argv[] = {"xxd", "-u", "-l", "4", NULL};
        int rc = xxd_main(4, argv);
        assert(rc == 0);
        const char *expected = "00000000: 7465 7374                                test\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 10: Uppercase mode -u\n");
    }

    /* 11. Empty input */
    {
        reset_io_str("");
        char *argv[] = {"xxd", NULL};
        int rc = xxd_main(1, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 11: Empty input\n");
    }

    /* 12. File input and output operands */
    {
        const char *sample = "File payload!";
        reset_io_str(NULL);
        file_data[0] = (const unsigned char *)sample;
        file_len[0] = strlen(sample);
        char *argv[] = {"xxd", "in_file", "out_file", NULL};
        int rc = xxd_main(3, argv);
        assert(rc == 0);
        assert(file_out_len > 0);
        assert(strstr((char *)file_out_buf, "File payload!") != NULL);
        printf("  [PASS] Test 12: File input and output operands\n");
    }

    /* 13. Help option (--help) */
    {
        reset_io_str(NULL);
        char *argv[] = {"xxd", "--help", NULL};
        int rc = xxd_main(2, argv);
        assert(rc == 0);
        assert(strstr((char *)output, "Usage: xxd") != NULL);
        printf("  [PASS] Test 13: Help option --help\n");
    }

    /* 14. Invalid argument */
    {
        reset_io_str(NULL);
        char *argv[] = {"xxd", "-z", NULL};
        int rc = xxd_main(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "invalid option") != NULL);
        printf("  [PASS] Test 14: Invalid argument\n");
    }

    /* 15. Binary bytes roundtrip with null bytes and high bytes */
    {
        unsigned char raw[256];
        for (int i = 0; i < 256; i++) raw[i] = (unsigned char)i;
        reset_io(raw, 256);
        char *argv1[] = {"xxd", NULL};
        int rc1 = xxd_main(1, argv1);
        assert(rc1 == 0);

        /* Now revert that output */
        unsigned char hex_dump[65536];
        size_t hex_len = output_len;
        memcpy(hex_dump, output, hex_len);

        reset_io(hex_dump, hex_len);
        char *argv2[] = {"xxd", "-r", NULL};
        int rc2 = xxd_main(2, argv2);
        assert(rc2 == 0);
        assert(output_len == 256);
        assert(memcmp(output, raw, 256) == 0);
        printf("  [PASS] Test 15: Full 256-byte binary roundtrip\n");
    }

    printf("ALL XXD HOST TESTS PASSED!\n");
    return 0;
}
