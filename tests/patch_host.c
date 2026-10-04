/* Host unit test suite for patch with mocked syscalls under ASan/UBSan */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

typedef struct {
    bool open;
    bool is_write;
    unsigned char data[65536];
    size_t len;
    size_t pos;
} mock_fd_t;

static mock_fd_t fds[32];
static char errors[4096];
static size_t errors_len = 0;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_OPEN) {
        const char *name = (const char *)a;
        int flags = (int)b;
        if (!strcmp(name, "missing") || !strcmp(name, "nonexistent")) {
            return SYSCALL_ENOENT;
        }
        if (!strcmp(name, "orig.txt") || !strcmp(name, "file.txt") || !strcmp(name, "source.c")) {
            int fd = 4;
            fds[fd].open = true;
            if (flags & VFS_O_TRUNC) {
                fds[fd].len = 0;
                fds[fd].pos = 0;
            }
            fds[fd].is_write = (flags & VFS_O_WRONLY) != 0;
            return fd;
        }
        if (!strcmp(name, "patch.diff") || !strcmp(name, "input.patch")) {
            int fd = 5;
            fds[fd].open = true;
            fds[fd].pos = 0;
            fds[fd].is_write = false;
            return fd;
        }
        if (!strcmp(name, "out.txt") || !strcmp(name, "dest.c")) {
            int fd = 6;
            fds[fd].open = true;
            if (flags & VFS_O_TRUNC) {
                fds[fd].len = 0;
                fds[fd].pos = 0;
            }
            fds[fd].is_write = (flags & VFS_O_WRONLY) != 0;
            return fd;
        }
        if (strstr(name, ".tmp")) {
            int fd = 8;
            fds[fd].open = true;
            if (flags & VFS_O_TRUNC) {
                fds[fd].len = 0;
                fds[fd].pos = 0;
            }
            fds[fd].is_write = (flags & VFS_O_WRONLY) != 0;
            return fd;
        }
        return SYSCALL_ENOENT;
    }
    if (nr == SYS_UNLINK) {
        const char *name = (const char *)a;
        if (strstr(name, ".tmp")) {
            fds[8].len = 0;
            fds[8].open = false;
        }
        return 0;
    }
    if (nr == SYS_RENAME) {
        const char *oldname = (const char *)a;
        const char *newname = (const char *)b;
        if (strstr(oldname, ".tmp") && (!strcmp(newname, "orig.txt") || !strcmp(newname, "file.txt") || !strcmp(newname, "source.c"))) {
            memcpy(fds[4].data, fds[8].data, fds[8].len);
            fds[4].len = fds[8].len;
            fds[4].data[fds[4].len] = 0;
            fds[8].len = 0;
            return 0;
        }
        return 0;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        fds[a].open = false;
        return 0;
    }
    if (nr == SYS_READ) {
        if (a >= 32 || !fds[a].open) return SYSCALL_EBADF;
        mock_fd_t *fd = &fds[a];
        size_t n = fd->len - fd->pos;
        if (n > c) n = c;
        memcpy((void *)b, fd->data + fd->pos, n);
        fd->pos += n;
        return (long)n;
    }
    if (nr == SYS_WRITE) {
        size_t n = c;
        if (a == 1) {
            mock_fd_t *fd = &fds[1];
            assert(fd->len + n < sizeof(fd->data));
            memcpy(fd->data + fd->len, (const void *)b, n);
            fd->len += n;
            fd->data[fd->len] = 0;
        } else if (a == 2) {
            assert(errors_len + n < sizeof(errors));
            memcpy(errors + errors_len, (const void *)b, n);
            errors_len += n;
            errors[errors_len] = 0;
        } else if (a < 32 && fds[a].open && fds[a].is_write) {
            mock_fd_t *fd = &fds[a];
            assert(fd->len + n < sizeof(fd->data));
            memcpy(fd->data + fd->len, (const void *)b, n);
            fd->len += n;
            fd->data[fd->len] = 0;
        } else {
            return SYSCALL_EBADF;
        }
        return (long)n;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

static void reset_io(const char *stdin_str, const char *orig_str, const char *diff_str) {
    memset(fds, 0, sizeof(fds));
    errors_len = 0;
    errors[0] = 0;

    fds[0].open = true;
    if (stdin_str) {
        size_t l = strlen(stdin_str);
        memcpy(fds[0].data, stdin_str, l);
        fds[0].len = l;
        fds[0].pos = 0;
    }
    fds[1].open = true;
    fds[2].open = true;

    if (orig_str) {
        size_t l = strlen(orig_str);
        memcpy(fds[4].data, orig_str, l);
        fds[4].len = l;
        fds[4].pos = 0;
    }
    if (diff_str) {
        size_t l = strlen(diff_str);
        memcpy(fds[5].data, diff_str, l);
        fds[5].len = l;
        fds[5].pos = 0;
    }
}

int main(void) {
    printf("Running patch host tests under ASan/UBSan...\n");

    /* Test 1: Simple single-hunk unified diff to output file */
    {
        const char *orig = "apple\nbanana\ncherry\n";
        const char *diff =
            "--- orig.txt\n"
            "+++ orig.txt\n"
            "@@ -1,3 +1,3 @@\n"
            " apple\n"
            "-banana\n"
            "+blueberry\n"
            " cherry\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "-o", "out.txt", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(5, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "apple\nblueberry\ncherry\n"));
        printf("  [PASS] Test 1: Simple unified diff with -o\n");
    }

    /* Test 2: In-place patching */
    {
        const char *orig = "line 1\nline 2\nline 3\n";
        const char *diff =
            "--- file.txt\n"
            "+++ file.txt\n"
            "@@ -2,2 +2,3 @@\n"
            "-line 2\n"
            "+line 2 modified\n"
            "+line 2.5 inserted\n"
            " line 3\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(3, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[4].data, "line 1\nline 2 modified\nline 2.5 inserted\nline 3\n"));
        printf("  [PASS] Test 2: In-place patching modifies target file\n");
    }

    /* Test 3: Multiple hunks with additions and deletions */
    {
        const char *orig = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n";
        const char *diff =
            "--- orig.txt\n"
            "+++ orig.txt\n"
            "@@ -1,3 +1,2 @@\n"
            " one\n"
            "-two\n"
            " three\n"
            "@@ -6,3 +5,4 @@\n"
            " six\n"
            "-seven\n"
            "+SEVEN\n"
            "+SEVEN-B\n"
            " eight\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "-o", "out.txt", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(5, argv);
        assert(rc == 0);
        const char *expected = "one\nthree\nfour\nfive\nsix\nSEVEN\nSEVEN-B\neight\n";
        assert(!strcmp((char *)fds[6].data, expected));
        printf("  [PASS] Test 3: Multiple hunks across the file\n");
    }

    /* Test 4: Reverse patch (-R) */
    {
        const char *patched = "apple\nblueberry\ncherry\n";
        const char *diff =
            "--- orig.txt\n"
            "+++ orig.txt\n"
            "@@ -1,3 +1,3 @@\n"
            " apple\n"
            "-banana\n"
            "+blueberry\n"
            " cherry\n";
        reset_io(NULL, patched, diff);
        char *argv[] = {"patch", "-R", "-o", "out.txt", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(6, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "apple\nbanana\ncherry\n"));
        printf("  [PASS] Test 4: Reverse patch (-R) restores original content\n");
    }

    /* Test 5: Patch from stdin */
    {
        const char *orig = "alpha\nbeta\ngamma\n";
        const char *diff =
            "@@ -2,2 +2,2 @@\n"
            "-beta\n"
            "+BETA\n"
            " gamma\n";
        reset_io(diff, orig, NULL);
        char *argv[] = {"patch", "-o", "out.txt", "orig.txt", NULL};
        int rc = patch_main(4, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "alpha\nBETA\ngamma\n"));
        printf("  [PASS] Test 5: Patch read from stdin\n");
    }

    /* Test 6: Normal diff format (traditional diff: 2c2) */
    {
        const char *orig = "first\nsecond\nthird\n";
        const char *normal_diff =
            "2c2\n"
            "< second\n"
            "---\n"
            "> SECOND\n";
        reset_io(NULL, orig, normal_diff);
        char *argv[] = {"patch", "-o", "out.txt", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(5, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "first\nSECOND\nthird\n"));
        printf("  [PASS] Test 6: Traditional / normal diff format\n");
    }

    /* Test 7: Normal diff format append (2a3,4) and delete (2d1) */
    {
        const char *orig = "first\nsecond\nthird\n";
        const char *normal_diff =
            "2a3,4\n"
            "> added 1\n"
            "> added 2\n";
        reset_io(NULL, orig, normal_diff);
        char *argv[] = {"patch", "-o", "out.txt", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(5, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "first\nsecond\nadded 1\nadded 2\nthird\n"));
        printf("  [PASS] Test 7: Normal diff append action\n");
    }

    /* Test 8: Fail-closed safety: failed hunk leaves file unmodified and returns 1 */
    {
        const char *orig = "first\nWRONG_LINE\nthird\n";
        const char *diff =
            "--- orig.txt\n"
            "+++ orig.txt\n"
            "@@ -1,3 +1,3 @@\n"
            " first\n"
            "-second\n"
            "+SECOND\n"
            " third\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(3, argv);
        assert(rc == 1);
        /* Target file MUST NOT be modified! */
        assert(!strcmp((char *)fds[4].data, "first\nWRONG_LINE\nthird\n"));
        printf("  [PASS] Test 8: Fail-closed on hunk mismatch, file untouched\n");
    }

    /* Test 9: --dry-run mode */
    {
        const char *orig = "first\nsecond\nthird\n";
        const char *diff =
            "@@ -2,1 +2,1 @@\n"
            "-second\n"
            "+SECOND\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "--dry-run", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(4, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[4].data, "first\nsecond\nthird\n"));
        printf("  [PASS] Test 9: --dry-run verifies clean application without file mutation\n");
    }

    /* Test 10: Path stripping with -p2 and target deduction from diff header */
    {
        const char *orig = "header\nbody\nfooter\n";
        const char *diff =
            "--- a/sub/orig.txt\n"
            "+++ b/sub/orig.txt\n"
            "@@ -2,1 +2,1 @@\n"
            "-body\n"
            "+BODY\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "-p2", "-i", "patch.diff", "-o", "out.txt", NULL};
        int rc = patch_main(6, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[6].data, "header\nBODY\nfooter\n"));
        printf("  [PASS] Test 10: -p2 strips leading path and deduces target from header\n");
    }

    /* Test 11: Output to stdout (-o -) */
    {
        const char *orig = "hello\nworld\n";
        const char *diff =
            "@@ -1,2 +1,2 @@\n"
            "-hello\n"
            "+HELLO\n"
            " world\n";
        reset_io(NULL, orig, diff);
        char *argv[] = {"patch", "-s", "-o", "-", "orig.txt", "patch.diff", NULL};
        int rc = patch_main(6, argv);
        assert(rc == 0);
        assert(!strcmp((char *)fds[1].data, "HELLO\nworld\n"));
        printf("  [PASS] Test 11: -o - streams patched output to stdout\n");
    }

    /* Test 12: Help argument returns 0 */
    {
        reset_io(NULL, NULL, NULL);
        char *argv[] = {"patch", "--help", NULL};
        int rc = patch_main(2, argv);
        assert(rc == 0);
        printf("  [PASS] Test 12: --help exits 0\n");
    }

    /* Test 13: Missing operand returns 2 */
    {
        reset_io(NULL, NULL, NULL);
        char *argv[] = {"patch", "-i", "missing", NULL};
        int rc = patch_main(3, argv);
        assert(rc == 2);
        printf("  [PASS] Test 13: Missing file returns 2\n");
    }

    printf("ALL 13 patch host tests PASSED under ASan/UBSan!\n");
    return 0;
}
