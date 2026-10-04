/* Host unit test suite for sort with mocked syscalls under ASan/UBSan */
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
        if (!strcmp(name, "in_file1")) {
            fds[4] = (input_t){true, file_data[0], file_len[0], 0};
            return 4;
        }
        if (!strcmp(name, "in_file2")) {
            fds[6] = (input_t){true, file_data[1], file_len[1], 0};
            return 6;
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
    printf("Running sort host tests under ASan/UBSan...\n");

    /* 1. Basic lexicographical sort */
    {
        const char *sample = "banana\napple\ncherry\ndate\n";
        reset_io_str(sample);
        char *argv[] = {"sort", NULL};
        int rc = sort_main(1, argv);
        assert(rc == 0);
        const char *expected = "apple\nbanana\ncherry\ndate\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 1: Basic lexicographical sort\n");
    }

    /* 2. Reverse sort (-r) */
    {
        const char *sample = "apple\nbanana\ncherry\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-r", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        const char *expected = "cherry\nbanana\napple\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 2: Reverse sort -r\n");
    }

    /* 3. Numeric sort (-n) with negatives, zero, positives */
    {
        const char *sample = "10\n-5\n2\n0\n1\n-20\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-n", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        const char *expected = "-20\n-5\n0\n1\n2\n10\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 3: Numeric sort -n\n");
    }

    /* 4. Unique sort (-u) */
    {
        const char *sample = "b\na\nb\nc\na\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-u", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        const char *expected = "a\nb\nc\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 4: Unique sort -u\n");
    }

    /* 5. Numeric unique sort (-n -u) */
    {
        const char *sample = "5 beta\n2 gamma\n5 alpha\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-n", "-u", NULL};
        int rc = sort_main(3, argv);
        assert(rc == 0);
        const char *expected = "2 gamma\n5 beta\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 5: Numeric unique sort -n -u\n");
    }

    /* 6. Case-insensitive sort (-f) */
    {
        const char *sample = "b\nA\na\nB\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-f", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        /* A and a compare equal on key, stable sort preserves original order */
        const char *expected = "A\na\nb\nB\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 6: Case-insensitive sort -f\n");
    }

    /* 7. Key field sort (-k 2) */
    {
        const char *sample = "user3 charlie\nuser1 alice\nuser2 bob\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-k", "2", NULL};
        int rc = sort_main(3, argv);
        assert(rc == 0);
        const char *expected = "user1 alice\nuser2 bob\nuser3 charlie\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 7: Key field sort -k 2\n");
    }

    /* 8. Key field numeric sort (-k 2 -n) */
    {
        const char *sample = "john 25\nalice 30\nbob 20\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-k", "2", "-n", NULL};
        int rc = sort_main(4, argv);
        assert(rc == 0);
        const char *expected = "bob 20\njohn 25\nalice 30\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 8: Key field numeric sort -k 2 -n\n");
    }

    /* 9. Check sorted order (-c) on sorted input */
    {
        const char *sample = "apple\nbanana\ncherry\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-c", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 9: Check -c sorted\n");
    }

    /* 10. Check sorted order (-c) on disordered input */
    {
        const char *sample = "banana\napple\ncherry\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-c", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 1);
        assert(strstr(errors, "disorder") != NULL);
        printf("  [PASS] Test 10: Check -c disordered\n");
    }

    /* 11. Output file option (-o out_file) */
    {
        const char *sample = "zebra\nantelope\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-o", "out_file", NULL};
        int rc = sort_main(3, argv);
        assert(rc == 0);
        assert(output_len == 0);
        assert(strcmp((char *)file_out_buf, "antelope\nzebra\n") == 0);
        printf("  [PASS] Test 11: Output file option -o\n");
    }

    /* 12. Multiple input files */
    {
        const char *file1 = "cat\napple\n";
        const char *file2 = "dog\nbanana\n";
        reset_io_str(NULL);
        file_data[0] = (const unsigned char *)file1;
        file_len[0] = strlen(file1);
        file_data[1] = (const unsigned char *)file2;
        file_len[1] = strlen(file2);
        char *argv[] = {"sort", "in_file1", "in_file2", NULL};
        int rc = sort_main(3, argv);
        assert(rc == 0);
        const char *expected = "apple\nbanana\ncat\ndog\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 12: Multiple input files\n");
    }

    /* 13. Stability test on equal keys */
    {
        const char *sample = "item 1 first\nitem 2\nitem 1 second\n";
        reset_io_str(sample);
        char *argv[] = {"sort", "-k", "2", "-n", NULL};
        int rc = sort_main(4, argv);
        assert(rc == 0);
        /* item 1 first and item 1 second have key=1; tie-breaker distinguishes them */
        const char *expected = "item 1 first\nitem 1 second\nitem 2\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 13: Sort stability and tie-breaker\n");
    }

    /* 14. Empty input */
    {
        reset_io_str("");
        char *argv[] = {"sort", NULL};
        int rc = sort_main(1, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 14: Empty input\n");
    }

    /* 15. Help option (--help) */
    {
        reset_io_str(NULL);
        char *argv[] = {"sort", "--help", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 0);
        assert(strstr((char *)output, "Usage: sort") != NULL);
        printf("  [PASS] Test 15: Help option --help\n");
    }

    /* 16. Invalid option */
    {
        reset_io_str(NULL);
        char *argv[] = {"sort", "-x", NULL};
        int rc = sort_main(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "invalid option") != NULL);
        printf("  [PASS] Test 16: Invalid option\n");
    }

    printf("ALL SORT HOST TESTS PASSED!\n");
    return 0;
}
