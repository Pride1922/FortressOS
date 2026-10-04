/* Host unit test suite for grep with mocked syscalls under ASan/UBSan */
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
static int reads, writes, opens, closes, stats;
static int fail_read, fail_write, fail_close, fail_open;
static long write_error;
static size_t read_chunk, write_chunk;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_STAT) {
        stats++;
        const char *name = (const char *)a;
        if (!strcmp(name, "missing")) return SYSCALL_ENOENT;
        ((vfs_stat_t *)b)->type = !strcmp(name, "dir") ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_OPEN) {
        opens++;
        if (fail_open) return SYSCALL_EMFILE;
        assert(b == VFS_O_RDONLY);
        const char *name = (const char *)a;
        int file_idx = 0;
        if (!strcmp(name, "f1")) file_idx = 0;
        else if (!strcmp(name, "f2")) file_idx = 1;
        else if (!strcmp(name, "f3")) file_idx = 2;
        else return SYSCALL_ENOENT;

        for (int i = 3; i < 32; i++) {
            if (!fds[i].open) {
                fds[i] = (input_t){true, file_data[file_idx], file_len[file_idx], 0};
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        closes++;
        fds[a].open = false;
        return fail_close ? SYSCALL_EIO : 0;
    }
    if (nr == SYS_READ) {
        reads++;
        if (reads == fail_read) return SYSCALL_EIO;
        if (a >= 32 || !fds[a].open || !fds[a].data) return SYSCALL_EBADF;
        input_t *fd = &fds[a];
        size_t n = fd->len - fd->pos;
        if (n > c) n = c;
        if (read_chunk && n > read_chunk) n = read_chunk;
        memcpy((void *)b, fd->data + fd->pos, n);
        fd->pos += n;
        return (long)n;
    }
    if (nr == SYS_WRITE) {
        assert(a == 1 || a == 2);
        if (a == 1 && ++writes == fail_write) return write_error;
        size_t n = c;
        if (write_chunk && n > write_chunk) n = write_chunk;
        if (a == 1) {
            assert(output_len + n < sizeof(output));
            memcpy(output + output_len, (const void *)b, n);
            output_len += n;
            output[output_len] = 0;
        } else {
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

static void reset_io(const char *stdin_str) {
    memset(fds, 0, sizeof(fds));
    size_t in_len = stdin_str ? strlen(stdin_str) : 0;
    fds[0] = (input_t){true, (const unsigned char *)stdin_str, in_len, 0};
    fds[1].open = fds[2].open = true;
    for (int i = 0; i < 4; i++) {
        file_data[i] = NULL;
        file_len[i] = 0;
    }
    output_len = errors_len = 0;
    output[0] = errors[0] = 0;
    reads = writes = opens = closes = stats = 0;
    fail_read = fail_write = fail_close = fail_open = 0;
    write_error = SYSCALL_EPIPE;
    read_chunk = 128;
    write_chunk = 64;
}

static void set_file(int idx, const char *data) {
    file_data[idx] = (const unsigned char *)data;
    file_len[idx] = strlen(data);
}

static int run_grep(int argc, char **argv) {
    return grep_main(argc, argv);
}

int main(void) {
    printf("Running grep host tests under ASan/UBSan...\n");

    /* 1. Basic matching from stdin */
    {
        const char *sample = "apple\nbanana\ncherry\n";
        reset_io(sample);
        char *argv[] = {"grep", "banana", NULL};
        int ret = run_grep(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "banana\n"));
    }

    /* 2. Non-matching returns exit code 1 */
    {
        const char *sample = "apple\nbanana\ncherry\n";
        reset_io(sample);
        char *argv[] = {"grep", "orange", NULL};
        int ret = run_grep(2, argv);
        assert(ret == 1);
        assert(output_len == 0);
    }

    /* 3. Case-insensitive (-i) */
    {
        const char *sample = "Apple\nBANANA\ncherry\n";
        reset_io(sample);
        char *argv[] = {"grep", "-i", "banana", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "BANANA\n"));
    }

    /* 4. Invert match (-v) */
    {
        const char *sample = "apple\nbanana\ncherry\n";
        reset_io(sample);
        char *argv[] = {"grep", "-v", "banana", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "apple\ncherry\n"));
    }

    /* 5. Count matching lines (-c) */
    {
        const char *sample = "foo 1\nbar\nfoo 2\nbaz\nfoo 3\n";
        reset_io(sample);
        char *argv[] = {"grep", "-c", "foo", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "3\n"));
    }

    /* 6. Line numbers (-n) */
    {
        const char *sample = "line one\nline two\nline three\n";
        reset_io(sample);
        char *argv[] = {"grep", "-n", "two", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "2:line two\n"));
    }

    /* 7. Quiet mode (-q): exit 0 on match, nothing written */
    {
        const char *sample = "match this line\nother\n";
        reset_io(sample);
        char *argv[] = {"grep", "-q", "match", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(output_len == 0);

        reset_io(sample);
        char *argv_no[] = {"grep", "-q", "nomatch", NULL};
        ret = run_grep(3, argv_no);
        assert(ret == 1);
        assert(output_len == 0);
    }

    /* 8. Anchors: ^ and $ */
    {
        const char *sample = "prefix_foo\nfoo_suffix\nfoo\nbar\n";
        reset_io(sample);
        char *argv_start[] = {"grep", "^foo", NULL};
        int ret = run_grep(2, argv_start);
        assert(ret == 0);
        assert(!strcmp((char *)output, "foo_suffix\nfoo\n"));

        reset_io(sample);
        char *argv_end[] = {"grep", "foo$", NULL};
        ret = run_grep(2, argv_end);
        assert(ret == 0);
        assert(!strcmp((char *)output, "prefix_foo\nfoo\n"));

        reset_io(sample);
        char *argv_exact[] = {"grep", "^foo$", NULL};
        ret = run_grep(2, argv_exact);
        assert(ret == 0);
        assert(!strcmp((char *)output, "foo\n"));
    }

    /* 9. Wildcard . and repetition * */
    {
        const char *sample = "ab\nacb\naccb\naxzb\n";
        reset_io(sample);
        char *argv[] = {"grep", "ac*b", NULL};
        int ret = run_grep(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "ab\nacb\naccb\n"));

        reset_io(sample);
        char *argv_dot[] = {"grep", "a.b", NULL};
        ret = run_grep(2, argv_dot);
        assert(ret == 0);
        assert(!strcmp((char *)output, "acb\n"));
    }

    /* 10. Character classes [0-9], [^0-9] */
    {
        const char *sample = "item1\nitemA\nitem2\n";
        reset_io(sample);
        char *argv_digit[] = {"grep", "item[0-9]", NULL};
        int ret = run_grep(2, argv_digit);
        assert(ret == 0);
        assert(!strcmp((char *)output, "item1\nitem2\n"));

        reset_io(sample);
        char *argv_nondigit[] = {"grep", "item[^0-9]", NULL};
        ret = run_grep(2, argv_nondigit);
        assert(ret == 0);
        assert(!strcmp((char *)output, "itemA\n"));
    }

    /* 11. Fixed strings (-F) */
    {
        const char *sample = "a.b\nacb\na*b\n";
        reset_io(sample);
        char *argv[] = {"grep", "-F", "a.b", NULL};
        int ret = run_grep(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "a.b\n"));
    }

    /* 12. Multiple files and prefixing */
    {
        reset_io(NULL);
        set_file(0, "alpha line\nbeta line\n");
        set_file(1, "gamma line\nalpha second\n");
        char *argv[] = {"grep", "alpha", "f1", "f2", NULL};
        int ret = run_grep(4, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "f1:alpha line\nf2:alpha second\n"));

        /* -h suppresses filename prefix */
        reset_io(NULL);
        set_file(0, "alpha line\nbeta line\n");
        set_file(1, "gamma line\nalpha second\n");
        char *argv_h[] = {"grep", "-h", "alpha", "f1", "f2", NULL};
        ret = run_grep(5, argv_h);
        assert(ret == 0);
        assert(!strcmp((char *)output, "alpha line\nalpha second\n"));

        /* -H forces filename prefix on single file */
        reset_io(NULL);
        set_file(0, "alpha line\n");
        char *argv_H[] = {"grep", "-H", "alpha", "f1", NULL};
        ret = run_grep(4, argv_H);
        assert(ret == 0);
        assert(!strcmp((char *)output, "f1:alpha line\n"));
    }

    /* 13. Files with matches (-l) */
    {
        reset_io(NULL);
        set_file(0, "first alpha\nsecond alpha\n");
        set_file(1, "no match here\n");
        set_file(2, "another alpha\n");
        char *argv[] = {"grep", "-l", "alpha", "f1", "f2", "f3", NULL};
        int ret = run_grep(6, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "f1\nf3\n"));
    }

    /* 14. Error handling: missing pattern, missing file, directory */
    {
        /* Missing pattern */
        reset_io(NULL);
        char *argv_nopat[] = {"grep", NULL};
        int ret = run_grep(1, argv_nopat);
        assert(ret == 2);
        assert(strstr(errors, "missing pattern"));

        /* Missing file */
        reset_io(NULL);
        char *argv_missing[] = {"grep", "foo", "missing", NULL};
        ret = run_grep(3, argv_missing);
        assert(ret == 2);
        assert(strstr(errors, "cannot open missing"));

        /* Directory */
        reset_io(NULL);
        char *argv_dir[] = {"grep", "foo", "dir", NULL};
        ret = run_grep(3, argv_dir);
        assert(ret == 2);
        assert(strstr(errors, "is a directory dir"));
    }

    /* 15. Help display */
    {
        reset_io(NULL);
        char *argv_help[] = {"grep", "--help", NULL};
        int ret = run_grep(2, argv_help);
        assert(ret == 0);
        assert(strstr((char *)output, "Usage: grep"));
    }

    /* 16. Pipeline simulation: CRLF stripping and partial final line */
    {
        const char *crlf_sample = "header\r\neth0 link UP\r\nfooter";
        reset_io(crlf_sample);
        char *argv[] = {"grep", "eth0", NULL};
        int ret = run_grep(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "eth0 link UP\n"));

        /* Partial line at EOF without newline */
        const char *no_nl = "only line";
        reset_io(no_nl);
        char *argv2[] = {"grep", "only", NULL};
        ret = run_grep(2, argv2);
        assert(ret == 0);
        assert(!strcmp((char *)output, "only line\n"));
    }

    /* 17. Overlong 10 KB line with match at byte 8000 (verifies rolling window without false negatives) */
    {
        static char overlong_buf[10005];
        memset(overlong_buf, 'x', sizeof(overlong_buf));
        memcpy(overlong_buf + 8000, "TARGET", 6);
        overlong_buf[10000] = '\n';
        overlong_buf[10001] = '\0';

        /* Verify match found at byte 8000 */
        reset_io(overlong_buf);
        char *argv_found[] = {"grep", "TARGET", NULL};
        int ret = run_grep(2, argv_found);
        assert(ret == 0);
        assert(strstr((char *)output, "TARGET") != NULL);

        /* Verify count */
        reset_io(overlong_buf);
        char *argv_c[] = {"grep", "-c", "TARGET", NULL};
        ret = run_grep(3, argv_c);
        assert(ret == 0);
        assert(!strcmp((char *)output, "1\n"));

        /* Verify quiet mode */
        reset_io(overlong_buf);
        char *argv_q[] = {"grep", "-q", "TARGET", NULL};
        ret = run_grep(3, argv_q);
        assert(ret == 0);

        /* Verify inverted matching: TARGET line inverted gives 0 matches */
        reset_io(overlong_buf);
        char *argv_v[] = {"grep", "-c", "-v", "TARGET", NULL};
        ret = run_grep(4, argv_v);
        assert(ret == 1);
        assert(!strcmp((char *)output, "0\n"));
    }

    /* 18. Pathological multi-star regex under bitmask NFA (tests O(N) linear time and 0 stack recursion) */
    {
        static char rep_buf[4097];
        memset(rep_buf, 'a', 4095);
        rep_buf[4095] = '\n';
        rep_buf[4096] = '\0';

        reset_io(rep_buf);
        /* Pattern with 4 consecutive stars would explode on recursive backtracker */
        char *argv_pathological[] = {"grep", "[a-z]*[a-z]*[a-z]*[a-z]*b", NULL};
        int ret = run_grep(2, argv_pathological);
        assert(ret == 1); /* No 'b' present */

        /* Now with match at the end */
        rep_buf[4094] = 'b';
        reset_io(rep_buf);
        ret = run_grep(2, argv_pathological);
        assert(ret == 0);
    }

    /* 19. Match spanning across 4096-byte chunk boundary */
    {
        static char split_buf[8195];
        memset(split_buf, 'x', sizeof(split_buf));
        /* Place "SPLIT_MATCH" across boundary at 4090 */
        memcpy(split_buf + 4090, "SPLIT_MATCH", 11);
        split_buf[8192] = '\n';
        split_buf[8193] = '\0';

        reset_io(split_buf);
        char *argv_split[] = {"grep", "SPLIT_MATCH", NULL};
        int ret = run_grep(2, argv_split);
        assert(ret == 0);
        assert(strstr((char *)output, "SPLIT_MATCH") != NULL);
    }

    printf(">>> ALL GREP HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
