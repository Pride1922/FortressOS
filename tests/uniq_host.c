/* Host unit test suite for uniq with mocked syscalls under ASan/UBSan */
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

static void reset_io(const char *stdin_str) {
    memset(fds, 0, sizeof(fds));
    size_t in_len = stdin_str ? strlen(stdin_str) : 0;
    fds[0] = (input_t){true, (const unsigned char *)stdin_str, in_len, 0};
    fds[1].open = fds[2].open = true;
    for (int i = 0; i < 4; i++) {
        file_data[i] = NULL;
        file_len[i] = 0;
    }
    output_len = errors_len = file_out_len = 0;
    output[0] = errors[0] = file_out_buf[0] = 0;
}

static int run_uniq(int argc, char **argv) {
    return uniq_main(argc, argv);
}

int main(void) {
    printf("Running uniq host tests under ASan/UBSan...\n");

    /* 1. Basic deduplication of adjacent duplicate lines */
    {
        const char *sample = "apple\napple\nbanana\ncherry\ncherry\ncherry\n";
        reset_io(sample);
        char *argv[] = {"uniq", NULL};
        int ret = run_uniq(1, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "apple\nbanana\ncherry\n"));
    }

    /* 2. Count (-c) */
    {
        const char *sample = "apple\napple\nbanana\ncherry\ncherry\ncherry\n";
        reset_io(sample);
        char *argv[] = {"uniq", "-c", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(strstr((char *)output, "2 apple\n") != NULL);
        assert(strstr((char *)output, "1 banana\n") != NULL);
        assert(strstr((char *)output, "3 cherry\n") != NULL);
    }

    /* 3. Duplicates only (-d) */
    {
        const char *sample = "apple\napple\nbanana\ncherry\ncherry\n";
        reset_io(sample);
        char *argv[] = {"uniq", "-d", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "apple\ncherry\n"));
    }

    /* 4. Unique only (-u) */
    {
        const char *sample = "apple\napple\nbanana\ncherry\ncherry\n";
        reset_io(sample);
        char *argv[] = {"uniq", "-u", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "banana\n"));
    }

    /* 5. Case-insensitive (-i) */
    {
        const char *sample = "Apple\napple\nAPPLE\nbanana\nBanana\n";
        reset_io(sample);
        char *argv[] = {"uniq", "-i", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "Apple\nbanana\n"));
    }

    /* 6. File input operand */
    {
        reset_io(NULL);
        file_data[0] = (const unsigned char *)"line1\nline1\nline2\n";
        file_len[0] = strlen((const char *)file_data[0]);
        char *argv[] = {"uniq", "in_file", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line1\nline2\n"));
    }

    /* 7. File output operand */
    {
        reset_io(NULL);
        file_data[0] = (const unsigned char *)"a\na\nb\n";
        file_len[0] = strlen((const char *)file_data[0]);
        char *argv[] = {"uniq", "in_file", "out_file", NULL};
        int ret = run_uniq(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)file_out_buf, "a\nb\n"));
        assert(output_len == 0); /* output went to out_file */
    }

    /* 8. Help display */
    {
        reset_io(NULL);
        char *argv[] = {"uniq", "--help", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 0);
        assert(strstr((char *)output, "Usage: uniq") != NULL);
    }

    /* 9. Error cases: non-existent file, directory */
    {
        reset_io(NULL);
        char *argv[] = {"uniq", "missing", NULL};
        int ret = run_uniq(2, argv);
        assert(ret == 1);
        assert(strstr(errors, "cannot open missing") != NULL);

        reset_io(NULL);
        char *argv_dir[] = {"uniq", "dir", NULL};
        ret = run_uniq(2, argv_dir);
        assert(ret == 1);
        assert(strstr(errors, "is a directory dir") != NULL);
    }

    /* 10. CRLF stripping and partial final line */
    {
        const char *sample = "foo\r\nfoo\r\nbar\r\nbaz";
        reset_io(sample);
        char *argv[] = {"uniq", NULL};
        int ret = run_uniq(1, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "foo\nbar\nbaz\n"));
    }

    printf(">>> ALL UNIQ HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
