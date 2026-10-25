/* Host unit test suite for base64 with mocked syscalls under ASan/UBSan */
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
        if (!strcmp(name, "missing")) return -VFS_ENOENT;
        ((vfs_stat_t *)b)->type = !strcmp(name, "dir") ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_OPEN) {
        const char *name = (const char *)a;
        if (!strcmp(name, "test_file")) {
            fds[4] = (input_t){true, file_data[0], file_len[0], 0};
            return 4;
        }
        return -VFS_ENOENT;
    }
    if (nr == SYS_CLOSE) {
        assert(a < 32 && fds[a].open);
        fds[a].open = false;
        return 0;
    }
    if (nr == SYS_READ) {
        if (a >= 32 || !fds[a].open || !fds[a].data) return -VFS_EBADF;
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
    return -VFS_EINVAL;
}

static void reset_io(const void *in, size_t in_len) {
    memset(fds, 0, sizeof(fds));
    fds[0] = (input_t){true, (const unsigned char *)in, in_len, 0};
    output_len = 0;
    errors_len = 0;
    memset(output, 0, sizeof(output));
    memset(errors, 0, sizeof(errors));
}

static void test_rfc_vectors(void) {
    printf("[1/7] Testing RFC 4648 canonical vectors...\n");

    const struct {
        const char *plain;
        const char *b64;
    } vectors[] = {
        {"", ""},
        {"f", "Zg==\n"},
        {"fo", "Zm8=\n"},
        {"foo", "Zm9v\n"},
        {"foob", "Zm9vYg==\n"},
        {"fooba", "Zm9vYmE=\n"},
        {"foobar", "Zm9vYmFy\n"}
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        /* Test encoding */
        reset_io(vectors[i].plain, strlen(vectors[i].plain));
        char *argv_enc[] = {"base64", NULL};
        int r = base64_main(1, argv_enc);
        assert(r == 0);
        assert(output_len == strlen(vectors[i].b64));
        assert(memcmp(output, vectors[i].b64, output_len) == 0);

        /* Test decoding */
        if (strlen(vectors[i].b64) > 0) {
            reset_io(vectors[i].b64, strlen(vectors[i].b64));
            char *argv_dec[] = {"base64", "-d", NULL};
            r = base64_main(2, argv_dec);
            assert(r == 0);
            assert(output_len == strlen(vectors[i].plain));
            assert(memcmp(output, vectors[i].plain, output_len) == 0);
        }
    }
    printf("  -> RFC 4648 vectors passed.\n");
}

static void test_wrapping(void) {
    printf("[2/7] Testing line wrapping (-w, --wrap)...\n");

    /* 57 bytes of 'A' produces 76 base64 chars + '\n' = 77 bytes */
    unsigned char buf57[57];
    memset(buf57, 'A', sizeof(buf57));

    reset_io(buf57, sizeof(buf57));
    char *argv1[] = {"base64", NULL};
    int r = base64_main(1, argv1);
    assert(r == 0);
    assert(output_len == 77);
    assert(output[76] == '\n');

    /* 58 bytes produces 76 chars + '\n' + 4 chars + '\n' = 82 bytes */
    unsigned char buf58[58];
    memset(buf58, 'A', sizeof(buf58));

    reset_io(buf58, sizeof(buf58));
    r = base64_main(1, argv1);
    assert(r == 0);
    assert(output_len == 82);
    assert(output[76] == '\n');
    assert(output[81] == '\n');

    /* Custom wrap -w 4 */
    reset_io("foobar", 6);
    char *argv_w4[] = {"base64", "-w4", NULL};
    r = base64_main(2, argv_w4);
    assert(r == 0);
    /* "Zm9vYmFy" with wrap 4: "Zm9v\nYmFy\n" */
    assert(strcmp((char *)output, "Zm9v\nYmFy\n") == 0);

    /* Disable wrap -w 0 */
    reset_io("foobar", 6);
    char *argv_w0[] = {"base64", "-w", "0", NULL};
    r = base64_main(3, argv_w0);
    assert(r == 0);
    /* No trailing newline with wrap 0 */
    assert(strcmp((char *)output, "Zm9vYmFy") == 0);

    printf("  -> Line wrapping passed.\n");
}

static void test_binary_roundtrip(void) {
    printf("[3/7] Testing binary arbitrary round-trip across chunk boundaries...\n");

    /* Test 256 bytes (0x00 to 0xFF) */
    unsigned char bin256[256];
    for (int i = 0; i < 256; i++) bin256[i] = (unsigned char)i;

    reset_io(bin256, sizeof(bin256));
    char *argv_enc[] = {"base64", "-w0", NULL};
    int r = base64_main(2, argv_enc);
    assert(r == 0);

    unsigned char encoded_copy[65536];
    size_t enc_copy_len = output_len;
    memcpy(encoded_copy, output, enc_copy_len);

    reset_io(encoded_copy, enc_copy_len);
    char *argv_dec[] = {"base64", "-d", NULL};
    r = base64_main(2, argv_dec);
    assert(r == 0);
    assert(output_len == sizeof(bin256));
    assert(memcmp(output, bin256, sizeof(bin256)) == 0);

    /* Test large buffer (8192 bytes, exceeding 4096 buffer size) */
    unsigned char large[8192];
    for (size_t i = 0; i < sizeof(large); i++) {
        large[i] = (unsigned char)((i * 37 + 13) & 0xFF);
    }

    reset_io(large, sizeof(large));
    r = base64_main(2, argv_enc);
    assert(r == 0);

    enc_copy_len = output_len;
    memcpy(encoded_copy, output, enc_copy_len);

    reset_io(encoded_copy, enc_copy_len);
    r = base64_main(2, argv_dec);
    assert(r == 0);
    assert(output_len == sizeof(large));
    assert(memcmp(output, large, sizeof(large)) == 0);

    printf("  -> Binary streaming round-trip passed.\n");
}

static void test_decoder_resilience(void) {
    printf("[4/7] Testing decoder whitespace handling and CRLF...\n");

    /* Valid with mixed CR and LF */
    const char *spaced = "Z\r\nm\n9\rv\nY\rm\nF\ny\n\n";
    reset_io(spaced, strlen(spaced));
    char *argv_dec[] = {"base64", "-d", NULL};
    int r = base64_main(2, argv_dec);
    assert(r == 0);
    assert(output_len == 6);
    assert(memcmp(output, "foobar", 6) == 0);

    printf("  -> Whitespace handling passed.\n");
}

static void test_ignore_garbage(void) {
    printf("[5/7] Testing garbage handling with and without -i...\n");

    const char *dirty = "Zm9!v#Y$m%F&y*";

    /* Without -i: must fail with exit code 1 */
    reset_io(dirty, strlen(dirty));
    char *argv_strict[] = {"base64", "-d", NULL};
    int r = base64_main(2, argv_strict);
    assert(r != 0);
    assert(strstr(errors, "invalid input") != NULL);

    /* With -i: must ignore garbage characters and succeed */
    reset_io(dirty, strlen(dirty));
    char *argv_lax[] = {"base64", "-d", "-i", NULL};
    r = base64_main(3, argv_lax);
    assert(r == 0);
    assert(output_len == 6);
    assert(memcmp(output, "foobar", 6) == 0);

    /* Combined flag -di */
    reset_io(dirty, strlen(dirty));
    char *argv_comb[] = {"base64", "-di", NULL};
    r = base64_main(2, argv_comb);
    assert(r == 0);
    assert(output_len == 6);
    assert(memcmp(output, "foobar", 6) == 0);

    printf("  -> Garbage handling passed.\n");
}

static void test_invalid_input(void) {
    printf("[6/7] Testing rejection of truncated or malformed input...\n");

    /* Truncated quantum (single character) */
    reset_io("Z\n", 2);
    char *argv_dec[] = {"base64", "-d", NULL};
    int r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Truncated quantum (two characters) */
    reset_io("Zg\n", 3);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Truncated quantum (three characters) */
    reset_io("Zm8\n", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Padding placed illegally at beginning */
    reset_io("=AAA", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Padding placed at position 1 */
    reset_io("A=AA", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Double padding with mismatched final char */
    reset_io("Zg=A", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* All padding */
    reset_io("====", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    /* Non-zero padding bits */
    reset_io("Zh==", 4);
    r = base64_main(2, argv_dec);
    assert(r != 0);

    printf("  -> Malformed input rejection passed.\n");
}

static void test_cli_files(void) {
    printf("[7/7] Testing file operand and CLI options...\n");

    /* Help option */
    reset_io("", 0);
    char *argv_help[] = {"base64", "--help", NULL};
    int r = base64_main(2, argv_help);
    assert(r == 0);
    assert(strstr((char *)output, "Usage: base64") != NULL);

    /* File input */
    file_data[0] = (const unsigned char *)"hello world";
    file_len[0] = 11;
    reset_io("", 0);
    char *argv_file[] = {"base64", "-w0", "test_file", NULL};
    r = base64_main(3, argv_file);
    assert(r == 0);
    assert(strcmp((char *)output, "aGVsbG8gd29ybGQ=") == 0);

    /* Non-existent file */
    reset_io("", 0);
    char *argv_missing[] = {"base64", "missing", NULL};
    r = base64_main(2, argv_missing);
    assert(r != 0);

    /* Extra operand */
    reset_io("", 0);
    char *argv_extra[] = {"base64", "test_file", "extra_file", NULL};
    r = base64_main(3, argv_extra);
    assert(r != 0);

    printf("  -> CLI options passed.\n");
}

int main(void) {
    printf("=========================================\n");
    printf("  FortressOS Base64 Host Unit Test Suite \n");
    printf("=========================================\n");
    test_rfc_vectors();
    test_wrapping();
    test_binary_roundtrip();
    test_decoder_resilience();
    test_ignore_garbage();
    test_invalid_input();
    test_cli_files();
    printf("\n>>> ALL BASE64 HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
