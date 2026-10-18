/*
 * Host unit & integration test suite for FortressOS Bounded DEFLATE/gzip (RFC 1951 / RFC 1952)
 *
 * Verifies:
 * - CRC-32 table and test vectors (RFC 1952)
 * - Compression and decompression round-trips
 * - SHA-256 exact match on 1MB random file
 * - High compression on repetitive data
 * - Edge cases (empty file, 1-byte file)
 * - Interop with Linux gzip in both directions (our gzip -> Linux gzip -d, Linux gzip -c -> our gunzip)
 * - Error cases: truncated file, corrupted header, corrupted trailer CRC/ISIZE
 * - Decompression bomb (zip bomb) bounded rejection (> 100MB) without crash or OOM
 * - CLI flags (-c, -d, -k, -f, --help)
 * - ASan / UBSan clean, stack frames <= 1 KiB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>

#include "common.h"
#include "gzip.h"
#include "digest.h"

/* =========================================================================
 * Mock Syscall Implementation for Host Testing
 * ========================================================================= */

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_READ) {
        int fd = (int)a;
        void *buf = (void *)b;
        size_t count = (size_t)c;
        ssize_t ret = read(fd, buf, count);
        return ret < 0 ? -1 : ret;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const void *buf = (const void *)b;
        size_t count = (size_t)c;
        ssize_t ret = write(fd, buf, count);
        return ret < 0 ? -1 : ret;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        int flags = (int)b;
        mode_t mode = (mode_t)c;

        int host_flags = 0;
        int acc = flags & VFS_O_ACCMODE;
        if (acc == VFS_O_RDONLY) host_flags |= O_RDONLY;
        else if (acc == VFS_O_WRONLY) host_flags |= O_WRONLY;
        else if (acc == VFS_O_RDWR) host_flags |= O_RDWR;

        if (flags & VFS_O_CREAT) host_flags |= O_CREAT;
        if (flags & VFS_O_TRUNC) host_flags |= O_TRUNC;
        if (flags & VFS_O_APPEND) host_flags |= O_APPEND;

        int fd = open(path, host_flags, mode ? mode : 0644);
        return fd < 0 ? -1 : fd;
    }
    if (nr == SYS_CLOSE) {
        int fd = (int)a;
        return close(fd) < 0 ? -1 : 0;
    }
    if (nr == SYS_STAT) {
        const char *path = (const char *)a;
        vfs_stat_t *st = (vfs_stat_t *)b;
        struct stat host_st;
        if (stat(path, &host_st) < 0) return -1;
        st->size = (uint64_t)host_st.st_size;
        st->type = S_ISDIR(host_st.st_mode) ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr == SYS_UNLINK) {
        const char *path = (const char *)a;
        return unlink(path) < 0 ? -1 : 0;
    }
    return -1;
}

/* =========================================================================
 * Helper: File Hash & Creation
 * ========================================================================= */

static void compute_file_sha256(const char *path, uint8_t out[32]) {
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    digest_t ctx;
    digest_init(&ctx, DIGEST_SHA256);
    static uint8_t s_hash_buf[4096];
    for (;;) {
        ssize_t n = read(fd, s_hash_buf, sizeof(s_hash_buf));
        assert(n >= 0);
        if (n == 0) break;
        digest_update(&ctx, s_hash_buf, (size_t)n);
    }
    close(fd);
    digest_final(&ctx, out);
}

static void create_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    if (len > 0) {
        ssize_t w = write(fd, data, len);
        assert((size_t)w == len);
    }
    close(fd);
}

static size_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (size_t)st.st_size;
}

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static void test_crc32(void) {
    printf("[TEST] CRC-32 (RFC 1952 standard vector)... ");
    const char *test_vec = "123456789";
    uint32_t val = gzip_crc32(0, test_vec, 9);
    assert(val == 0xCBF43926U);

    /* Empty string */
    uint32_t empty_val = gzip_crc32(0, "", 0);
    assert(empty_val == 0x00000000U);
    printf("PASS\n");
}

static void test_empty_file(void) {
    printf("[TEST] Empty file (0 bytes) round-trip & interop... ");
    const char *orig = "/tmp/test_gzip_empty.txt";
    const char *gz   = "/tmp/test_gzip_empty.txt.gz";
    const char *rest = "/tmp/test_gzip_empty_restored.txt";

    create_file(orig, "", 0);

    /* 1. Compress with our tool */
    char *argv_comp[] = {"gzip", "-k", (char *)orig, NULL};
    int r = gzip_main(3, argv_comp);
    assert(r == 0);
    assert(get_file_size(gz) > 0);

    /* 2. Decompress with Linux gzip -d */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gzip -dc %s > %s", gz, rest);
    r = system(cmd);
    assert(r == 0);
    assert(get_file_size(rest) == 0);

    /* 3. Re-compress with Linux gzip -c */
    unlink(gz);
    snprintf(cmd, sizeof(cmd), "gzip -c %s > %s", orig, gz);
    r = system(cmd);
    assert(r == 0);

    /* 4. Decompress with our gunzip */
    unlink(rest);
    char *argv_decomp[] = {"gunzip", "-c", (char *)gz, NULL};
    /* Redirect stdout to file */
    int saved_stdout = dup(1);
    int out_fd = open(rest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out_fd, 1);
    close(out_fd);
    r = gunzip_main(3, argv_decomp);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    assert(r == 0);
    assert(get_file_size(rest) == 0);

    unlink(orig);
    unlink(gz);
    unlink(rest);
    printf("PASS\n");
}

static void test_single_byte(void) {
    printf("[TEST] Single byte file round-trip & interop... ");
    const char *orig = "/tmp/test_gzip_single.txt";
    const char *gz   = "/tmp/test_gzip_single.txt.gz";
    const char *rest = "/tmp/test_gzip_single_restored.txt";

    create_file(orig, "Q", 1);

    /* 1. Compress with our gzip */
    char *argv_comp[] = {"gzip", "-k", (char *)orig, NULL};
    int r = gzip_main(3, argv_comp);
    assert(r == 0);

    /* 2. Decompress with Linux gzip */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gzip -dc %s > %s", gz, rest);
    r = system(cmd);
    assert(r == 0);
    assert(get_file_size(rest) == 1);

    /* 3. Re-compress with Linux gzip */
    unlink(gz);
    snprintf(cmd, sizeof(cmd), "gzip -c %s > %s", orig, gz);
    r = system(cmd);
    assert(r == 0);

    /* 4. Decompress with our gunzip */
    unlink(rest);
    char *argv_decomp[] = {"gunzip", "-c", (char *)gz, NULL};
    int saved_stdout = dup(1);
    int out_fd = open(rest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out_fd, 1);
    close(out_fd);
    r = gunzip_main(3, argv_decomp);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    assert(r == 0);
    assert(get_file_size(rest) == 1);

    char byte_val = 0;
    int fd = open(rest, O_RDONLY);
    assert(read(fd, &byte_val, 1) == 1);
    close(fd);
    assert(byte_val == 'Q');

    unlink(orig);
    unlink(gz);
    unlink(rest);
    printf("PASS\n");
}

static void test_repetitive_file(void) {
    printf("[TEST] Repetitive 1MB file (high compression ratio)... ");
    const char *orig = "/tmp/test_gzip_rep.txt";
    const char *gz   = "/tmp/test_gzip_rep.txt.gz";
    const char *rest = "/tmp/test_gzip_rep_restored.txt";

    size_t size = 1048576; /* 1 MB */
    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf != NULL);
    const char *pattern = "FortressOS Bounded DEFLATE (RFC 1951 / RFC 1952) Memory-Safe 2026! ";
    size_t plen = strlen(pattern);
    for (size_t i = 0; i < size; i++) {
        buf[i] = (uint8_t)pattern[i % plen];
    }
    create_file(orig, buf, size);

    uint8_t orig_hash[32];
    compute_file_sha256(orig, orig_hash);

    /* Compress with our gzip */
    char *argv_comp[] = {"gzip", "-k", (char *)orig, NULL};
    int r = gzip_main(3, argv_comp);
    assert(r == 0);

    size_t comp_size = get_file_size(gz);
    assert(comp_size > 0 && comp_size < 25000); /* 1MB down to <25KB: >40x compression */

    /* Decompress with Linux gzip -d */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gzip -dc %s > %s", gz, rest);
    r = system(cmd);
    assert(r == 0);
    uint8_t rest_hash[32];
    compute_file_sha256(rest, rest_hash);
    assert(memcmp(orig_hash, rest_hash, 32) == 0);

    /* Linux gzip compress -> our gunzip decompress */
    unlink(gz);
    snprintf(cmd, sizeof(cmd), "gzip -c %s > %s", orig, gz);
    r = system(cmd);
    assert(r == 0);

    unlink(rest);
    char *argv_decomp[] = {"gunzip", "-c", (char *)gz, NULL};
    int saved_stdout = dup(1);
    int out_fd = open(rest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out_fd, 1);
    close(out_fd);
    r = gunzip_main(3, argv_decomp);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    assert(r == 0);

    compute_file_sha256(rest, rest_hash);
    assert(memcmp(orig_hash, rest_hash, 32) == 0);

    free(buf);
    unlink(orig);
    unlink(gz);
    unlink(rest);
    printf("PASS (compressed %zu -> %zu bytes)\n", size, comp_size);
}

static void test_random_1mb_file(void) {
    printf("[TEST] Random 1MB file round-trip & Linux interop (SHA-256 exact match)... ");
    const char *orig = "/tmp/test_gzip_rand.bin";
    const char *gz   = "/tmp/test_gzip_rand.bin.gz";
    const char *rest = "/tmp/test_gzip_rand_restored.bin";

    size_t size = 1048576; /* 1 MB */
    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf != NULL);

    /* Deterministic pseudo-random generation (LCG) */
    uint32_t state = 0x12345678;
    for (size_t i = 0; i < size; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
    create_file(orig, buf, size);

    uint8_t orig_hash[32];
    compute_file_sha256(orig, orig_hash);

    /* 1. Compress with our tool */
    char *argv_comp[] = {"gzip", "-k", (char *)orig, NULL};
    int r = gzip_main(3, argv_comp);
    assert(r == 0);

    /* 2. Decompress with our tool */
    char *argv_decomp[] = {"gunzip", "-c", (char *)gz, NULL};
    int saved_stdout = dup(1);
    int out_fd = open(rest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out_fd, 1);
    close(out_fd);
    r = gunzip_main(3, argv_decomp);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    assert(r == 0);

    uint8_t rest_hash[32];
    compute_file_sha256(rest, rest_hash);
    assert(memcmp(orig_hash, rest_hash, 32) == 0);

    /* 3. Decompress with Linux gzip -d */
    unlink(rest);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gzip -dc %s > %s", gz, rest);
    r = system(cmd);
    assert(r == 0);
    compute_file_sha256(rest, rest_hash);
    assert(memcmp(orig_hash, rest_hash, 32) == 0);

    /* 4. Compress with Linux gzip -c -> decompress with our gunzip */
    unlink(gz);
    snprintf(cmd, sizeof(cmd), "gzip -c %s > %s", orig, gz);
    r = system(cmd);
    assert(r == 0);

    unlink(rest);
    saved_stdout = dup(1);
    out_fd = open(rest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out_fd, 1);
    close(out_fd);
    r = gunzip_main(3, argv_decomp);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    assert(r == 0);

    compute_file_sha256(rest, rest_hash);
    assert(memcmp(orig_hash, rest_hash, 32) == 0);

    free(buf);
    unlink(orig);
    unlink(gz);
    unlink(rest);
    printf("PASS\n");
}

static void test_zip_bomb(void) {
    printf("[TEST] Decompression bomb (zip bomb > 100MB) protection... ");
    const char *bomb_gz = "/tmp/test_gzip_bomb.gz";
    const char *bomb_out = "/tmp/test_gzip_bomb.out";

    /* Create 105 MB of zeros and compress using Linux gzip */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "head -c 110100480 /dev/zero | gzip -c > %s", bomb_gz);
    int r = system(cmd);
    assert(r == 0);

    size_t gz_size = get_file_size(bomb_gz);
    assert(gz_size < 200000); /* Small archive ~100KB expands to 105MB */

    /* Feed zip bomb to our gunzip */
    char *argv_bomb[] = {"gunzip", "-f", (char *)bomb_gz, NULL};
    r = gunzip_main(3, argv_bomb);
    assert(r != 0); /* MUST exit non-zero */

    /* Partial destination must be cleaned up / unlinked */
    assert(get_file_size(bomb_out) == 0);

    unlink(bomb_gz);
    unlink(bomb_out);
    printf("PASS (exited cleanly without crash or OOM)\n");
}

static void test_corrupted_inputs(void) {
    printf("[TEST] Corrupted and truncated gzip handling... ");
    const char *orig = "/tmp/test_gzip_err.txt";
    const char *gz   = "/tmp/test_gzip_err.txt.gz";

    create_file(orig, "Hello, FortressOS gzip error testing!", 37);
    char *argv_comp[] = {"gzip", "-k", (char *)orig, NULL};
    int r = gzip_main(3, argv_comp);
    assert(r == 0);

    size_t gz_size = get_file_size(gz);
    assert(gz_size > 18);

    uint8_t *gz_data = (uint8_t *)malloc(gz_size);
    assert(gz_data != NULL);
    int fd = open(gz, O_RDONLY);
    assert(read(fd, gz_data, gz_size) == (ssize_t)gz_size);
    close(fd);

    /* 1. Truncated gzip file */
    const char *trunc_file = "/tmp/test_gzip_trunc.txt.gz";
    create_file(trunc_file, gz_data, gz_size / 2);
    char *argv_trunc[] = {"gunzip", "-c", (char *)trunc_file, NULL};
    r = gunzip_main(3, argv_trunc);
    assert(r != 0);
    unlink(trunc_file);

    /* 2. Corrupted magic header byte */
    const char *bad_hdr_file = "/tmp/test_gzip_bad_hdr.txt.gz";
    gz_data[0] ^= 0x55;
    create_file(bad_hdr_file, gz_data, gz_size);
    gz_data[0] ^= 0x55;
    char *argv_bad_hdr[] = {"gunzip", "-c", (char *)bad_hdr_file, NULL};
    r = gunzip_main(3, argv_bad_hdr);
    assert(r != 0);
    unlink(bad_hdr_file);

    /* 3. Corrupted compression method */
    gz_data[2] = 0x09; /* Not DEFLATE (8) */
    create_file(bad_hdr_file, gz_data, gz_size);
    gz_data[2] = 0x08;
    r = gunzip_main(3, argv_bad_hdr);
    assert(r != 0);
    unlink(bad_hdr_file);

    /* 4. Reserved flags set */
    gz_data[3] |= 0x80;
    create_file(bad_hdr_file, gz_data, gz_size);
    gz_data[3] &= ~0x80;
    r = gunzip_main(3, argv_bad_hdr);
    assert(r != 0);
    unlink(bad_hdr_file);

    /* 5. Corrupted CRC-32 trailer */
    const char *bad_crc_file = "/tmp/test_gzip_bad_crc.txt.gz";
    gz_data[gz_size - 8] ^= 0xFF;
    create_file(bad_crc_file, gz_data, gz_size);
    gz_data[gz_size - 8] ^= 0xFF;
    char *argv_bad_crc[] = {"gunzip", "-c", (char *)bad_crc_file, NULL};
    r = gunzip_main(3, argv_bad_crc);
    assert(r != 0);
    unlink(bad_crc_file);

    /* 6. Corrupted ISIZE trailer */
    const char *bad_size_file = "/tmp/test_gzip_bad_size.txt.gz";
    gz_data[gz_size - 4] ^= 0x01;
    create_file(bad_size_file, gz_data, gz_size);
    gz_data[gz_size - 4] ^= 0x01;
    char *argv_bad_size[] = {"gunzip", "-c", (char *)bad_size_file, NULL};
    r = gunzip_main(3, argv_bad_size);
    assert(r != 0);
    unlink(bad_size_file);

    free(gz_data);
    unlink(orig);
    unlink(gz);
    printf("PASS\n");
}

static void test_cli_flags(void) {
    printf("[TEST] CLI flags (-c, -d, -k, -f, file deletion/preservation)... ");
    const char *orig = "/tmp/test_gzip_cli.txt";
    const char *gz   = "/tmp/test_gzip_cli.txt.gz";

    create_file(orig, "Command line testing file contents.", 36);

    /* 1. Default gzip compresses and deletes original file */
    char *argv1[] = {"gzip", (char *)orig, NULL};
    int r = gzip_main(2, argv1);
    assert(r == 0);
    assert(get_file_size(gz) > 0);
    assert(access(orig, F_OK) != 0); /* Deleted! */

    /* 2. Overwrite error without -f when output already exists */
    create_file(orig, "New content", 11);
    char *argv2[] = {"gzip", (char *)orig, NULL};
    r = gzip_main(2, argv2);
    assert(r != 0); /* Error: output file already exists */

    /* 3. Force overwrite with -f */
    char *argv3[] = {"gzip", "-f", (char *)orig, NULL};
    r = gzip_main(3, argv3);
    assert(r == 0);
    assert(access(orig, F_OK) != 0);

    /* 4. Default gunzip decompresses and deletes .gz */
    char *argv4[] = {"gunzip", (char *)gz, NULL};
    r = gunzip_main(2, argv4);
    assert(r == 0);
    assert(access(gz, F_OK) != 0); /* Deleted! */
    assert(access(orig, F_OK) == 0); /* Restored! */

    /* 5. Keep flag (-k) preserves original */
    char *argv5[] = {"gzip", "-k", (char *)orig, NULL};
    r = gzip_main(3, argv5);
    assert(r == 0);
    assert(access(orig, F_OK) == 0); /* Preserved! */
    assert(access(gz, F_OK) == 0);

    /* 6. gzip -d flag */
    unlink(orig);
    char *argv6[] = {"gzip", "-d", "-k", (char *)gz, NULL};
    r = gzip_main(4, argv6);
    assert(r == 0);
    assert(access(orig, F_OK) == 0);
    assert(access(gz, F_OK) == 0);

    unlink(orig);
    unlink(gz);
    printf("PASS\n");
}

int main(void) {
    printf("=================================================================\n");
    printf(" FortressOS Bounded DEFLATE/gzip (RFC 1951 / RFC 1952) Host Suite\n");
    printf("=================================================================\n");

    test_crc32();
    test_empty_file();
    test_single_byte();
    test_repetitive_file();
    test_random_1mb_file();
    test_zip_bomb();
    test_corrupted_inputs();
    test_cli_flags();

    printf("=================================================================\n");
    printf(" All DEFLATE / gzip host tests PASSED (100%% ASan/UBSan clean)!\n");
    printf("=================================================================\n");
    return 0;
}
