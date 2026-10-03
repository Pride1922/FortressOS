#include "common.h"
#include "digest.h"

#define TAR_MAX_MEMBERS      256
#define TAR_MAX_DEPTH        16
#define TAR_MAX_FILE_SIZE    (16ULL * 1024 * 1024)   /* 16 MiB */
#define TAR_MAX_TOTAL_SIZE   (64ULL * 1024 * 1024)   /* 64 MiB */
#define TAR_MAX_PATH_LEN     255                     /* < 256 bytes */
#define TAR_MAX_NAME_LEN     63                      /* < 64 bytes */
#define TAR_BLOCK_SIZE       512

typedef struct {
    char path[TAR_MAX_PATH_LEN + 1];
    size_t path_len;
    uint64_t size;
    uint32_t mode;
    bool is_dir;
} tar_member_t;

/* Keep member table and buffers off the small userspace stack */
static tar_member_t members[TAR_MAX_MEMBERS];
static size_t member_count;
static uint64_t total_extracted_size;

static uint8_t sha256_pass1[32];
static uint8_t sha256_pass2[32];
static uint8_t sha256_pass3[32];

static uint8_t tar_block[TAR_BLOCK_SIZE];
static char dest_dir_buf[TAR_MAX_PATH_LEN + 1];

static long read_full(int fd, void *buf, size_t count) {
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)(p + total), count - total);
        if (n < 0) return n;
        if (n == 0) break;
        total += (size_t)n;
    }
    return (long)total;
}

static long write_full(int fd, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        long n = tool_syscall(SYS_WRITE, fd, (uintptr_t)(p + total), count - total);
        if (n < 0) return n;
        if (n == 0) return SYSCALL_EIO;
        total += (size_t)n;
    }
    return (long)total;
}

static bool parse_octal(const uint8_t *p, size_t len, uint64_t *out) {
    size_t i = 0;
    while (i < len && p[i] == ' ') i++;
    if (i == len || p[i] == '\0') {
        *out = 0;
        return true;
    }
    uint64_t val = 0;
    bool found_digit = false;
    while (i < len && p[i] >= '0' && p[i] <= '7') {
        found_digit = true;
        unsigned d = (unsigned)(p[i] - '0');
        if (val > (UINT64_MAX - d) / 8) return false;
        val = (val * 8) + d;
        i++;
    }
    if (!found_digit) return false;
    while (i < len && (p[i] == ' ' || p[i] == '\0')) i++;
    if (i < len && p[i] != '\0' && p[i] != ' ') return false;
    *out = val;
    return true;
}

static bool verify_header_checksum(const uint8_t *block) {
    uint64_t expected = 0;
    if (!parse_octal(block + 148, 8, &expected)) return false;
    uint32_t unsigned_sum = 0;
    int32_t signed_sum = 0;
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        uint8_t b = (i >= 148 && i < 156) ? ' ' : block[i];
        unsigned_sum += b;
        signed_sum += (int8_t)b;
    }
    return (expected == unsigned_sum) || (expected == (uint64_t)signed_sum);
}

static bool is_zero_block(const uint8_t *block) {
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0) return false;
    }
    return true;
}

static bool is_ustar(const uint8_t *block) {
    if (tool_equal((const char *)block + 257, "ustar")) {
        return true;
    }
    /* "ustar\0" or "ustar " */
    if (block[257] == 'u' && block[258] == 's' && block[259] == 't' &&
        block[260] == 'a' && block[261] == 'r') {
        return (block[262] == '\0' || block[262] == ' ');
    }
    return false;
}

static bool extract_raw_path(const uint8_t *block, char *out, size_t out_cap) {
    size_t prefix_len = 0;
    while (prefix_len < 155 && block[345 + prefix_len] != '\0') prefix_len++;
    size_t name_len = 0;
    while (name_len < 100 && block[name_len] != '\0') name_len++;

    if (prefix_len == 0 && name_len == 0) return false;

    size_t total = (prefix_len > 0) ? (prefix_len + 1 + name_len) : name_len;
    if (total >= out_cap) return false;

    size_t pos = 0;
    if (prefix_len > 0) {
        for (size_t i = 0; i < prefix_len; i++) out[pos++] = (char)block[345 + i];
        out[pos++] = '/';
    }
    for (size_t i = 0; i < name_len; i++) out[pos++] = (char)block[i];
    out[pos] = '\0';
    return true;
}

static int parse_and_validate_member(const uint8_t *block, const char *archive_path, bool list_mode) {
    if (block[0] == 0x1f && block[1] == 0x8b) {
        return tool_error("tar", "compressed archives not supported (gzip detected)", archive_path);
    }
    if (block[0] == 'B' && block[1] == 'Z') {
        return tool_error("tar", "compressed archives not supported (bzip2 detected)", archive_path);
    }
    if (block[0] == 0xfd && block[1] == '7') {
        return tool_error("tar", "compressed archives not supported (xz detected)", archive_path);
    }
    if (!is_ustar(block)) {
        return tool_error("tar", "not a valid USTAR archive", archive_path);
    }
    if (!verify_header_checksum(block)) {
        return tool_error("tar", "header checksum mismatch", archive_path);
    }

    char type = (char)block[156];
    if (type == '\0') type = '0';

    if (type == '1') return tool_error("tar", "hard links not supported", NULL);
    if (type == '2') return tool_error("tar", "symlinks not supported", NULL);
    if (type == '3' || type == '4') return tool_error("tar", "device files not supported", NULL);
    if (type == '6') return tool_error("tar", "FIFOs not supported", NULL);
    if (type == 'g' || type == 'x') return tool_error("tar", "PAX extensions not supported", NULL);
    if (type == 'L' || type == 'K' || type == 'S') return tool_error("tar", "GNU extensions not supported", NULL);
    if (type != '0' && type != '5') return tool_error("tar", "unsupported member type", NULL);

    char raw_path[TAR_MAX_PATH_LEN + 1];
    if (!extract_raw_path(block, raw_path, sizeof(raw_path))) {
        return tool_error("tar", "member path exceeds maximum length (255 bytes)", NULL);
    }

    if (raw_path[0] == '/' || raw_path[0] == '\\') {
        return tool_error("tar", "absolute paths not allowed", raw_path);
    }

    size_t raw_len = tool_length(raw_path);
    bool is_dir = (type == '5');
    if (type == '0' && raw_len > 0 && raw_path[raw_len - 1] == '/') {
        return tool_error("tar", "regular file path cannot end with slash", raw_path);
    }
    while (is_dir && raw_len > 0 && raw_path[raw_len - 1] == '/') {
        raw_path[--raw_len] = '\0';
    }
    if (raw_len == 0) {
        return tool_error("tar", "empty member path", NULL);
    }

    /* Validate components */
    char canon_path[TAR_MAX_PATH_LEN + 1];
    size_t canon_pos = 0;
    size_t comp_count = 0;
    size_t comp_start = 0;

    for (size_t i = 0; i <= raw_len; i++) {
        if (i == raw_len || raw_path[i] == '/') {
            size_t comp_len = i - comp_start;
            if (comp_len == 0) {
                return tool_error("tar", "empty path component not allowed", raw_path);
            }
            if (comp_len > TAR_MAX_NAME_LEN) {
                return tool_error("tar", "path component exceeds 63 bytes", raw_path);
            }
            if ((comp_len == 1 && raw_path[comp_start] == '.') ||
                (comp_len == 2 && raw_path[comp_start] == '.' && raw_path[comp_start + 1] == '.')) {
                return tool_error("tar", "path traversal not allowed", raw_path);
            }
            comp_count++;
            if (comp_count > TAR_MAX_DEPTH) {
                return tool_error("tar", "path depth exceeds 16 components", raw_path);
            }
            if (canon_pos > 0) {
                if (canon_pos >= TAR_MAX_PATH_LEN) return tool_error("tar", "path too long", NULL);
                canon_path[canon_pos++] = '/';
            }
            for (size_t k = 0; k < comp_len; k++) {
                if (canon_pos >= TAR_MAX_PATH_LEN) return tool_error("tar", "path too long", NULL);
                canon_path[canon_pos++] = raw_path[comp_start + k];
            }
            comp_start = i + 1;
        }
    }
    canon_path[canon_pos] = '\0';

    uint64_t size = 0;
    if (!parse_octal(block + 124, 12, &size)) {
        return tool_error("tar", "malformed file size in header", canon_path);
    }
    if (is_dir && size != 0) {
        return tool_error("tar", "directory entry has nonzero size", canon_path);
    }
    if (!is_dir) {
        if (size > TAR_MAX_FILE_SIZE) {
            return tool_error("tar", "member size exceeds 16 MiB cap", canon_path);
        }
        if (total_extracted_size + size > TAR_MAX_TOTAL_SIZE) {
            return tool_error("tar", "total archive size exceeds 64 MiB cap", canon_path);
        }
        total_extracted_size += size;
    }

    uint64_t mode = 0;
    (void)parse_octal(block + 100, 8, &mode);

    if (member_count >= TAR_MAX_MEMBERS) {
        return tool_error("tar", "archive member count exceeds 256 cap", NULL);
    }

    /* Duplicate and prefix conflict checks */
    for (size_t i = 0; i < member_count; i++) {
        if (tool_equal(members[i].path, canon_path)) {
            return tool_error("tar", "duplicate member path", canon_path);
        }
        if (!members[i].is_dir) {
            size_t elen = members[i].path_len;
            if (canon_pos > elen && canon_path[elen] == '/') {
                bool match = true;
                for (size_t k = 0; k < elen; k++) {
                    if (canon_path[k] != members[i].path[k]) { match = false; break; }
                }
                if (match) return tool_error("tar", "file/directory prefix conflict", canon_path);
            }
        }
        if (!is_dir) {
            size_t nlen = canon_pos;
            if (members[i].path_len > nlen && members[i].path[nlen] == '/') {
                bool match = true;
                for (size_t k = 0; k < nlen; k++) {
                    if (members[i].path[k] != canon_path[k]) { match = false; break; }
                }
                if (match) return tool_error("tar", "file/directory prefix conflict", members[i].path);
            }
        }
    }

    /* Record member */
    for (size_t k = 0; k <= canon_pos; k++) members[member_count].path[k] = canon_path[k];
    members[member_count].path_len = canon_pos;
    members[member_count].size = size;
    members[member_count].mode = (uint32_t)mode;
    members[member_count].is_dir = is_dir;
    member_count++;

    if (list_mode) {
        if (tool_write("tar", canon_path, canon_pos)) return 1;
        if (is_dir) {
            if (tool_write("tar", "/\n", 2)) return 1;
        } else {
            if (tool_write("tar", "\n", 1)) return 1;
        }
    }

    return 0;
}

static int pass1_validate_archive(const char *archive_path, bool list_mode) {
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)archive_path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0) return tool_error("tar", "cannot open archive", archive_path);

    member_count = 0;
    total_extracted_size = 0;

    digest_t ctx;
    digest_init(&ctx, DIGEST_SHA256);

    bool saw_end_blocks = false;
    int status = 0;

    for (;;) {
        long got = read_full((int)fd, tar_block, TAR_BLOCK_SIZE);
        if (got < 0) { status = tool_error("tar", "read error in archive", archive_path); break; }
        if (got == 0) {
            if (!saw_end_blocks) {
                status = tool_error("tar", "archive truncated: missing two zero end blocks", archive_path);
            }
            break;
        }
        if (got != TAR_BLOCK_SIZE) {
            status = tool_error("tar", "archive truncated: incomplete block", archive_path);
            break;
        }

        if (!digest_update(&ctx, tar_block, TAR_BLOCK_SIZE)) {
            status = tool_error("tar", "digest error", NULL);
            break;
        }

        if (saw_end_blocks) {
            /* Any padding after two zero end blocks must be entirely zero */
            if (!is_zero_block(tar_block)) {
                status = tool_error("tar", "non-zero data after end of archive marker", archive_path);
                break;
            }
            continue;
        }

        if (is_zero_block(tar_block)) {
            /* First zero block: require second zero block immediately */
            got = read_full((int)fd, tar_block, TAR_BLOCK_SIZE);
            if (got != TAR_BLOCK_SIZE) {
                status = tool_error("tar", "archive truncated: expected second zero end block", archive_path);
                break;
            }
            if (!digest_update(&ctx, tar_block, TAR_BLOCK_SIZE)) {
                status = tool_error("tar", "digest error", NULL);
                break;
            }
            if (!is_zero_block(tar_block)) {
                status = tool_error("tar", "malformed end-of-archive marker", archive_path);
                break;
            }
            saw_end_blocks = true;
            continue;
        }

        /* Parse and validate member header */
        int parse_rc = parse_and_validate_member(tar_block, archive_path, list_mode);
        if (parse_rc != 0) {
            status = parse_rc;
            break;
        }

        /* Skip file data blocks */
        tar_member_t *m = &members[member_count - 1];
        if (!m->is_dir && m->size > 0) {
            uint64_t rem = m->size;
            while (rem > 0) {
                size_t chunk = rem > TOOL_BUFFER_SIZE ? TOOL_BUFFER_SIZE : (size_t)rem;
                long r = read_full((int)fd, tool_buffer, chunk);
                if (r < 0 || (size_t)r != chunk) {
                    status = tool_error("tar", "archive truncated in member data", m->path);
                    break;
                }
                if (!digest_update(&ctx, tool_buffer, chunk)) {
                    status = tool_error("tar", "digest error", NULL);
                    break;
                }
                rem -= chunk;
            }
            if (status != 0) break;

            size_t pad = (TAR_BLOCK_SIZE - (m->size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
            if (pad > 0) {
                long r = read_full((int)fd, tar_block, pad);
                if (r < 0 || (size_t)r != pad) {
                    status = tool_error("tar", "archive truncated in member padding", m->path);
                    break;
                }
                if (!digest_update(&ctx, tar_block, pad)) {
                    status = tool_error("tar", "digest error", NULL);
                    break;
                }
            }
        }
    }

    if (tool_syscall(SYS_CLOSE, fd, 0, 0) < 0 && status == 0) {
        status = tool_error("tar", "failed to close archive", archive_path);
    }
    if (status == 0) {
        digest_final(&ctx, sha256_pass1);
    }
    return status;
}

static int pass2_hash_archive(const char *archive_path) {
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)archive_path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0) return tool_error("tar", "cannot reopen archive for validation pass 2", archive_path);

    digest_t ctx;
    digest_init(&ctx, DIGEST_SHA256);

    int status = 0;
    for (;;) {
        long got = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (got < 0) {
            status = tool_error("tar", "read error during validation pass 2", archive_path);
            break;
        }
        if (got == 0) break;
        if (!digest_update(&ctx, tool_buffer, (size_t)got)) {
            status = tool_error("tar", "digest error", NULL);
            break;
        }
    }

    if (tool_syscall(SYS_CLOSE, fd, 0, 0) < 0 && status == 0) {
        status = tool_error("tar", "failed to close archive after pass 2", archive_path);
    }
    if (status == 0) {
        digest_final(&ctx, sha256_pass2);
    }
    return status;
}

static int ensure_parent_dirs(const char *dest, const char *rel) {
    char buf[TAR_MAX_PATH_LEN + 1];
    size_t dlen = tool_length(dest);
    if (dlen >= TAR_MAX_PATH_LEN) return -1;
    for (size_t k = 0; k < dlen; k++) buf[k] = dest[k];
    buf[dlen] = '\0';

    size_t rlen = tool_length(rel);
    for (size_t i = 0; i < rlen; i++) {
        if (rel[i] == '/') {
            if (dlen + 1 + i >= TAR_MAX_PATH_LEN) return -1;
            buf[dlen] = '/';
            for (size_t k = 0; k < i; k++) buf[dlen + 1 + k] = rel[k];
            buf[dlen + 1 + i] = '\0';

            vfs_stat_t st;
            if (tool_syscall(SYS_STAT, (uintptr_t)buf, (uintptr_t)&st, 0) < 0) {
                long ret = tool_syscall(SYS_MKDIR, (uintptr_t)buf, 0755, 0);
                if (ret < 0) {
                    if (tool_syscall(SYS_STAT, (uintptr_t)buf, (uintptr_t)&st, 0) < 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static int pass3_extract_archive(const char *archive_path, const char *dest_dir) {
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)archive_path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0) {
        tool_error("tar", "cannot reopen archive for extraction; retained partial directory:", dest_dir);
        return 1;
    }

    digest_t ctx;
    digest_init(&ctx, DIGEST_SHA256);

    size_t dlen = tool_length(dest_dir);

    for (size_t i = 0; i < member_count; i++) {
        tar_member_t *m = &members[i];

        long got = read_full((int)fd, tar_block, TAR_BLOCK_SIZE);
        if (got != TAR_BLOCK_SIZE) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "archive read error during extraction; retained partial directory:", dest_dir);
            return 1;
        }
        if (!digest_update(&ctx, tar_block, TAR_BLOCK_SIZE)) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "digest error during extraction; retained partial directory:", dest_dir);
            return 1;
        }

        /* Check that header still matches */
        if (!verify_header_checksum(tar_block)) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "archive header changed during extraction; retained partial directory:", dest_dir);
            return 1;
        }

        char full_path[TAR_MAX_PATH_LEN + 1];
        if (dlen + 1 + m->path_len >= TAR_MAX_PATH_LEN) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "extracted path exceeds filesystem limit; retained partial directory:", dest_dir);
            return 1;
        }
        for (size_t k = 0; k < dlen; k++) full_path[k] = dest_dir[k];
        full_path[dlen] = '/';
        for (size_t k = 0; k <= m->path_len; k++) full_path[dlen + 1 + k] = m->path[k];

        if (ensure_parent_dirs(dest_dir, m->path) != 0) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "failed to create parent directories; retained partial directory:", dest_dir);
            return 1;
        }

        if (m->is_dir) {
            vfs_stat_t st;
            if (tool_syscall(SYS_STAT, (uintptr_t)full_path, (uintptr_t)&st, 0) < 0) {
                long ret = tool_syscall(SYS_MKDIR, (uintptr_t)full_path, 0755, 0);
                if (ret < 0) {
                    tool_syscall(SYS_CLOSE, fd, 0, 0);
                    tool_error("tar", "failed to create directory; retained partial directory:", dest_dir);
                    return 1;
                }
            }
        } else {
            uint32_t fmode = (m->mode & 0777) ? (m->mode & 0777) : 0644;
            long out_fd = tool_syscall(SYS_OPEN, (uintptr_t)full_path,
                                       VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_CLOEXEC, fmode);
            if (out_fd < 0) {
                tool_syscall(SYS_CLOSE, fd, 0, 0);
                tool_error("tar", "failed to open output file; retained partial directory:", dest_dir);
                return 1;
            }

            uint64_t rem = m->size;
            bool err = false;
            while (rem > 0) {
                size_t chunk = rem > TOOL_BUFFER_SIZE ? TOOL_BUFFER_SIZE : (size_t)rem;
                long r = read_full((int)fd, tool_buffer, chunk);
                if (r < 0 || (size_t)r != chunk) { err = true; break; }
                if (!digest_update(&ctx, tool_buffer, chunk)) { err = true; break; }
                long w = write_full((int)out_fd, tool_buffer, chunk);
                if (w < 0 || (size_t)w != chunk) { err = true; break; }
                rem -= chunk;
            }
            if (tool_syscall(SYS_CLOSE, out_fd, 0, 0) < 0) err = true;

            if (err) {
                tool_syscall(SYS_CLOSE, fd, 0, 0);
                tool_error("tar", "write error during file extraction; retained partial directory:", dest_dir);
                return 1;
            }

            size_t pad = (TAR_BLOCK_SIZE - (m->size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
            if (pad > 0) {
                long r = read_full((int)fd, tar_block, pad);
                if (r < 0 || (size_t)r != pad) {
                    tool_syscall(SYS_CLOSE, fd, 0, 0);
                    tool_error("tar", "unexpected EOF in padding; retained partial directory:", dest_dir);
                    return 1;
                }
                if (!digest_update(&ctx, tar_block, pad)) {
                    tool_syscall(SYS_CLOSE, fd, 0, 0);
                    tool_error("tar", "digest error in padding; retained partial directory:", dest_dir);
                    return 1;
                }
            }
        }
    }

    /* Read remaining blocks (two zero blocks + padding) */
    for (;;) {
        long got = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (got < 0) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "read error reading end of archive; retained partial directory:", dest_dir);
            return 1;
        }
        if (got == 0) break;
        if (!digest_update(&ctx, tool_buffer, (size_t)got)) {
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            tool_error("tar", "digest error at end of archive; retained partial directory:", dest_dir);
            return 1;
        }
    }

    tool_syscall(SYS_CLOSE, fd, 0, 0);
    digest_final(&ctx, sha256_pass3);

    for (size_t k = 0; k < 32; k++) {
        if (sha256_pass1[k] != sha256_pass3[k]) {
            tool_error("tar", "archive changed during extraction; retained partial directory:", dest_dir);
            return 1;
        }
    }

    return 0;
}

static void print_help(void) {
    const char *help =
        "Usage: tar -tf ARCHIVE\n"
        "       tar -xf ARCHIVE -C NEW_DIR\n\n"
        "Options:\n"
        "  -t           List archive contents\n"
        "  -x           Extract archive members\n"
        "  -f ARCHIVE   Archive file to process\n"
        "  -C NEW_DIR   Destination directory for extraction (must not exist)\n"
        "  --help       Show this help message\n\n"
        "Format & Limits:\n"
        "  Uncompressed POSIX USTAR regular files and directories only.\n"
        "  Maximum 256 members, depth 16, 16 MiB per file, 64 MiB total.\n"
        "  Extraction requires non-existing destination directory.\n";
    (void)tool_write("tar", help, tool_length(help));
}

int tar_main(int argc, char **argv) {
    tool_output_failed = false;
    enum { MODE_NONE, MODE_LIST, MODE_EXTRACT } mode = MODE_NONE;
    const char *archive_path = NULL;
    const char *dest_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            print_help();
            return 0;
        }
        if (tool_equal(arg, "-tf")) {
            if (mode != MODE_NONE && mode != MODE_LIST) {
                tool_error("tar", "cannot combine -t and -x", NULL);
                return 2;
            }
            mode = MODE_LIST;
            if (i + 1 >= argc) {
                tool_error("tar", "option -f requires an argument", NULL);
                return 2;
            }
            archive_path = argv[++i];
        } else if (tool_equal(arg, "-xf")) {
            if (mode != MODE_NONE && mode != MODE_EXTRACT) {
                tool_error("tar", "cannot combine -t and -x", NULL);
                return 2;
            }
            mode = MODE_EXTRACT;
            if (i + 1 >= argc) {
                tool_error("tar", "option -f requires an argument", NULL);
                return 2;
            }
            archive_path = argv[++i];
        } else if (tool_equal(arg, "-t")) {
            if (mode == MODE_EXTRACT) {
                tool_error("tar", "cannot combine -t and -x", NULL);
                return 2;
            }
            mode = MODE_LIST;
        } else if (tool_equal(arg, "-x")) {
            if (mode == MODE_LIST) {
                tool_error("tar", "cannot combine -t and -x", NULL);
                return 2;
            }
            mode = MODE_EXTRACT;
        } else if (tool_equal(arg, "-f")) {
            if (i + 1 >= argc) {
                tool_error("tar", "option -f requires an argument", NULL);
                return 2;
            }
            archive_path = argv[++i];
        } else if (tool_equal(arg, "-C")) {
            if (i + 1 >= argc) {
                tool_error("tar", "option -C requires an argument", NULL);
                return 2;
            }
            dest_dir = argv[++i];
        } else {
            tool_error("tar", "unrecognized option; use --help", arg);
            return 2;
        }
    }

    if (mode == MODE_NONE) {
        tool_error("tar", "must specify action (-t to list or -x to extract)", NULL);
        return 2;
    }
    if (!archive_path) {
        tool_error("tar", "missing archive operand (-f ARCHIVE)", NULL);
        return 2;
    }
    if (mode == MODE_LIST) {
        if (dest_dir) {
            tool_error("tar", "-C destination directory cannot be used with -t", NULL);
            return 2;
        }
        return pass1_validate_archive(archive_path, true);
    }

    /* MODE_EXTRACT */
    if (!dest_dir) {
        tool_error("tar", "extraction requires destination directory (-C NEW_DIR)", NULL);
        return 2;
    }

    size_t dlen = tool_length(dest_dir);
    if (dlen == 0) {
        tool_error("tar", "empty destination directory", NULL);
        return 2;
    }
    if (dlen > TAR_MAX_PATH_LEN) {
        tool_error("tar", "destination path exceeds maximum length", dest_dir);
        return 2;
    }
    for (size_t k = 0; k <= dlen; k++) dest_dir_buf[k] = dest_dir[k];
    while (dlen > 1 && dest_dir_buf[dlen - 1] == '/') {
        dest_dir_buf[--dlen] = '\0';
    }

    /* Reject an existing destination */
    vfs_stat_t st;
    if (tool_syscall(SYS_STAT, (uintptr_t)dest_dir_buf, (uintptr_t)&st, 0) == 0) {
        tool_error("tar", "destination already exists:", dest_dir_buf);
        return 1;
    }

    /* Validation Pass 1 */
    int rc = pass1_validate_archive(archive_path, false);
    if (rc != 0) {
        return rc;
    }

    /* Validation Pass 2: check for source change before creating destination */
    rc = pass2_hash_archive(archive_path);
    if (rc != 0) {
        return rc;
    }

    for (size_t k = 0; k < 32; k++) {
        if (sha256_pass1[k] != sha256_pass2[k]) {
            tool_error("tar", "archive changed between validation passes: aborting before creating destination", archive_path);
            return 1;
        }
    }

    /* Both passes agree: create destination directory */
    long mk = tool_syscall(SYS_MKDIR, (uintptr_t)dest_dir_buf, 0755, 0);
    if (mk < 0) {
        tool_error("tar", "failed to create destination directory", dest_dir_buf);
        return 1;
    }

    /* Pass 3: extract archive */
    return pass3_extract_archive(archive_path, dest_dir_buf);
}
