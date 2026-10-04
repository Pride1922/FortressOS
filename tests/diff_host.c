/* Host unit test suite for diff with mocked syscalls under ASan/UBSan */
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

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        const char *name = (const char *)a;
        if (!strcmp(name, "missing")) return SYSCALL_ENOENT;
        ((vfs_stat_t *)b)->type = !strcmp(name, "dir") ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *name = (const char *)a;
        if (!strcmp(name, "file_a")) {
            fds[4] = (input_t){true, file_data[0], file_len[0], 0};
            return 4;
        }
        if (!strcmp(name, "file_b")) {
            fds[5] = (input_t){true, file_data[1], file_len[1], 0};
            return 5;
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
        }
        return (long)n;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

static void reset_io(const char *stdin_str, const char *fa, const char *fb) {
    memset(fds, 0, sizeof(fds));
    size_t in_len = stdin_str ? strlen(stdin_str) : 0;
    fds[0] = (input_t){true, (const unsigned char *)stdin_str, in_len, 0};
    fds[1].open = fds[2].open = true;
    for (int i = 0; i < 4; i++) {
        file_data[i] = NULL;
        file_len[i] = 0;
    }
    if (fa) {
        file_data[0] = (const unsigned char *)fa;
        file_len[0] = strlen(fa);
    }
    if (fb) {
        file_data[1] = (const unsigned char *)fb;
        file_len[1] = strlen(fb);
    }
    output_len = errors_len = 0;
    output[0] = errors[0] = 0;
}

int main(void) {
    printf("Running diff host tests under ASan/UBSan...\n");

    /* 1. Identical files */
    {
        const char *text = "alpha\nbeta\ngamma\n";
        reset_io(NULL, text, text);
        char *argv[] = {"diff", "file_a", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 1: Identical files return 0\n");
    }

    /* 2. Deletion hunk (2d1) */
    {
        const char *fa = "alpha\nbeta\ngamma\n";
        const char *fb = "alpha\ngamma\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "file_a", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 1);
        const char *expected = "2d1\n< beta\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 2: Deletion hunk 2d1\n");
    }

    /* 3. Addition hunk (1a2) */
    {
        const char *fa = "alpha\ngamma\n";
        const char *fb = "alpha\nbeta\ngamma\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "file_a", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 1);
        const char *expected = "1a2\n> beta\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 3: Addition hunk 1a2\n");
    }

    /* 4. Change hunk (2c2) */
    {
        const char *fa = "alpha\nbeta\ngamma\n";
        const char *fb = "alpha\nBETA\ngamma\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "file_a", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 1);
        const char *expected = "2c2\n< beta\n---\n> BETA\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 4: Change hunk 2c2\n");
    }

    /* 5. Classic Myers example (multiple hunks) */
    {
        const char *fa = "a\nb\nc\nd\nf\ng\nh\n";
        const char *fb = "a\nc\nd\ne\nf\ng\ni\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "file_a", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 1);
        const char *expected =
            "2d1\n< b\n"
            "4a4\n> e\n"
            "7c7\n< h\n---\n> i\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 5: Classic Myers multi-hunk diff\n");
    }

    /* 6. Unified diff format (-u) */
    {
        const char *fa = "a\nb\nc\nd\nf\ng\nh\n";
        const char *fb = "a\nc\nd\ne\nf\ng\ni\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "-u", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 1);
        assert(strstr((char *)output, "--- file_a\n") != NULL);
        assert(strstr((char *)output, "+++ file_b\n") != NULL);
        assert(strstr((char *)output, "@@ -1,7 +1,7 @@\n") != NULL);
        assert(strstr((char *)output, "-b\n") != NULL);
        assert(strstr((char *)output, "+e\n") != NULL);
        assert(strstr((char *)output, "-h\n") != NULL);
        assert(strstr((char *)output, "+i\n") != NULL);
        printf("  [PASS] Test 6: Unified diff format -u\n");
    }

    /* 7. Brief diff (-q) */
    {
        const char *fa = "one\n";
        const char *fb = "two\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "-q", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 1);
        const char *expected = "Files file_a and file_b differ\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 7: Brief diff -q\n");
    }

    /* 8. Ignore case (-i) */
    {
        const char *fa = "Hello World\n";
        const char *fb = "hello world\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "-i", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 8: Ignore case -i\n");
    }

    /* 9. Ignore all space (-w) */
    {
        const char *fa = "h e l l o\n";
        const char *fb = "hello\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "-w", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 9: Ignore all space -w\n");
    }

    /* 10. Ignore space change (-b) */
    {
        const char *fa = "hello   world\n";
        const char *fb = "hello world\n";
        reset_io(NULL, fa, fb);
        char *argv[] = {"diff", "-b", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 0);
        assert(output_len == 0);
        printf("  [PASS] Test 10: Ignore space change -b\n");
    }

    /* 11. Stdin comparison (diff file_a -) */
    {
        const char *fa = "line 1\nline 2\n";
        const char *stdin_text = "line 1\nline 2 MODIFIED\n";
        reset_io(stdin_text, fa, NULL);
        char *argv[] = {"diff", "file_a", "-", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 1);
        const char *expected = "2c2\n< line 2\n---\n> line 2 MODIFIED\n";
        assert(strcmp((char *)output, expected) == 0);
        printf("  [PASS] Test 11: Stdin comparison diff file -\n");
    }

    /* 12. Missing file error handling */
    {
        reset_io(NULL, NULL, NULL);
        char *argv[] = {"diff", "missing", "file_b", NULL};
        int rc = diff_main(3, argv);
        assert(rc == 2);
        assert(strstr(errors, "cannot open") != NULL);
        printf("  [PASS] Test 12: Missing file returns code 2\n");
    }

    /* 13. Help option (--help) */
    {
        reset_io(NULL, NULL, NULL);
        char *argv[] = {"diff", "--help", NULL};
        int rc = diff_main(2, argv);
        assert(rc == 0);
        assert(strstr((char *)output, "Usage: diff") != NULL);
        printf("  [PASS] Test 13: Help option --help\n");
    }

    /* 14. Invalid option */
    {
        reset_io(NULL, NULL, NULL);
        char *argv[] = {"diff", "-z", "file_a", "file_b", NULL};
        int rc = diff_main(4, argv);
        assert(rc == 2);
        assert(strstr(errors, "invalid option") != NULL);
        printf("  [PASS] Test 14: Invalid option\n");
    }

    printf("ALL DIFF HOST TESTS PASSED!\n");
    return 0;
}
