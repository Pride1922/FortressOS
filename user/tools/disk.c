#include "common.h"
#include "syscall_abi.h"
#include "vfs.h"

#define MAX_MOUNTS 16
#define MAX_BLOCKS 16

/* Static BSS storage — bounded stack, zero dynamic allocation */
static mount_info_t s_mounts[MAX_MOUNTS];
static uint32_t s_mount_count = 0;
static block_info_t s_blocks[MAX_BLOCKS];
static uint32_t s_block_count = 0;

static void out_str(const char *s) {
    if (!s) return;
    tool_write("disk", s, tool_length(s));
}

static void out_u64(uint64_t val) {
    char buf[32];
    tool_format_u64(buf, val);
    out_str(buf);
}

static void out_pad_spaces(size_t count) {
    while (count > 0) {
        size_t n = count > 16 ? 16 : count;
        out_str("                " + (16 - n));
        count -= n;
    }
}

static void out_left(const char *str, size_t width) {
    size_t len = tool_length(str);
    out_str(str);
    if (width > len) {
        out_pad_spaces(width - len);
    }
}

static void out_right(const char *str, size_t width) {
    size_t len = tool_length(str);
    if (width > len) {
        out_pad_spaces(width - len);
    }
    out_str(str);
}

static const char *fs_type_str(uint32_t fs_type) {
    switch (fs_type) {
        case VFS_FS_TARFS: return "TarFS";
        case VFS_FS_EXT2:  return "ext2";
        case VFS_FS_EXT4:  return "ext4";
        default:           return "unknown";
    }
}

static void format_size(char *buf, size_t buf_sz, uint64_t bytes) {
    if (bytes == 0) {
        buf[0] = '0';
        buf[1] = 'B';
        buf[2] = '\0';
        return;
    }
    const char *suffix = "B";
    uint64_t val = bytes;
    if (val >= 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        val /= (1024ULL * 1024ULL * 1024ULL * 1024ULL);
        suffix = "T";
    } else if (val >= 1024ULL * 1024ULL * 1024ULL) {
        val /= (1024ULL * 1024ULL * 1024ULL);
        suffix = "G";
    } else if (val >= 1024ULL * 1024ULL) {
        val /= (1024ULL * 1024ULL);
        suffix = "M";
    } else if (val >= 1024ULL) {
        val /= 1024ULL;
        suffix = "K";
    }
    char num[32];
    tool_format_u64(num, val);
    size_t nlen = tool_length(num);
    size_t slen = tool_length(suffix);
    if (nlen + slen + 1 > buf_sz) return;
    for (size_t i = 0; i < nlen; i++) buf[i] = num[i];
    for (size_t i = 0; i < slen; i++) buf[nlen + i] = suffix[i];
    buf[nlen + slen] = '\0';
}

static void format_sector(char *buf, size_t buf_sz, uint32_t sector_size) {
    if (sector_size == 0) {
        buf[0] = '-';
        buf[1] = '\0';
        return;
    }
    if (sector_size >= 1024 && (sector_size % 1024 == 0)) {
        char num[16];
        tool_format_u64(num, sector_size / 1024);
        size_t nlen = tool_length(num);
        if (nlen + 2 > buf_sz) return;
        for (size_t i = 0; i < nlen; i++) buf[i] = num[i];
        buf[nlen] = 'K';
        buf[nlen + 1] = '\0';
    } else {
        char num[16];
        tool_format_u64(num, sector_size);
        size_t nlen = tool_length(num);
        if (nlen + 2 > buf_sz) return;
        for (size_t i = 0; i < nlen; i++) buf[i] = num[i];
        buf[nlen] = 'B';
        buf[nlen + 1] = '\0';
    }
}

static void format_pct(char *buf, size_t buf_sz, uint64_t used, uint64_t total) {
    if (total == 0) {
        buf[0] = '-';
        buf[1] = '\0';
        return;
    }
    uint64_t pct = (used * 100ULL) / total;
    char num[32];
    tool_format_u64(num, pct);
    size_t nlen = tool_length(num);
    if (nlen + 2 > buf_sz) return;
    for (size_t i = 0; i < nlen; i++) buf[i] = num[i];
    buf[nlen] = '%';
    buf[nlen + 1] = '\0';
}

static void print_help(void) {
    out_str("Usage: disk [subcommand] [options]\n\n");
    out_str("Subcommands:\n");
    out_str("  list  [-c]       List block devices (default)\n");
    out_str("  usage [-c]       Show filesystem disk space and inode usage\n");
    out_str("  bench [options]  Benchmark filesystem throughput\n\n");
    out_str("Options:\n");
    out_str("  -c, --comparison Output comparison-friendly single-line format\n");
    out_str("  -h, --help       Show this help message\n");
}

static int collect_mounts(void) {
    s_mount_count = 0;
    for (uint32_t i = 0; i < MAX_MOUNTS; i++) {
        long res = tool_syscall(SYS_MOUNTINFO, i, (uintptr_t)&s_mounts[s_mount_count], 0);
        if (res == 1) {
            s_mount_count++;
        } else if (res == 0) {
            break;
        } else {
            tool_error("disk", "failed to query mount info", NULL);
            return -1;
        }
    }
    return 0;
}

static int collect_blocks(void) {
    s_block_count = 0;
    for (uint32_t i = 0; i < MAX_BLOCKS; i++) {
        long res = tool_syscall(SYS_BLOCKINFO, i, (uintptr_t)&s_blocks[s_block_count], 0);
        if (res == 1) {
            s_block_count++;
        } else if (res == 0) {
            break;
        } else {
            tool_error("disk", "failed to query block info", NULL);
            return -1;
        }
    }
    return 0;
}

static const char *find_mount_for_dev(const char *dev_name) {
    for (uint32_t i = 0; i < s_mount_count; i++) {
        if (tool_equal(s_mounts[i].source, dev_name)) {
            return s_mounts[i].mount_path;
        }
    }
    return NULL;
}

static int disk_list(bool comparison) {
    if (collect_mounts() < 0) {
        return 1;
    }
    if (collect_blocks() < 0) {
        return 1;
    }

    if (s_block_count == 0) {
        out_str("disk: no block devices found\n");
        return 0;
    }

    if (!comparison) {
        /* Default tabular output */
        out_left("NAME", 14);
        out_str(" ");
        out_right("SIZE", 7);
        out_str(" ");
        out_right("SECTOR", 7);
        out_str("  ");
        out_left("MOUNT", 10);
        out_str("\n");

        for (uint32_t i = 0; i < s_block_count; i++) {
            block_info_t *b = &s_blocks[i];
            char size_buf[16], sector_buf[16];
            format_size(size_buf, sizeof(size_buf), b->size_bytes);
            format_sector(sector_buf, sizeof(sector_buf), b->sector_size);

            const char *mnt = find_mount_for_dev(b->name);
            const char *mnt_str = mnt ? mnt : "—";

            out_left(b->name, 14);
            out_str(" ");
            out_right(size_buf, 7);
            out_str(" ");
            out_right(sector_buf, 7);
            out_str("  ");
            out_left(mnt_str, 10);
            out_str("\n");
        }
    } else {
        /* Comparison key=value single-line format */
        for (uint32_t i = 0; i < s_block_count; i++) {
            block_info_t *b = &s_blocks[i];
            out_str("name=");
            out_str(b->name);
            out_str(" sector=");
            out_u64((uint64_t)b->sector_size);
            out_str(" size=");
            out_u64(b->size_bytes);

            const char *mnt = find_mount_for_dev(b->name);
            if (mnt) {
                out_str(" mount=");
                out_str(mnt);
            }
            out_str("\n");
        }
    }
    return 0;
}

static int disk_usage(bool comparison) {
    if (collect_mounts() < 0) {
        return 1;
    }

    if (s_mount_count == 0) {
        out_str("disk: no mounted filesystems found\n");
        return 0;
    }

    if (!comparison) {
        /* Human-readable table view */
        out_left("MOUNT", 10);
        out_str(" ");
        out_left("SOURCE", 12);
        out_str(" ");
        out_left("FS", 6);
        out_str(" ");
        out_right("SIZE", 7);
        out_str(" ");
        out_right("USED", 7);
        out_str(" ");
        out_right("AVAIL", 7);
        out_str(" ");
        out_right("USE%", 6);
        out_str(" ");
        out_right("INODES", 8);
        out_str(" ");
        out_right("IUSE%", 6);
        out_str("\n");

        for (uint32_t i = 0; i < s_mount_count; i++) {
            mount_info_t *m = &s_mounts[i];
            uint64_t bsz = m->block_size ? (uint64_t)m->block_size : 512ULL;
            uint64_t total_bytes = m->total_blocks * bsz;
            uint64_t free_bytes = m->free_blocks * bsz;
            uint64_t used_bytes = (m->total_blocks >= m->free_blocks) ? (m->total_blocks - m->free_blocks) * bsz : 0;

            char size_buf[16], used_buf[16], avail_buf[16], use_buf[16];
            char inodes_buf[16], iuse_buf[16];

            if (m->total_blocks > 0) {
                format_size(size_buf, sizeof(size_buf), total_bytes);
                format_size(used_buf, sizeof(used_buf), used_bytes);
                format_size(avail_buf, sizeof(avail_buf), free_bytes);
                format_pct(use_buf, sizeof(use_buf), used_bytes, total_bytes);
            } else {
                size_buf[0] = '-'; size_buf[1] = '\0';
                used_buf[0] = '-'; used_buf[1] = '\0';
                avail_buf[0] = '-'; avail_buf[1] = '\0';
                use_buf[0] = '-'; use_buf[1] = '\0';
            }

            if (m->total_inodes > 0) {
                tool_format_u64(inodes_buf, m->total_inodes);
                uint64_t used_inodes = (m->total_inodes >= m->free_inodes) ? (m->total_inodes - m->free_inodes) : 0;
                format_pct(iuse_buf, sizeof(iuse_buf), used_inodes, m->total_inodes);
            } else {
                inodes_buf[0] = '-'; inodes_buf[1] = '\0';
                iuse_buf[0] = '-'; iuse_buf[1] = '\0';
            }

            out_left(m->mount_path, 10);
            out_str(" ");
            out_left(m->source, 12);
            out_str(" ");
            out_left(fs_type_str(m->fs_type), 6);
            out_str(" ");
            out_right(size_buf, 7);
            out_str(" ");
            out_right(used_buf, 7);
            out_str(" ");
            out_right(avail_buf, 7);
            out_str(" ");
            out_right(use_buf, 6);
            out_str(" ");
            out_right(inodes_buf, 8);
            out_str(" ");
            out_right(iuse_buf, 6);
            out_str("\n");
        }
    } else {
        /* Comparison key=value single-line format */
        for (uint32_t i = 0; i < s_mount_count; i++) {
            mount_info_t *m = &s_mounts[i];
            uint64_t bsz = m->block_size ? (uint64_t)m->block_size : 512ULL;
            uint64_t total_bytes = m->total_blocks * bsz;
            uint64_t free_bytes = m->free_blocks * bsz;
            uint64_t used_bytes = (m->total_blocks >= m->free_blocks) ? (m->total_blocks - m->free_blocks) * bsz : 0;

            char size_buf[16], used_buf[16], avail_buf[16], use_buf[16];
            char inodes_buf[16], iuse_buf[16];

            if (m->total_blocks > 0) {
                format_size(size_buf, sizeof(size_buf), total_bytes);
                format_size(used_buf, sizeof(used_buf), used_bytes);
                format_size(avail_buf, sizeof(avail_buf), free_bytes);
                format_pct(use_buf, sizeof(use_buf), used_bytes, total_bytes);
            } else {
                size_buf[0] = '-'; size_buf[1] = '\0';
                used_buf[0] = '-'; used_buf[1] = '\0';
                avail_buf[0] = '-'; avail_buf[1] = '\0';
                use_buf[0] = '-'; use_buf[1] = '\0';
            }

            if (m->total_inodes > 0) {
                tool_format_u64(inodes_buf, m->total_inodes);
                uint64_t used_inodes = (m->total_inodes >= m->free_inodes) ? (m->total_inodes - m->free_inodes) : 0;
                format_pct(iuse_buf, sizeof(iuse_buf), used_inodes, m->total_inodes);
            } else {
                inodes_buf[0] = '-'; inodes_buf[1] = '\0';
                iuse_buf[0] = '-'; iuse_buf[1] = '\0';
            }

            out_str("mount=");
            out_str(m->mount_path);
            out_str(" source=");
            out_str(m->source);
            out_str(" fs=");
            out_str(fs_type_str(m->fs_type));
            out_str(" size=");
            out_str(size_buf);
            out_str(" used=");
            out_str(used_buf);
            out_str(" avail=");
            out_str(avail_buf);
            out_str(" use_pct=");
            out_str(use_buf);
            out_str(" inodes=");
            out_str(inodes_buf);
            out_str(" iuse_pct=");
            out_str(iuse_buf);
            out_str("\n");
        }
    }

    return 0;
}

int disk_main(int argc, char **argv) {
    if (argc >= 2 && tool_equal(argv[1], "bench")) {
        return diskbench_main(argc - 1, argv + 1);
    }

    bool comparison = false;
    const char *subcmd = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "-c") || tool_equal(arg, "--comparison")) {
            comparison = true;
        } else if (tool_equal(arg, "-h") || tool_equal(arg, "--help") || tool_equal(arg, "help")) {
            print_help();
            return 0;
        } else if (arg[0] != '-' && subcmd == NULL) {
            subcmd = arg;
        } else {
            tool_error("disk", "unrecognized argument", arg);
            return 2;
        }
    }

    if (!subcmd) {
        /* Default to disk list */
        return disk_list(comparison);
    }

    if (tool_equal(subcmd, "list")) {
        return disk_list(comparison);
    } else if (tool_equal(subcmd, "usage")) {
        return disk_usage(comparison);
    } else {
        tool_error("disk", "unknown subcommand", subcmd);
        print_help();
        return 2;
    }
}
