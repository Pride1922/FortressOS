/* Host unit test suite for awk with mocked syscalls under ASan/UBSan */
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
    void *file_ptr;
} mock_fd_t;

static mock_fd_t fds[32];
static unsigned char output[65536];
static size_t output_len;
static char errors[4096];
static size_t errors_len;

typedef struct {
    char name[64];
    unsigned char data[65536];
    size_t len;
    bool exists;
} mock_file_t;

static mock_file_t files[16];

static void fs_reset(void) {
    memset(files, 0, sizeof(files));
}

static mock_file_t *fs_find(const char *name) {
    for (int i = 0; i < 16; i++) {
        if (files[i].exists && !strcmp(files[i].name, name)) {
            return &files[i];
        }
    }
    return NULL;
}

static mock_file_t *fs_create(const char *name) {
    mock_file_t *f = fs_find(name);
    if (f) return f;
    for (int i = 0; i < 16; i++) {
        if (!files[i].exists) {
            files[i].exists = true;
            size_t nlen = strlen(name);
            if (nlen >= sizeof(files[i].name)) nlen = sizeof(files[i].name) - 1;
            memcpy(files[i].name, name, nlen);
            files[i].name[nlen] = '\0';
            files[i].len = 0;
            return &files[i];
        }
    }
    return NULL;
}

static void set_file_content(const char *name, const char *content) {
    mock_file_t *f = fs_create(name);
    assert(f != NULL);
    size_t len = strlen(content);
    assert(len < sizeof(f->data));
    memcpy(f->data, content, len);
    f->len = len;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)c;
    if (nr == SYS_OPEN) {
        const char *name = (const char *)a;
        int flags = (int)b;

        mock_file_t *f = fs_find(name);
        if (!f) {
            if (flags & VFS_O_CREAT) {
                f = fs_create(name);
            } else {
                return SYSCALL_ENOENT;
            }
        }
        if (!f) return SYSCALL_ENOENT;

        for (int i = 3; i < 32; i++) {
            if (!fds[i].open) {
                fds[i].open = true;
                fds[i].is_write = (flags & (VFS_O_WRONLY | VFS_O_RDWR)) != 0;
                fds[i].pos = 0;
                if (flags & VFS_O_TRUNC) {
                    f->len = 0;
                    fds[i].len = 0;
                } else {
                    fds[i].len = f->len;
                    memcpy(fds[i].data, f->data, f->len);
                }
                fds[i].file_ptr = f;
                return i;
            }
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        mock_file_t *f = (mock_file_t *)fds[a].file_ptr;
        if (f && fds[a].is_write) {
            memcpy(f->data, fds[a].data, fds[a].len);
            f->len = fds[a].len;
            f->data[f->len] = '\0';
        }
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
        assert(a < 32);
        if (a == 1) {
            size_t n = c;
            assert(output_len + n < sizeof(output));
            memcpy(output + output_len, (const void *)b, n);
            output_len += n;
            output[output_len] = '\0';
            return (long)n;
        }
        if (a == 2) {
            size_t n = c;
            assert(errors_len + n < sizeof(errors));
            memcpy(errors + errors_len, (const void *)b, n);
            errors_len += n;
            errors[errors_len] = '\0';
            return (long)n;
        }
        if (!fds[a].open) return SYSCALL_EBADF;
        mock_fd_t *fd = &fds[a];
        size_t n = c;
        assert(fd->len + n < sizeof(fd->data));
        memcpy(fd->data + fd->len, (const void *)b, n);
        fd->len += n;
        return (long)n;
    }
    return 0;
}

static void reset_io(const char *stdin_data) {
    memset(fds, 0, sizeof(fds));
    output_len = 0;
    output[0] = '\0';
    errors_len = 0;
    errors[0] = '\0';

    fds[0].open = true;
    fds[0].is_write = false;
    fds[0].pos = 0;
    if (stdin_data) {
        fds[0].len = strlen(stdin_data);
        memcpy(fds[0].data, stdin_data, fds[0].len);
    } else {
        fds[0].len = 0;
    }

    fds[1].open = true;
    fds[1].is_write = true;
    fds[2].open = true;
    fds[2].is_write = true;
}

static int run_awk(int argc, char **argv) {
    return awk_main(argc, argv);
}

int main(void) {
    printf("[*] Running awk host tests under ASan/UBSan...\n");

    /* Test 1: Basic field printing */
    {
        reset_io("Alice 30\nBob 25\n");
        char *argv[] = {"awk", "{ print $1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "Alice\nBob\n"));
    }

    /* Test 2: Multiple field printing with default OFS */
    {
        reset_io("one two three four\nalpha beta gamma delta\n");
        char *argv[] = {"awk", "{ print $1, $3 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "one three\nalpha gamma\n"));
    }

    /* Test 3: $NF last field */
    {
        reset_io("foo bar baz\napple orange\nsingle\n");
        char *argv[] = {"awk", "{ print $NF }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "baz\norange\nsingle\n"));
    }

    /* Test 4: Whole line $0 */
    {
        reset_io("hello world\nfoo bar\n");
        char *argv[] = {"awk", "{ print $0 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "hello world\nfoo bar\n"));
    }

    /* Test 5: Plain print (defaults to $0) */
    {
        reset_io("line 1\nline 2\n");
        char *argv[] = {"awk", "{ print }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "line 1\nline 2\n"));
    }

    /* Test 6: Custom field separator -F: */
    {
        reset_io("root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:User:/home/user:/bin/sh\n");
        char *argv[] = {"awk", "-F:", "{ print $1, $3 }", NULL};
        int rc = run_awk(3, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "root 0\nuser 1000\n"));
    }

    /* Test 7: NR and NF built-in variables */
    {
        reset_io("first line\nsecond line here\nthird\n");
        char *argv[] = {"awk", "{ print NR, NF }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "1 2\n2 3\n3 1\n"));
    }

    /* Test 8: Expression pattern with NR */
    {
        reset_io("one\ntwo\nthree\nfour\n");
        char *argv[] = {"awk", "NR == 2", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "two\n"));
    }

    /* Test 9: Comparison pattern on fields */
    {
        reset_io("Alice 30\nBob 25\nCharlie 35\n");
        char *argv[] = {"awk", "$2 > 28 { print $1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "Alice\nCharlie\n"));
    }

    /* Test 10: String comparison */
    {
        reset_io("foo 10\nbar 20\nfoo 30\n");
        char *argv[] = {"awk", "$1 == \"foo\" { print $2 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "10\n30\n"));
    }

    /* Test 11: Arithmetic operations */
    {
        reset_io("10 20\n5 15\n");
        char *argv[] = {"awk", "{ print $1 + $2, $2 - $1, $1 * 2, $2 / 2, $2 % 3 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "30 10 20 10 2\n20 10 10 7 0\n"));
    }

    /* Test 12: Unary minus and negation */
    {
        reset_io("10\n0\n-5\n");
        char *argv[] = {"awk", "{ print -$1, !$1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "-10 0\n0 1\n5 0\n"));
    }

    /* Test 13: Logical AND (&&) and OR (||) */
    {
        reset_io("1 1\n1 0\n0 1\n0 0\n");
        char *argv[] = {"awk", "{ print ($1 && $2), ($1 || $2) }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "1 1\n0 1\n0 1\n0 0\n"));
    }

    /* Test 14: String concatenation (adjacency) */
    {
        reset_io("hello world\ngoodbye now\n");
        char *argv[] = {"awk", "{ print $1 \"-\" $2 \"!\" }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "hello-world!\ngoodbye-now!\n"));
    }

    /* Test 15: Regex pattern matching */
    {
        reset_io("apple\nbanana\napricot\nberry\n");
        char *argv[] = {"awk", "/^ap/ { print $1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "apple\napricot\n"));
    }

    /* Test 16: Multiple pattern-action pairs */
    {
        reset_io("start 1\nmiddle 2\nend 3\n");
        char *argv[] = {"awk", "/start/ { print \"BEGIN_TAG\", $2 } /end/ { print \"END_TAG\", $2 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "BEGIN_TAG 1\nEND_TAG 3\n"));
    }

    /* Test 17: printf formatting */
    {
        reset_io("hello 42\nworld 100\n");
        char *argv[] = {"awk", "{ printf \"%s: %04d\n\", $1, $2 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "hello: 0042\nworld: 0100\n"));
    }

    /* Test 18: Field assignment and $0 reconstruction */
    {
        reset_io("foo bar baz\n");
        char *argv[] = {"awk", "{ $2 = \"MODIFIED\"; print }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "foo MODIFIED baz\n"));
    }

    /* Test 19: Field assignment with expression */
    {
        reset_io("item 10\nitem 25\n");
        char *argv[] = {"awk", "{ $2 = $2 + 5; print $1, $2 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "item 15\nitem 30\n"));
    }

    /* Test 20: Missing field evaluation (empty string / 0) */
    {
        reset_io("one two\n");
        char *argv[] = {"awk", "{ print \"<\" $5 \">\", $5 + 10 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "<> 10\n"));
    }

    /* Test 21: File argument reading */
    {
        fs_reset();
        set_file_content("test1.txt", "alpha 1\nbeta 2\n");
        set_file_content("test2.txt", "gamma 3\n");
        reset_io(NULL);
        char *argv[] = {"awk", "{ print $1 }", "test1.txt", "test2.txt", NULL};
        int rc = run_awk(4, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "alpha\nbeta\ngamma\n"));
    }

    /* Test 22: Script via -f option */
    {
        fs_reset();
        set_file_content("script.awk", "{ print $2, $1 }\n");
        reset_io("foo bar\n");
        char *argv[] = {"awk", "-f", "script.awk", NULL};
        int rc = run_awk(3, argv);
        assert(rc == 0);
        assert(!strcmp((const char *)output, "bar foo\n"));
    }

    /* Test 23: Rejection - unsupported function 'length' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ length($1) }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported function 'length'") != NULL);
    }

    /* Test 24: Rejection - unsupported function 'substr' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ substr($1, 1, 2) }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported function 'substr'") != NULL);
    }

    /* Test 25: Rejection - unsupported control flow 'if' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ if ($1 == \"foo\") print }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported control flow 'if'") != NULL);
    }

    /* Test 26: Rejection - unsupported control flow 'for' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ for (i=0; i<10; i++) print }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported control flow 'for'") != NULL);
    }

    /* Test 27: Rejection - unsupported block 'BEGIN' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "BEGIN { print 1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported block 'BEGIN'") != NULL);
    }

    /* Test 28: Rejection - unsupported block 'END' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "END { print 1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "unsupported block 'END'") != NULL);
    }

    /* Test 29: Rejection - unsupported arrays */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ a[$1] = 1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "arrays are unsupported") != NULL);
    }

    /* Test 30: Rejection - unsupported ternary operator */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "{ print $1 ? 1 : 2 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "ternary operator is unsupported") != NULL);
    }

    /* Test 31: Rejection - unsupported option '-v' */
    {
        reset_io("foo\n");
        char *argv[] = {"awk", "-v", "x=1", "{ print x }", NULL};
        int rc = run_awk(4, argv);
        assert(rc == 2);
        assert(strstr(errors, "option '-v' is unsupported") != NULL);
    }

    /* Test 32: Bounded field count limit (64 fields max) */
    {
        /* Construct a line with 65 fields */
        char big_line[512];
        size_t bpos = 0;
        for (int i = 0; i < 65; i++) {
            bpos += snprintf(big_line + bpos, sizeof(big_line) - bpos, "%d ", i);
        }
        big_line[bpos++] = '\n';
        big_line[bpos] = '\0';
        reset_io(big_line);
        char *argv[] = {"awk", "{ print $1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 2);
        assert(strstr(errors, "line exceeds maximum fields (64)") != NULL);
    }

    /* Test 33: Empty input */
    {
        reset_io("");
        char *argv[] = {"awk", "{ print $1 }", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(output_len == 0);
    }

    /* Test 34: Help flag --help */
    {
        reset_io(NULL);
        char *argv[] = {"awk", "--help", NULL};
        int rc = run_awk(2, argv);
        assert(rc == 0);
        assert(strstr((const char *)output, "Usage: awk") != NULL);
    }

    printf("[+] All awk host tests passed successfully (34/34 PASS).\n");
    return 0;
}
