/* Host unit test suite for sed with mocked syscalls under ASan/UBSan */
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

/* Simple in-memory filesystem for testing file operations and -i */
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

static const char *get_file_content(const char *name) {
    mock_file_t *f = fs_find(name);
    if (!f) return NULL;
    f->data[f->len] = '\0';
    return (const char *)f->data;
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
    if (nr == SYS_UNLINK) {
        const char *name = (const char *)a;
        mock_file_t *f = fs_find(name);
        if (f) {
            f->exists = false;
            f->len = 0;
        }
        return 0;
    }
    if (nr == SYS_RENAME) {
        const char *oldname = (const char *)a;
        const char *newname = (const char *)b;
        mock_file_t *old_f = fs_find(oldname);
        if (!old_f) return SYSCALL_ENOENT;

        mock_file_t *new_f = fs_find(newname);
        if (!new_f) new_f = fs_create(newname);
        assert(new_f != NULL);

        memcpy(new_f->data, old_f->data, old_f->len);
        new_f->len = old_f->len;
        new_f->data[new_f->len] = '\0';

        old_f->exists = false;
        old_f->len = 0;
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
        /* Write to mock fd */
        if (!fds[a].open) return SYSCALL_EBADF;
        mock_fd_t *fd = &fds[a];
        size_t n = c;
        assert(fd->len + n < sizeof(fd->data));
        memcpy(fd->data + fd->len, (const void *)b, n);
        fd->len += n;
        fd->data[fd->len] = '\0';
        mock_file_t *f = (mock_file_t *)fd->file_ptr;
        if (f) {
            memcpy(f->data, fd->data, fd->len);
            f->len = fd->len;
            f->data[f->len] = '\0';
        }
        return (long)n;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}

static void reset_io(const char *stdin_str) {
    memset(fds, 0, sizeof(fds));
    size_t in_len = stdin_str ? strlen(stdin_str) : 0;
    fds[0].open = true;
    fds[0].is_write = false;
    fds[0].len = in_len;
    fds[0].pos = 0;
    if (in_len > 0) {
        assert(in_len < sizeof(fds[0].data));
        memcpy(fds[0].data, stdin_str, in_len);
    }
    fds[1].open = fds[2].open = true;
    output_len = 0;
    output[0] = '\0';
    errors_len = 0;
    errors[0] = '\0';
}

static int run_sed(int argc, char **argv) {
    return sed_main(argc, argv);
}

int main(void) {
    printf("Running sed host tests under ASan/UBSan...\n");

    /* 1. Basic substitution without 'g' */
    {
        reset_io("hello world\n");
        char *argv[] = {"sed", "s/world/there/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "hello there\n"));
    }

    /* 2. Substitution only affects first match without 'g' */
    {
        reset_io("aaa\n");
        char *argv[] = {"sed", "s/a/b/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "baa\n"));
    }

    /* 3. Global substitution with 'g' */
    {
        reset_io("aaa\n");
        char *argv[] = {"sed", "s/a/b/g", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "bbb\n"));
    }

    /* 4. Capture group backreference \1 */
    {
        reset_io("hello\n");
        char *argv[] = {"sed", "s/\\(hel\\)lo/\\1p/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "help\n"));
    }

    /* 5. Multiple capture groups \1 and \2 */
    {
        reset_io("first second\n");
        char *argv[] = {"sed", "s/\\([a-z]*\\) \\([a-z]*\\)/\\2 \\1/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "second first\n"));
    }

    /* 6. Whole match backreference & */
    {
        reset_io("world\n");
        char *argv[] = {"sed", "s/world/[&]/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "[world]\n"));
    }

    /* 7. Case-insensitive substitution 'i' */
    {
        reset_io("Hello World\n");
        char *argv[] = {"sed", "s/world/there/i", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "Hello there\n"));
    }

    /* 8. Nth match flag (s/.../.../2) */
    {
        reset_io("foo foo foo\n");
        char *argv[] = {"sed", "s/foo/bar/2", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "foo bar foo\n"));
    }

    /* 9. Anchor ^ matching start of line */
    {
        reset_io("foo\nbar\nbaz\n");
        char *argv[] = {"sed", "s/^/prefix-/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "prefix-foo\nprefix-bar\nprefix-baz\n"));
    }

    /* 10. Anchor $ matching end of line */
    {
        reset_io("foo\nbar\n");
        char *argv[] = {"sed", "s/$/-suffix/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "foo-suffix\nbar-suffix\n"));
    }

    /* 11. Line number addressing: 2p with -n */
    {
        reset_io("line1\nline2\nline3\n");
        char *argv[] = {"sed", "-n", "2p", NULL};
        int ret = run_sed(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line2\n"));
    }

    /* 12. Line range delete: 1,2d */
    {
        reset_io("a\nb\nc\n");
        char *argv[] = {"sed", "1,2d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "c\n"));
    }

    /* 13. Single line delete: 2d */
    {
        reset_io("line1\nline2\nline3\n");
        char *argv[] = {"sed", "2d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line1\nline3\n"));
    }

    /* 14. Pattern addressing: /pattern/d */
    {
        reset_io("apple\nbanana\ncherry\n");
        char *argv[] = {"sed", "/banana/d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "apple\ncherry\n"));
    }

    /* 15. Pattern range addressing: /start/,/end/d */
    {
        reset_io("a\nSTART\nb\nEND\nc\n");
        char *argv[] = {"sed", "/START/,/END/d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "a\nc\n"));
    }

    /* 16. Last line address: $ */
    {
        reset_io("line1\nline2\nline3\n");
        char *argv[] = {"sed", "$d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line1\nline2\n"));
    }

    /* 17. Last line print with -n: $p */
    {
        reset_io("line1\nline2\nline3\n");
        char *argv[] = {"sed", "-n", "$p", NULL};
        int ret = run_sed(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line3\n"));
    }

    /* 18. Range to last line: 2,$d */
    {
        reset_io("line1\nline2\nline3\nline4\n");
        char *argv[] = {"sed", "2,$d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line1\n"));
    }

    /* 19. -n with s/.../.../p */
    {
        reset_io("one\ntwo\nthree\n");
        char *argv[] = {"sed", "-n", "s/two/TWO/p", NULL};
        int ret = run_sed(3, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "TWO\n"));
    }

    /* 20. Multi-line scripts via multiple -e */
    {
        reset_io("apple pie\n");
        char *argv[] = {"sed", "-e", "s/apple/cherry/", "-e", "s/pie/cake/", NULL};
        int ret = run_sed(5, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "cherry cake\n"));
    }

    /* 21. Semicolon separated commands in single script */
    {
        reset_io("apple pie\n");
        char *argv[] = {"sed", "s/apple/cherry/; s/pie/cake/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "cherry cake\n"));
    }

    /* 22. Unsupported commands rejected with clear error */
    {
        const char *unsupported[] = {"a text", "i text", "c text", "y/a/b/", "h", "H", "g", "G", "x", "{", "}", "="};
        for (size_t u = 0; u < sizeof(unsupported) / sizeof(unsupported[0]); u++) {
            reset_io("test line\n");
            char *argv[] = {"sed", (char *)unsupported[u], NULL};
            int ret = run_sed(2, argv);
            assert(ret != 0);
            assert(strstr(errors, "unsupported command") != NULL);
        }
    }

    /* 23. Empty input stream */
    {
        reset_io("");
        char *argv[] = {"sed", "s/a/b/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(output_len == 0);
    }

    /* 24. One-line input */
    {
        reset_io("single line\n");
        char *argv[] = {"sed", "s/single/one/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "one line\n"));
    }

    /* 25. No-match input passes through unchanged */
    {
        reset_io("hello world\n");
        char *argv[] = {"sed", "s/xyz/abc/", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "hello world\n"));
    }

    /* 26. Invert/negate address: 2!d deletes all except line 2 */
    {
        reset_io("line1\nline2\nline3\n");
        char *argv[] = {"sed", "2!d", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "line2\n"));
    }

    /* 27. In-place editing: sed -i */
    {
        fs_reset();
        set_file_content("target.txt", "initial foo text\n");
        reset_io(NULL);
        char *argv[] = {"sed", "-i", "s/foo/bar/", "target.txt", NULL};
        int ret = run_sed(4, argv);
        assert(ret == 0);
        const char *content = get_file_content("target.txt");
        assert(content != NULL);
        assert(!strcmp(content, "initial bar text\n"));
    }

    /* 28. In-place editing with backup: sed -i.bak */
    {
        fs_reset();
        set_file_content("target.txt", "original text\n");
        reset_io(NULL);
        char *argv[] = {"sed", "-i.bak", "s/original/modified/", "target.txt", NULL};
        int ret = run_sed(4, argv);
        assert(ret == 0);
        const char *content = get_file_content("target.txt");
        assert(content != NULL);
        assert(!strcmp(content, "modified text\n"));
        const char *bak_content = get_file_content("target.txt.bak");
        assert(bak_content != NULL);
        assert(!strcmp(bak_content, "original text\n"));
    }

    /* 29. Help output */
    {
        reset_io(NULL);
        char *argv[] = {"sed", "--help", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(strstr((char *)output, "Usage: sed") != NULL);
    }

    /* 30. Arbitrary delimiter: s:foo:bar: */
    {
        reset_io("/usr/local/bin\n");
        char *argv[] = {"sed", "s:/usr/local:/opt:g", NULL};
        int ret = run_sed(2, argv);
        assert(ret == 0);
        assert(!strcmp((char *)output, "/opt/bin\n"));
    }

    printf("ALL 30 sed host tests PASSED successfully under ASan/UBSan!\n");
    return 0;
}
