/* Actual freestanding tools with mocked syscalls; no pipe/scheduler claims. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

typedef struct { bool open; const unsigned char *data; size_t len, pos; } input_t;
static input_t fds[32];
static const unsigned char *file_data[2];
static size_t file_len[2];
static unsigned char output[600000], input[300000];
static size_t output_len;
static char errors[4096];
static size_t errors_len, read_chunk, write_chunk;
static int reads, writes, opens, closes, stats;
static int fail_read, fail_write, fail_close, fail_open;
static long write_error;
static bool stderr_closed, seed_file, seed_total;

/* Test-only seeds exercise the real diagnostic/row paths, not 16 EiB of data. */
void tool_test_wc_seed(wc_counts_t *counts, bool total) {
    if ((total && seed_total) || (!total && seed_file)) counts->bytes = UINT64_MAX;
}

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
        int file = !strcmp((const char *)a, "f2");
        for (int i = 0; i < 32; i++) if (!fds[i].open) {
            fds[i] = (input_t){true, file_data[file], file_len[file], 0};
            return i;
        }
        return SYSCALL_EMFILE;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        closes++; fds[a].open = false;
        return fail_close ? SYSCALL_EIO : 0;
    }
    if (nr == SYS_READ) {
        reads++;
        if (reads == fail_read) return SYSCALL_EIO;
        if (a >= 32 || !fds[a].open || !fds[a].data) return SYSCALL_EBADF;
        input_t *fd = &fds[a];
        size_t n = fd->len - fd->pos;
        if (n > c) n = c;
        if (n > read_chunk) n = read_chunk;
        memcpy((void *)b, fd->data + fd->pos, n); fd->pos += n;
        return (long)n;
    }
    if (nr == SYS_WRITE) {
        assert(a == 1 || a == 2);
        if (!fds[a].open || fds[a].data) return SYSCALL_EBADF;
        if (a == 2 && stderr_closed) return SYSCALL_EBADF;
        if (a == 1 && ++writes == fail_write) return write_error;
        size_t n = c < write_chunk ? c : write_chunk;
        if (a == 1) {
            assert(output_len + n <= sizeof(output));
            memcpy(output + output_len, (const void *)b, n); output_len += n;
        } else {
            assert(errors_len + n < sizeof(errors));
            memcpy(errors + errors_len, (const void *)b, n); errors_len += n;
            errors[errors_len] = 0;
        }
        return (long)n;
    }
    assert(!"unexpected syscall");
    return SYSCALL_ENOSYS;
}
static void reset(const void *data, size_t n) {
    memset(fds, 0, sizeof(fds));
    fds[0] = (input_t){true, data, n, 0};
    fds[1].open = fds[2].open = true;
    file_data[0] = file_data[1] = data; file_len[0] = file_len[1] = n;
    output_len = errors_len = 0; errors[0] = 0;
    reads = writes = opens = closes = stats = 0;
    fail_read = fail_write = fail_close = fail_open = 0;
    write_error = SYSCALL_EPIPE;
    stderr_closed = seed_file = seed_total = false;
    read_chunk = 97; write_chunk = 17;
}
static void bytes_equal(const void *p, size_t n) {
    assert(output_len == n && !memcmp(output, p, n));
}
static int invoke(int (*main_fn)(int, char **), const char *a, const char *b, const char *c) {
    char *argv[] = {"tool", (char *)a, (char *)b, (char *)c, NULL};
    int argc = 1;
    while (argc < 4 && argv[argc]) argc++;
    return main_fn(argc, argv);
}
int main(void) {
    for (size_t i = 0; i < sizeof(input); i++) input[i] = (unsigned char)i;
    reset(input, sizeof(input)); assert(invoke(cat_main, NULL, NULL, NULL) == 0);
    bytes_equal(input, sizeof(input)); assert(!closes && fds[0].open);
    reset("stdin", 5); file_data[0] = (const unsigned char *)"left"; file_len[0] = 4;
    file_data[1] = (const unsigned char *)"right"; file_len[1] = 5;
    assert(invoke(cat_main, "f1", "-", "f2") == 0); bytes_equal("leftstdinright", 14);
    assert(opens == 2 && closes == 2 && fds[0].open);
    reset("x", 1); assert(invoke(cat_main, "-", "-", NULL) == 0); bytes_equal("x", 1);
    for (int low = 0; low < 3; low++) {
        reset("x", 1); fds[low].open = false;
        int result = invoke(cat_main, "f1", NULL, NULL);
        assert(result == (low == 1 ? 1 : 0));
        assert(opens == 1 && closes == 1 && !fds[low].open);
    }
    reset("x", 1); assert(invoke(cat_main, "missing", "f1", NULL) == 1);
    bytes_equal("x", 1); assert(strstr(errors, "cannot open missing"));
    reset("x", 1); assert(invoke(cat_main, "dir", NULL, NULL) == 1 && !reads && !opens);
    reset("x", 1); fail_open = 1;
    assert(invoke(cat_main, "f1", NULL, NULL) == 1 && !reads && !closes);
    reset("abc", 3); fail_close = 1;
    assert(invoke(cat_main, "f1", NULL, NULL) == 1 && strstr(errors, "close error"));
    reset("abc", 3); fail_read = 1;
    assert(invoke(cat_main, "f1", "f2", NULL) == 1 && closes == 2); bytes_equal("abc", 3);
    for (int kind = 0; kind < 3; kind++) {
        reset("abcdef", 6); fail_write = 2; write_chunk = 2;
        write_error = kind == 0 ? SYSCALL_EPIPE : kind == 1 ? 0 : SYSCALL_EIO;
        stderr_closed = true;
        assert(invoke(cat_main, "f1", "f2", NULL) == (kind == 0 ? 141 : 1));
        assert(opens == 1 && closes == 1); bytes_equal("ab", 2);
    }
    reset("abc", 3); assert(invoke(cat_main, "--", "-file", NULL) == 0); bytes_equal("abc", 3);

    static const char lines[] = "one\ntwo\nthree";
    reset(lines, sizeof(lines) - 1); read_chunk = 2;
    assert(invoke(head_main, "-n", "2", NULL) == 0); bytes_equal("one\ntwo\n", 8);
    assert(reads == 4);
    reset(lines, sizeof(lines) - 1);
    assert(invoke(head_main, NULL, NULL, NULL) == 0); bytes_equal(lines, sizeof(lines) - 1);
    reset(lines, sizeof(lines) - 1);
    assert(invoke(head_main, "-c", "5", NULL) == 0); bytes_equal("one\nt", 5); assert(reads == 1);
    reset("x", 1); assert(invoke(head_main, "-n", "0", "missing") == 1 && !reads);
    reset("x", 1); assert(invoke(head_main, "-c", "0", "f1") == 0 && !reads && opens == 1 && closes == 1);
    reset("x", 1); assert(invoke(head_main, "-n", "18446744073709551615", NULL) == 0);
    bytes_equal("x", 1);

    const char *bad[] = {"-1", "+1", "1k", "", "18446744073709551616", "abc"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        reset("x", 1); assert(invoke(head_main, "-c", bad[i], "f1") == 2);
        assert(!reads && !opens && !stats);
    }
    reset("x", 1); assert(invoke(head_main, "-n", NULL, NULL) == 2 && !reads);
    reset("x", 1); assert(invoke(head_main, "-n1", NULL, NULL) == 2 && !reads);
    reset("x", 1); char *mixed[] = {"head", "-n", "1", "-c", "1", "f1"};
    assert(head_main(6, mixed) == 2 && !opens && !reads);

    reset(input, sizeof(input)); assert(invoke(tail_main, "-c", "65536", NULL) == 0);
    bytes_equal(input + sizeof(input) - 65536, 65536);
    reset(input, 5); assert(invoke(tail_main, "-c", "65536", NULL) == 0); bytes_equal(input, 5);
    reset(input, sizeof(input)); assert(invoke(tail_main, "-c", "1", NULL) == 0);
    bytes_equal(input + sizeof(input) - 1, 1);
    reset(lines, sizeof(lines) - 1); assert(invoke(tail_main, "-n", "2", NULL) == 0);
    bytes_equal("two\nthree", 9);
    reset("one\ntwo\n", 8); assert(invoke(tail_main, "-n", "1", NULL) == 0); bytes_equal("two\n", 4);
    reset("one\n\n", 5); assert(invoke(tail_main, "-n", "1", NULL) == 0); bytes_equal("\n", 1);
    reset("", 0); assert(invoke(tail_main, NULL, NULL, NULL) == 0); bytes_equal("", 0);
    for (int mode = 0; mode < 2; mode++) {
        reset(input, sizeof(input)); assert(invoke(tail_main, mode ? "-n" : "-c", "0", NULL) == 0);
        assert(fds[0].pos == sizeof(input) && !output_len && reads > 1);
        reset(input, sizeof(input)); fail_read = 2;
        assert(invoke(tail_main, mode ? "-n" : "-c", "0", NULL) == 1 && !output_len);
    }
    reset("x", 1); assert(invoke(tail_main, "-n", "11", "f1") == 2 && !opens && !stats && !reads);
    reset("x", 1); assert(invoke(tail_main, "-c", "65537", "f1") == 2 && !opens && !stats && !reads);
    memset(input, 'x', 4097); input[4096] = '\n';
    reset(input, 4097); assert(invoke(tail_main, "-n", "1", NULL) == 0); bytes_equal(input, 4097);
    input[4096] = 'x'; input[4097] = '\n'; input[4098] = 'z';
    reset(input, 4099); assert(invoke(tail_main, "-n", "1", NULL) == 0); bytes_equal("z", 1);
    reset(input, 4099); assert(invoke(tail_main, "-n", "2", NULL) == 1 && !output_len);
    assert(strstr(errors, "retained line exceeds 4096 bytes"));
    reset(input, 4097); assert(invoke(tail_main, "-n", "1", NULL) == 1 && !output_len);
    for (size_t i = 0; i < 200; i++) input[i] = i % 2 ? '\n' : 'a';
    reset(input, 200); assert(invoke(tail_main, NULL, NULL, NULL) == 0); bytes_equal(input + 180, 20);
    reset("q\n", 2); assert(invoke(tail_main, "f1", "f2", NULL) == 0); bytes_equal("q\nq\n", 4);

    static const unsigned char words[] = {'a',' ','b','\t','c','\n','d','\r','e','\v','f','\f',0,255};
    reset(words, sizeof(words)); read_chunk = 1;
    assert(invoke(wc_main, NULL, NULL, NULL) == 0); bytes_equal("1 7 14\n", 7);
    reset("", 0); assert(invoke(wc_main, NULL, NULL, NULL) == 0); bytes_equal("0 0 0\n", 6);
    reset("last", 4); assert(invoke(wc_main, "-cll", NULL, NULL) == 0); bytes_equal("0 4\n", 4);
    reset("a b\n", 4); assert(invoke(wc_main, "-w", "-l", NULL) == 0); bytes_equal("1 2\n", 4);
    reset("a b\n", 4); assert(invoke(wc_main, "f1", "f2", NULL) == 0);
    bytes_equal("1 2 4 f1\n1 2 4 f2\n2 4 8 total\n", 30);
    reset("a b\n", 4); fail_read = 1;
    assert(invoke(wc_main, "f1", "f2", NULL) == 1);
    bytes_equal("1 2 4 f2\n1 2 4 total\n", 21);
    reset("x", 1); assert(invoke(wc_main, "-", "-", NULL) == 0);
    bytes_equal("0 1 1 -\n0 0 0 -\n0 1 1 total\n", 28);
    reset("x", 1); seed_file = true;
    assert(invoke(wc_main, NULL, NULL, NULL) == 1 && !output_len && strstr(errors, "counter overflow"));
    reset("x", 1); seed_total = true;
    assert(invoke(wc_main, "f1", "f2", NULL) == 1);
    bytes_equal("0 1 1 f1\n0 1 1 f2\n", 18); assert(strstr(errors, "counter overflow"));
    wc_counts_t counts = {UINT64_MAX, 1, 2}, one = {1, 1, 1};
    assert(!wc_add_counts(&counts, &one) && counts.lines == UINT64_MAX && counts.bytes == 2);
    bool in_word = false; counts = (wc_counts_t){.words = UINT64_MAX};
    assert(!wc_count_chunk(&counts, &in_word, (const unsigned char *)"a", 1) && !in_word);
    reset("", 0); assert(invoke(tail_main, "--help", NULL, NULL) == 0);
    assert(output_len < sizeof(output)); output[output_len] = 0;
    assert(strstr((const char *)output, "always reads to EOF") && !reads && !opens);
    puts("PASS stream tools: actual CLI, byte/line semantics, bounded tail, faults and overflow");
    return 0;
}
