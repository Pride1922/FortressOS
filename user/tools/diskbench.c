#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "signal_abi.h"

#define BENCH_DEFAULT_WRITE_SIZE  (16ULL * 1024ULL * 1024ULL) /* 16 MiB */
#define BENCH_MAX_WRITE_SIZE      (64ULL * 1024ULL * 1024ULL) /* 64 MiB */
#define BENCH_DEFAULT_META_COUNT  1000
#define BENCH_MAX_META_COUNT      4096
#define BENCH_CHUNK_SIZE          16384
#define BENCH_MAX_PATH            VFS_MAX_PATH

typedef enum {
    TEST_ALL = 0,
    TEST_WRITE = 1,
    TEST_READ = 2,
    TEST_META = 3
} bench_test_t;

typedef struct {
    uint64_t write_size;
    uint32_t meta_count;
    bench_test_t test_filter;
    const char *test_dir;
    const char *mount_point;
    bool comparison_mode;  /* -c */
    bool silent_mode;      /* -s */
} bench_opts_t;

typedef struct {
    const char *fs_type;
    uint32_t block_size;
    const char *mount_flags;
    const char *device_class;
} fs_context_t;

/* Static BSS storage — bounded stack, zero dynamic allocation */
static char s_io_buf[BENCH_CHUNK_SIZE];
static char s_path_buf[BENCH_MAX_PATH];
static char s_dir_buf[BENCH_MAX_PATH];
static char s_cleanup_dir[BENCH_MAX_PATH];
static char s_cleanup_file[BENCH_MAX_PATH];
static uint32_t s_created_meta_count = 0;
static char s_dmesg_buf[16384];

static void out_str(const char *s) {
    if (!s) return;
    tool_write("diskbench", s, tool_length(s));
}

static void out_u64(uint64_t val) {
    char buf[32];
    tool_format_u64(buf, val);
    out_str(buf);
}

static void out_throughput(uint64_t bytes, uint64_t ms) {
    if (ms == 0) ms = 1;
    uint64_t mib_s_x100 = (bytes * 1000ULL * 100ULL) / (ms * 1024ULL * 1024ULL);
    uint64_t whole = mib_s_x100 / 100;
    uint64_t frac = mib_s_x100 % 100;
    out_u64(whole);
    out_str(".");
    if (frac < 10) out_str("0");
    out_u64(frac);
    out_str(" MiB/s");
}

static void out_throughput_raw(uint64_t bytes, uint64_t ms) {
    if (ms == 0) ms = 1;
    uint64_t mib_s_x100 = (bytes * 1000ULL * 100ULL) / (ms * 1024ULL * 1024ULL);
    uint64_t whole = mib_s_x100 / 100;
    uint64_t frac = mib_s_x100 % 100;
    out_u64(whole);
    out_str(".");
    if (frac < 10) out_str("0");
    out_u64(frac);
}

static void err_str(const char *s) {
    if (!s) return;
    size_t len = tool_length(s);
    const unsigned char *p = (const unsigned char *)s;
    while (len) {
        long w = tool_syscall(SYS_WRITE, 2, (uintptr_t)p, len);
        if (w <= 0) break;
        p += w;
        len -= (size_t)w;
    }
}

static uint64_t get_time_ms(void) {
    sysinfo_t info;
    if (tool_syscall(SYS_SYSINFO, (uintptr_t)&info, 0, 0) == 0) {
        uint64_t hz = info.tick_hz ? info.tick_hz : 100;
        return (info.uptime_ticks * 1000ULL) / hz;
    }
    return 0;
}

static void make_meta_filename(const char *dir, uint32_t index, char *out, size_t max_len) {
    size_t dlen = tool_length(dir);
    if (dlen + 12 >= max_len) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < dlen; i++) out[i] = dir[i];
    size_t p = dlen;
    if (p > 0 && out[p - 1] != '/') out[p++] = '/';
    out[p++] = 'm';
    out[p++] = '_';
    out[p++] = '0' + (char)((index / 1000) % 10);
    out[p++] = '0' + (char)((index / 100) % 10);
    out[p++] = '0' + (char)((index / 10) % 10);
    out[p++] = '0' + (char)(index % 10);
    out[p] = '\0';
}

static void cleanup(void) {
    for (uint32_t i = 0; i < s_created_meta_count; i++) {
        make_meta_filename(s_cleanup_dir, i, s_path_buf, sizeof(s_path_buf));
        tool_syscall(SYS_UNLINK, (uintptr_t)s_path_buf, 0, 0);
    }
    s_created_meta_count = 0;

    if (s_cleanup_file[0] != '\0') {
        tool_syscall(SYS_UNLINK, (uintptr_t)s_cleanup_file, 0, 0);
        s_cleanup_file[0] = '\0';
    }

    if (s_cleanup_dir[0] != '\0') {
        tool_syscall(SYS_UNLINK, (uintptr_t)s_cleanup_dir, 0, 0);
        s_cleanup_dir[0] = '\0';
    }
}

static void on_signal(int sig) {
    cleanup();
    tool_syscall(SYS_EXIT, (uintptr_t)(128 + sig), 0, 0);
}

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t hlen = tool_length(haystack);
    size_t nlen = tool_length(needle);
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return true;
    }
    return false;
}

static void detect_fs_context(const char *mount_point, fs_context_t *ctx) {
    ctx->fs_type = "ext2";
    ctx->block_size = 1024;
    ctx->mount_flags = "rw";
    ctx->device_class = "NVMe";

    if (str_contains(mount_point, "journal") || str_contains(mount_point, "ext4")) {
        ctx->fs_type = "ext4";
        ctx->block_size = 4096;
    }

    long n = tool_syscall(SYS_DMESG, (uintptr_t)s_dmesg_buf, sizeof(s_dmesg_buf) - 1, 0);
    if (n > 0) {
        s_dmesg_buf[n] = '\0';
        if (str_contains(s_dmesg_buf, "ext4") && (str_contains(s_dmesg_buf, mount_point) || str_contains(mount_point, "journal"))) {
            ctx->fs_type = "ext4";
            ctx->block_size = 4096;
        } else if (str_contains(s_dmesg_buf, "ext2")) {
            ctx->fs_type = "ext2";
            ctx->block_size = 1024;
        }

        if (str_contains(s_dmesg_buf, "Mounted") && str_contains(s_dmesg_buf, "USB")) {
            ctx->device_class = "USB Mass Storage";
        } else if (str_contains(s_dmesg_buf, "nvme") || str_contains(s_dmesg_buf, "NVMe")) {
            ctx->device_class = "NVMe";
        }

        if (str_contains(s_dmesg_buf, "read-only mount") || str_contains(s_dmesg_buf, "Mount mode: read-only")) {
            ctx->mount_flags = "ro";
        } else {
            ctx->mount_flags = "rw";
        }
    }
}

static bool parse_size(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    uint64_t val = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        int d = s[i++] - '0';
        if (val > (UINT64_MAX - (uint64_t)d) / 10) return false;
        val = val * 10 + (uint64_t)d;
    }
    if (i == 0) return false;
    char unit = s[i];
    if (unit == 'K' || unit == 'k') {
        val *= 1024ULL;
        i++;
    } else if (unit == 'M' || unit == 'm') {
        val *= 1024ULL * 1024ULL;
        i++;
    } else if (unit == 'G' || unit == 'g') {
        val *= 1024ULL * 1024ULL * 1024ULL;
        i++;
    }
    if (s[i] == 'i' || s[i] == 'I') i++;
    if (s[i] == 'B' || s[i] == 'b') i++;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] != '\0') return false;
    if (val > BENCH_MAX_WRITE_SIZE) {
        tool_error("diskbench", "size exceeds maximum 64 MiB", s);
        return false;
    }
    if (val == 0) {
        tool_error("diskbench", "size must be greater than 0", s);
        return false;
    }
    *out = val;
    return true;
}

static bool parse_count(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t val = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        int d = s[i++] - '0';
        if (val > (UINT32_MAX - (uint32_t)d) / 10) return false;
        val = val * 10 + (uint32_t)d;
    }
    if (i == 0 || s[i] != '\0') return false;
    if (val > BENCH_MAX_META_COUNT) {
        tool_error("diskbench", "count exceeds maximum 4096", s);
        return false;
    }
    if (val == 0) {
        tool_error("diskbench", "count must be greater than 0", s);
        return false;
    }
    *out = val;
    return true;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: diskbench [OPTIONS] [MOUNT_POINT]\n"
        "Filesystem throughput and latency benchmark tool.\n\n"
        "Options:\n"
        "  -w SIZE      Write test uses SIZE bytes (default: 16 MiB, max: 64 MiB)\n"
        "  -n COUNT     Metadata test uses COUNT files (default: 1000, max: 4096)\n"
        "  -t TEST      Run only TEST: write | read | meta | all (default: all)\n"
        "  -d DIR       Directory for test files (default: MOUNT_POINT/diskbench-tmp)\n"
        "  -c           Emit comparison-friendly output (single-line per test)\n"
        "  -s           Silent / summary only\n"
        "  --help       Show this help and exit\n";
    out_str(help_text);
    return 0;
}

int diskbench_main(int argc, char **argv) {
    bench_opts_t opts;
    opts.write_size = BENCH_DEFAULT_WRITE_SIZE;
    opts.meta_count = BENCH_DEFAULT_META_COUNT;
    opts.test_filter = TEST_ALL;
    opts.test_dir = NULL;
    opts.mount_point = "/mnt";
    opts.comparison_mode = false;
    opts.silent_mode = false;

    s_cleanup_dir[0] = '\0';
    s_cleanup_file[0] = '\0';
    s_created_meta_count = 0;

    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            return print_help();
        }
        if (tool_equal(arg, "-c")) {
            opts.comparison_mode = true;
            i++;
            continue;
        }
        if (tool_equal(arg, "-s")) {
            opts.silent_mode = true;
            i++;
            continue;
        }
        if (tool_equal(arg, "-w")) {
            if (i + 1 >= argc) {
                tool_error("diskbench", "option requires an argument -- 'w'", NULL);
                return 2;
            }
            if (!parse_size(argv[++i], &opts.write_size)) return 2;
            i++;
            continue;
        }
        if (tool_equal(arg, "-n")) {
            if (i + 1 >= argc) {
                tool_error("diskbench", "option requires an argument -- 'n'", NULL);
                return 2;
            }
            if (!parse_count(argv[++i], &opts.meta_count)) return 2;
            i++;
            continue;
        }
        if (tool_equal(arg, "-t")) {
            if (i + 1 >= argc) {
                tool_error("diskbench", "option requires an argument -- 't'", NULL);
                return 2;
            }
            const char *t = argv[++i];
            if (tool_equal(t, "write")) opts.test_filter = TEST_WRITE;
            else if (tool_equal(t, "read")) opts.test_filter = TEST_READ;
            else if (tool_equal(t, "meta")) opts.test_filter = TEST_META;
            else if (tool_equal(t, "all")) opts.test_filter = TEST_ALL;
            else {
                tool_error("diskbench", "invalid test; choose write, read, meta, or all", t);
                return 2;
            }
            i++;
            continue;
        }
        if (tool_equal(arg, "-d")) {
            if (i + 1 >= argc) {
                tool_error("diskbench", "option requires an argument -- 'd'", NULL);
                return 2;
            }
            opts.test_dir = argv[++i];
            i++;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            tool_error("diskbench", "unrecognized option", arg);
            return 2;
        }
        opts.mount_point = arg;
        i++;
    }

    /* Register signal handlers for clean interruption */
    signal_action_t sa = {.handler = (uintptr_t)on_signal, .mask = 0, .flags = 0, .reserved = 0};
    tool_syscall(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0);
    tool_syscall(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0);

    /* Construct test directory path */
    if (!opts.test_dir) {
        size_t mlen = tool_length(opts.mount_point);
        if (mlen + 16 >= sizeof(s_dir_buf)) {
            tool_error("diskbench", "mount path too long", opts.mount_point);
            return 2;
        }
        for (size_t k = 0; k < mlen; k++) s_dir_buf[k] = opts.mount_point[k];
        size_t p = mlen;
        if (p > 0 && s_dir_buf[p - 1] != '/') s_dir_buf[p++] = '/';
        const char *suffix = "diskbench-tmp";
        for (size_t k = 0; suffix[k]; k++) s_dir_buf[p++] = suffix[k];
        s_dir_buf[p] = '\0';
        opts.test_dir = s_dir_buf;
    }

    /* Record cleanup dir */
    size_t td_len = tool_length(opts.test_dir);
    for (size_t k = 0; k <= td_len && k < sizeof(s_cleanup_dir); k++) {
        s_cleanup_dir[k] = opts.test_dir[k];
    }

    /* Create test directory */
    (void)tool_syscall(SYS_MKDIR, (uintptr_t)opts.test_dir, 0755, 0);

    fs_context_t fs_ctx;
    detect_fs_context(opts.mount_point, &fs_ctx);

    if (opts.comparison_mode) {
        out_str("diskbench context mount=");
        out_str(opts.mount_point);
        out_str(" fs=");
        out_str(fs_ctx.fs_type);
        out_str(" block_size=");
        out_u64(fs_ctx.block_size);
        out_str(" flags=");
        out_str(fs_ctx.mount_flags);
        out_str(" dev=");
        out_str(fs_ctx.device_class);
        out_str("\n");
    } else if (!opts.silent_mode) {
        out_str("=== diskbench: ");
        out_str(opts.mount_point);
        out_str(" ===\nFilesystem:   ");
        out_str(fs_ctx.fs_type);
        out_str(" (block_size=");
        out_u64(fs_ctx.block_size);
        out_str(", flags=");
        out_str(fs_ctx.mount_flags);
        out_str(", dev=");
        out_str(fs_ctx.device_class);
        out_str(")\nDirectory:    ");
        out_str(opts.test_dir);
        out_str("\n\n");
    }

    /* Fill I/O buffer with deterministic test pattern */
    for (size_t k = 0; k < BENCH_CHUNK_SIZE; k++) {
        s_io_buf[k] = (char)(k & 0xFF);
    }

    /* Target data file for read/write tests */
    size_t tf_len = tool_length(opts.test_dir);
    for (size_t k = 0; k < tf_len && k < sizeof(s_cleanup_file) - 16; k++) {
        s_cleanup_file[k] = opts.test_dir[k];
    }
    size_t fp = tf_len;
    if (fp > 0 && s_cleanup_file[fp - 1] != '/') s_cleanup_file[fp++] = '/';
    const char *data_name = "benchfile.dat";
    for (size_t k = 0; data_name[k]; k++) s_cleanup_file[fp++] = data_name[k];
    s_cleanup_file[fp] = '\0';

    uint64_t actual_write_bytes = 0;
    uint64_t write_ms = 0;
    uint64_t actual_read_bytes = 0;
    uint64_t read_ms = 0;
    uint64_t meta_ms = 0;

    /* Write Test */
    if (opts.test_filter == TEST_ALL || opts.test_filter == TEST_WRITE || opts.test_filter == TEST_READ) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)s_cleanup_file, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
        if (fd < 0) {
            err_str("diskbench: cannot create test file in ");
            err_str(opts.test_dir);
            err_str("\n");
            cleanup();
            return 1;
        }

        uint64_t start_t = get_time_ms();
        while (actual_write_bytes < opts.write_size) {
            size_t chunk = BENCH_CHUNK_SIZE;
            if (opts.write_size - actual_write_bytes < (uint64_t)chunk) {
                chunk = (size_t)(opts.write_size - actual_write_bytes);
            }
            long w = tool_syscall(SYS_WRITE, (uintptr_t)fd, (uintptr_t)s_io_buf, chunk);
            if (w <= 0) break;
            actual_write_bytes += (uint64_t)w;
        }
        tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        uint64_t end_t = get_time_ms();
        write_ms = (end_t >= start_t) ? (end_t - start_t) : 1;
        if (write_ms == 0) write_ms = 1;

        if (opts.test_filter != TEST_READ) {
            if (opts.comparison_mode) {
                out_str("diskbench write bytes=");
                out_u64(actual_write_bytes);
                out_str(" time_ms=");
                out_u64(write_ms);
                out_str(" throughput_mibs=");
                out_throughput_raw(actual_write_bytes, write_ms);
                out_str("\n");
            } else if (opts.silent_mode) {
                out_str("write: ");
                out_u64(actual_write_bytes);
                out_str(" bytes in ");
                out_u64(write_ms);
                out_str(" ms (");
                out_throughput(actual_write_bytes, write_ms);
                out_str(")\n");
            } else {
                out_str("Write: ");
                out_u64(actual_write_bytes);
                out_str(" bytes in ");
                out_u64(write_ms);
                out_str(" ms -> ");
                out_throughput(actual_write_bytes, write_ms);
                out_str("\n");
            }
        }
    }

    /* Read Test */
    if (opts.test_filter == TEST_ALL || opts.test_filter == TEST_READ) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)s_cleanup_file, VFS_O_RDONLY, 0);
        if (fd >= 0) {
            uint64_t start_t = get_time_ms();
            for (;;) {
                long r = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)s_io_buf, BENCH_CHUNK_SIZE);
                if (r <= 0) break;
                actual_read_bytes += (uint64_t)r;
            }
            tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            uint64_t end_t = get_time_ms();
            read_ms = (end_t >= start_t) ? (end_t - start_t) : 1;
            if (read_ms == 0) read_ms = 1;

            if (opts.comparison_mode) {
                out_str("diskbench read bytes=");
                out_u64(actual_read_bytes);
                out_str(" time_ms=");
                out_u64(read_ms);
                out_str(" throughput_mibs=");
                out_throughput_raw(actual_read_bytes, read_ms);
                out_str("\n");
            } else if (opts.silent_mode) {
                out_str("read:  ");
                out_u64(actual_read_bytes);
                out_str(" bytes in ");
                out_u64(read_ms);
                out_str(" ms (");
                out_throughput(actual_read_bytes, read_ms);
                out_str(")\n");
            } else {
                out_str("Read:  ");
                out_u64(actual_read_bytes);
                out_str(" bytes in ");
                out_u64(read_ms);
                out_str(" ms -> ");
                out_throughput(actual_read_bytes, read_ms);
                out_str("\n");
            }
        }
        /* Finished with test file */
        tool_syscall(SYS_UNLINK, (uintptr_t)s_cleanup_file, 0, 0);
        s_cleanup_file[0] = '\0';
    } else if (opts.test_filter == TEST_WRITE) {
        tool_syscall(SYS_UNLINK, (uintptr_t)s_cleanup_file, 0, 0);
        s_cleanup_file[0] = '\0';
    }

    /* Metadata Test */
    if (opts.test_filter == TEST_ALL || opts.test_filter == TEST_META) {
        uint64_t start_t = get_time_ms();
        for (uint32_t k = 0; k < opts.meta_count; k++) {
            make_meta_filename(opts.test_dir, k, s_path_buf, sizeof(s_path_buf));
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)s_path_buf, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
            if (fd >= 0) {
                tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
                s_created_meta_count++;
            }
        }
        for (uint32_t k = 0; k < opts.meta_count; k++) {
            make_meta_filename(opts.test_dir, k, s_path_buf, sizeof(s_path_buf));
            tool_syscall(SYS_UNLINK, (uintptr_t)s_path_buf, 0, 0);
        }
        s_created_meta_count = 0;
        uint64_t end_t = get_time_ms();
        meta_ms = (end_t >= start_t) ? (end_t - start_t) : 1;
        if (meta_ms == 0) meta_ms = 1;

        uint64_t ops_per_sec = (opts.meta_count * 2ULL * 1000ULL) / meta_ms;

        if (opts.comparison_mode) {
            out_str("diskbench meta files=");
            out_u64(opts.meta_count);
            out_str(" time_ms=");
            out_u64(meta_ms);
            out_str(" ops_per_sec=");
            out_u64(ops_per_sec);
            out_str("\n");
        } else if (opts.silent_mode) {
            out_str("meta:  ");
            out_u64(opts.meta_count);
            out_str(" files in ");
            out_u64(meta_ms);
            out_str(" ms (");
            out_u64(ops_per_sec);
            out_str(" ops/s)\n");
        } else {
            out_str("Meta:  ");
            out_u64(opts.meta_count);
            out_str(" files created & unlinked in ");
            out_u64(meta_ms);
            out_str(" ms -> ");
            out_u64(ops_per_sec);
            out_str(" ops/s\n");
        }
    }

    if (!opts.comparison_mode && !opts.silent_mode && opts.test_filter == TEST_ALL) {
        out_str("Summary:\n");
        out_str("  Write: ");
        out_throughput(actual_write_bytes, write_ms);
        out_str("\n  Read:  ");
        out_throughput(actual_read_bytes, read_ms);
        out_str("\n  Meta:  ");
        uint64_t ops_per_sec = (opts.meta_count * 2ULL * 1000ULL) / (meta_ms ? meta_ms : 1);
        out_u64(ops_per_sec);
        out_str(" ops/s\n");
    }

    cleanup();
    return 0;
}
