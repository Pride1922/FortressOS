#include "ext2.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"
#include "serial.h"


#define EXT2_MAX_GROUPS 4096U
#define EXT2_MAX_NODES 1024U
#define EXT2_MAX_DIRECTORY (1024U * 1024U)
#define EXT2_MAX_READ (64U * 1024U)
#define EXT2_MAX_WRITE (64U * 1024U)

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks;
    uint16_t free_inodes;
    uint16_t used_dirs;
} ext2_group_desc_t;

typedef struct {
    block_dev_t *dev;
    uint32_t blocks, inodes, first, block_size, bpg, ipg, groups, inode_size;
    uint32_t incompat;
    uint32_t ro_compat;
    uint16_t reserved_gdt_blocks;
    uint32_t first_ino;
    uint32_t free_blocks;
    uint32_t free_inodes;
    ext2_group_desc_t *group_descs;
    size_t nodes;
    bool read_only;
    bool tainted;
} ext2_fs_t;

typedef struct {
    ext2_fs_t *fs;
    uint32_t ino;
    uint16_t mode;
    uint16_t links;
    uint32_t size;
    uint32_t i_blocks;
    uint32_t file_acl;
    uint32_t blocks[15];
} ext2_inode_t;

static ext2_fs_t *g_mounted_ext2;

/* Serializes reads, writes, allocations and cache publication on the bootstrap CPU.
 * Replace with sleepable I/O locking before asynchronous storage or SMP. */
static spinlock_t ext2_lock = SPINLOCK_RANKED(1, "ext2");

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return u16(p) | ((uint32_t)u16(p + 2) << 16);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool bytes(ext2_fs_t *fs, uint64_t off, void *out, size_t len) {
    uint8_t sector[4096];
    uint32_t ss = fs->dev->sector_size;
    uint64_t capacity = fs->dev->sector_count * ss;
    if (off > capacity || len > capacity - off) return false;
    while (len) {
        size_t skip = off % ss, n = ss - skip;
        if (n > len) n = len;
        if (!block_read_sector(fs->dev, off / ss, sector)) return false;
        memcpy(out, sector + skip, n);
        out = (uint8_t *)out + n;
        off += n;
        len -= n;
    }
    return true;
}

static bool write_bytes(ext2_fs_t *fs, uint64_t off, const void *in, size_t len) {
    if (fs->read_only || fs->tainted || !fs->dev->write_sector) return false;
    uint8_t sector[4096];
    uint32_t ss = fs->dev->sector_size;
    uint64_t capacity = fs->dev->sector_count * ss;
    if (off > capacity || len > capacity - off) return false;
    while (len) {
        size_t skip = off % ss, n = ss - skip;
        if (n > len) n = len;
        if (skip != 0 || n != ss) {
            if (!block_read_sector(fs->dev, off / ss, sector)) {
                fs->tainted = true;
                return false;
            }
        }
        memcpy(sector + skip, in, n);
        if (!block_write_sector(fs->dev, off / ss, sector)) {
            fs->tainted = true;
            return false;
        }
        in = (const uint8_t *)in + n;
        off += n;
        len -= n;
    }
    return true;
}

static bool ext2_sync_super(ext2_fs_t *fs) {
    uint8_t raw[8];
    put32(raw, fs->free_blocks);
    put32(raw + 4, fs->free_inodes);
    bool ok = write_bytes(fs, 1024 + 12, raw, 8);
    if (!ok) fs->tainted = true;
    return ok;
}

static bool ext2_sync_group_desc(ext2_fs_t *fs, uint32_t g) {
    if (g >= fs->groups) return false;
    uint64_t gdt = (uint64_t)(fs->first + 1) * fs->block_size;
    uint64_t off = gdt + (uint64_t)g * 32;
    uint8_t raw[6];
    put16(raw, fs->group_descs[g].free_blocks);
    put16(raw + 2, fs->group_descs[g].free_inodes);
    put16(raw + 4, fs->group_descs[g].used_dirs);
    bool ok = write_bytes(fs, off + 12, raw, 6);
    if (!ok) fs->tainted = true;
    return ok;
}

static bool ext2_bg_has_super(ext2_fs_t *fs, uint32_t group) {
    if (!(fs->ro_compat & 1)) return true;
    if (group <= 1) return true;
    for (uint32_t p = 3; p <= group; p *= 3) {
        if (p == group) return true;
        if (p > UINT32_MAX / 3) break;
    }
    for (uint32_t p = 5; p <= group; p *= 5) {
        if (p == group) return true;
        if (p > UINT32_MAX / 5) break;
    }
    for (uint32_t p = 7; p <= group; p *= 7) {
        if (p == group) return true;
        if (p > UINT32_MAX / 7) break;
    }
    return false;
}

static bool ext2_is_metadata_block(ext2_fs_t *fs, uint32_t block) {
    if (!block || block < fs->first || block >= fs->blocks) return true;
    uint32_t g = (block - fs->first) / fs->bpg;
    if (g >= fs->groups) return true;
    uint64_t table_blocks = ((uint64_t)fs->ipg * fs->inode_size + fs->block_size - 1) / fs->block_size;
    if (block == fs->group_descs[g].block_bitmap ||
        block == fs->group_descs[g].inode_bitmap ||
        (block >= fs->group_descs[g].inode_table && block < fs->group_descs[g].inode_table + table_blocks)) {
        return true;
    }
    uint32_t gdt_blocks = (fs->groups * 32 + fs->block_size - 1) / fs->block_size;
    uint32_t meta_reserved = 1 + gdt_blocks + fs->reserved_gdt_blocks;
    uint32_t start = fs->first + g * fs->bpg;
    if (ext2_bg_has_super(fs, g) && block < start + meta_reserved) {
        return true;
    }
    return false;
}

static bool inode(ext2_fs_t *fs, uint32_t number, ext2_inode_t *out) {
    if (!number || number > fs->inodes) return false;
    uint32_t group = (number - 1) / fs->ipg;
    if (group >= fs->groups) return false;
    uint64_t off = (uint64_t)fs->group_descs[group].inode_table * fs->block_size +
                   (uint64_t)((number - 1) % fs->ipg) * fs->inode_size;
    uint8_t raw[256];
    if (fs->inode_size > sizeof(raw)) return false;
    if (!bytes(fs, off, raw, fs->inode_size)) return false;
    memset(out, 0, sizeof(*out));
    out->fs = fs; out->ino = number; out->mode = u16(raw);
    out->links = u16(raw + 26);
    out->size = u32(raw + 4);
    out->i_blocks = u32(raw + 28);
    out->file_acl = (fs->inode_size >= 128) ? u32(raw + 104) : 0;
    uint16_t type = out->mode & 0xf000;
    if (type != 0x8000 && type != 0x4000) return false;
    /* Reject indexed directories, compression, extents and other unsupported inode flags.
     * LARGE_FILE is understood only for files whose high size word is zero. */
    if (u32(raw + 32) != 0 || (type == 0x8000 && u32(raw + 108))) return false;
    if (type == 0x4000 && (out->size > EXT2_MAX_DIRECTORY ||
                          out->size % fs->block_size)) return false;
    for (unsigned i = 0; i < 15; i++) {
        out->blocks[i] = u32(raw + 40 + i * 4);
        if (out->blocks[i] && (out->blocks[i] < fs->first ||
                              out->blocks[i] >= fs->blocks)) return false;
    }
    return true;
}

static bool write_inode_to_disk(ext2_inode_t *in) {
    ext2_fs_t *fs = in->fs;
    if (fs->read_only || fs->tainted) return false;
    if (in->file_acl != 0) return false;
    uint32_t number = in->ino;
    if (!number || number > fs->inodes) return false;
    uint32_t group = (number - 1) / fs->ipg;
    if (group >= fs->groups) return false;
    uint64_t off = (uint64_t)fs->group_descs[group].inode_table * fs->block_size +
                   (uint64_t)((number - 1) % fs->ipg) * fs->inode_size;
    uint8_t raw[256];
    if (fs->inode_size > sizeof(raw)) return false;
    if (!bytes(fs, off, raw, fs->inode_size)) {
        fs->tainted = true;
        return false;
    }
    /* Preserve untouched fields (uid, gid, flags, etc.) */
    put16(raw, in->mode);
    put32(raw + 4, in->size);
    /* Update modification timestamp */
    put32(raw + 16, 1726500000);
    put16(raw + 26, in->links);
    uint32_t sectors = in->i_blocks;
    if (!sectors && in->size) {
        uint64_t blks = ((uint64_t)in->size + fs->block_size - 1) / fs->block_size;
        sectors = (uint32_t)(blks * (fs->block_size / 512));
    }
    put32(raw + 28, sectors);
    for (unsigned i = 0; i < 15; i++) {
        put32(raw + 40 + i * 4, in->blocks[i]);
    }
    if (in->links == 0) {
        put32(raw + 20, 1726500000); /* Deletion time (i_dtime) */
        put32(raw + 4, 0);           /* i_size = 0 */
        put32(raw + 28, 0);          /* i_blocks = 0 */
        for (unsigned i = 0; i < 15; i++) {
            put32(raw + 40 + i * 4, 0);
        }
    }
    bool ok = write_bytes(fs, off, raw, fs->inode_size);
    if (!ok) fs->tainted = true;
    return ok;
}

static uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    if (fs->read_only || fs->tainted || !fs->free_blocks) return 0;

    /* Preallocate zero buffer BEFORE any disk reservation/mutation to prevent leaks on OOM */
    uint8_t *zero = kcalloc(1, fs->block_size);
    if (!zero) return 0;

    for (uint32_t g = 0; g < fs->groups; g++) {
        if (!fs->group_descs[g].free_blocks) continue;
        uint32_t bmp = fs->group_descs[g].block_bitmap;
        uint8_t *b = kmalloc(fs->block_size);
        if (!b) {
            kfree(zero);
            return 0;
        }
        if (!bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
            kfree(b);
            kfree(zero);
            fs->tainted = true;
            return 0;
        }
        uint32_t max_bits = fs->bpg;
        uint32_t start = fs->first + g * fs->bpg;
        if (start + max_bits > fs->blocks) max_bits = fs->blocks - start;
        uint32_t allocated = 0;
        for (uint32_t i = 0; i < max_bits; i++) {
            uint32_t candidate = start + i;
            if (ext2_is_metadata_block(fs, candidate)) {
                continue;
            }
            if (!(b[i / 8] & (1 << (i % 8)))) {
                b[i / 8] |= (1 << (i % 8));
                allocated = candidate;
                break;
            }
        }
        if (allocated) {
            /* Stage 1: Persist allocation reservation */
            if (!write_bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
                kfree(b);
                kfree(zero);
                fs->tainted = true;
                return 0;
            }
            kfree(b);
            fs->group_descs[g].free_blocks--;
            if (!ext2_sync_group_desc(fs, g)) {
                kfree(zero);
                fs->tainted = true;
                return 0;
            }
            fs->free_blocks--;
            if (!ext2_sync_super(fs)) {
                kfree(zero);
                fs->tainted = true;
                return 0;
            }
            /* Flush barrier 1: Reservation is durable */
            if (fs->dev->flush && !block_flush(fs->dev)) {
                kfree(zero);
                fs->tainted = true;
                return 0;
            }

            /* Stage 2: Initialize allocated block with zeroes */
            if (!write_bytes(fs, (uint64_t)allocated * fs->block_size, zero, fs->block_size)) {
                kfree(zero);
                fs->tainted = true;
                return 0;
            }
            kfree(zero);
            /* Flush barrier 2: Initialized payload is durable */
            if (fs->dev->flush && !block_flush(fs->dev)) {
                fs->tainted = true;
                return 0;
            }

            return allocated;
        }
        kfree(b);
    }
    kfree(zero);
    return 0;
}

static bool ext2_free_block(ext2_fs_t *fs, uint32_t block, uint8_t *b) {
    if (fs->read_only || fs->tainted || !block || block < fs->first || block >= fs->blocks) return false;
    if (ext2_is_metadata_block(fs, block)) {
        fs->tainted = true;
        return false;
    }
    uint32_t g = (block - fs->first) / fs->bpg;
    if (g >= fs->groups) {
        fs->tainted = true;
        return false;
    }

    uint32_t bmp = fs->group_descs[g].block_bitmap;
    if (!bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
        fs->tainted = true;
        return false;
    }

    uint32_t bit = (block - fs->first) % fs->bpg;
    if (!(b[bit / 8] & (1 << (bit % 8)))) {
        fs->tainted = true;
        return false; /* Block was already free or bitmap corruption */
    }

    b[bit / 8] &= ~(1 << (bit % 8));
    if (!write_bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
        fs->tainted = true;
        return false;
    }

    fs->group_descs[g].free_blocks++;
    if (!ext2_sync_group_desc(fs, g)) {
        fs->tainted = true;
        return false;
    }
    fs->free_blocks++;
    if (!ext2_sync_super(fs)) {
        fs->tainted = true;
        return false;
    }
    return true;
}

static uint32_t ext2_alloc_inode(ext2_fs_t *fs, bool is_dir) {
    if (fs->read_only || fs->tainted || !fs->free_inodes) return 0;
    for (uint32_t g = 0; g < fs->groups; g++) {
        if (!fs->group_descs[g].free_inodes) continue;
        uint32_t bmp = fs->group_descs[g].inode_bitmap;
        uint8_t *b = kmalloc(fs->block_size);
        if (!b) return 0;
        if (!bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
            kfree(b);
            fs->tainted = true;
            return 0;
        }
        /* In group 0, start scanning after reserved inodes */
        uint32_t start_ino = (g == 0) ? (fs->first_ino ? fs->first_ino : 11) : 1;
        uint32_t allocated = 0;
        for (uint32_t i = start_ino - 1; i < fs->ipg; i++) {
            uint32_t candidate = 1 + g * fs->ipg + i;
            if (candidate > fs->inodes) break;
            if (!(b[i / 8] & (1 << (i % 8)))) {
                b[i / 8] |= (1 << (i % 8));
                allocated = candidate;
                break;
            }
        }
        if (allocated) {
            /* Stage 1: Persist inode allocation in bitmap */
            if (!write_bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
                kfree(b);
                fs->tainted = true;
                return 0;
            }
            kfree(b);
            fs->group_descs[g].free_inodes--;
            if (is_dir) fs->group_descs[g].used_dirs++;
            if (!ext2_sync_group_desc(fs, g)) {
                fs->tainted = true;
                return 0;
            }
            fs->free_inodes--;
            if (!ext2_sync_super(fs)) {
                fs->tainted = true;
                return 0;
            }
            /* Flush barrier: Inode reservation is durable */
            if (fs->dev->flush && !block_flush(fs->dev)) {
                fs->tainted = true;
                return 0;
            }
            return allocated;
        }
        kfree(b);
    }
    return 0;
}

static bool __attribute__((unused)) ext2_free_inode(ext2_fs_t *fs, uint32_t ino, bool is_dir) {
    if (fs->read_only || fs->tainted || !ino || ino < (fs->first_ino ? fs->first_ino : 11) || ino > fs->inodes) {
        return false;
    }
    uint32_t g = (ino - 1) / fs->ipg;
    if (g >= fs->groups) return false;

    uint32_t bmp = fs->group_descs[g].inode_bitmap;
    uint8_t *b = kmalloc(fs->block_size);
    if (!b) return false;
    if (!bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
        kfree(b);
        fs->tainted = true;
        return false;
    }

    uint32_t bit = (ino - 1) % fs->ipg;
    if (!(b[bit / 8] & (1 << (bit % 8)))) {
        kfree(b);
        return false; /* Inode was already free */
    }

    b[bit / 8] &= ~(1 << (bit % 8));
    if (!write_bytes(fs, (uint64_t)bmp * fs->block_size, b, fs->block_size)) {
        kfree(b);
        fs->tainted = true;
        return false;
    }
    kfree(b);

    fs->group_descs[g].free_inodes++;
    if (is_dir && fs->group_descs[g].used_dirs > 0) {
        fs->group_descs[g].used_dirs--;
    }
    if (!ext2_sync_group_desc(fs, g)) {
        fs->tainted = true;
        return false;
    }
    fs->free_inodes++;
    if (!ext2_sync_super(fs)) {
        fs->tainted = true;
        return false;
    }
    return true;
}

static bool file_block(ext2_inode_t *in, uint64_t index, uint32_t *out) {
    ext2_fs_t *fs = in->fs;
    uint64_t n = fs->block_size / 4;
    if (index < 12) {
        *out = in->blocks[index];
        return true;
    }
    index -= 12;
    unsigned depth;
    uint32_t block;
    if (index < n) {
        depth = 1;
        block = in->blocks[12];
    } else if ((index -= n) < n * n) {
        depth = 2;
        block = in->blocks[13];
    } else if ((index -= n * n) < n * n * n) {
        depth = 3;
        block = in->blocks[14];
    } else {
        return false;
    }
    for (; depth > 0; depth--) {
        if (!block) { *out = 0; return true; }
        uint64_t span = 1;
        for (unsigned d = 1; d < depth; d++) span *= n;
        uint8_t raw[4];
        if (!bytes(fs, (uint64_t)block * fs->block_size + (index / span) * 4,
                   raw, 4)) return false;
        block = u32(raw);
        index %= span;
    }
    if (block && (block < fs->first || block >= fs->blocks)) return false;
    *out = block;
    return true;
}

static bool ext2_validate_block_mapping(ext2_fs_t *fs, uint32_t block) {
    if (!block || block < fs->first || block >= fs->blocks) {
        fs->tainted = true;
        return false;
    }
    if (ext2_is_metadata_block(fs, block)) {
        fs->tainted = true;
        return false;
    }
    uint32_t g = (block - fs->first) / fs->bpg;
    if (g >= fs->groups) {
        fs->tainted = true;
        return false;
    }
    uint32_t bmp = fs->group_descs[g].block_bitmap;
    uint32_t bit = (block - fs->first) % fs->bpg;
    uint8_t byte_val;
    if (!bytes(fs, (uint64_t)bmp * fs->block_size + (bit / 8), &byte_val, 1)) {
        fs->tainted = true;
        return false;
    }
    if (!(byte_val & (1 << (bit % 8)))) {
        fs->tainted = true;
        return false;
    }
    return true;
}

static bool file_block_alloc(ext2_inode_t *in, uint64_t index, uint32_t *out) {
    ext2_fs_t *fs = in->fs;
    if (fs->read_only || fs->tainted) return false;
    /* Reject files with double/triple indirection or external extended attributes */
    if (in->blocks[13] != 0 || in->blocks[14] != 0 || in->file_acl != 0) return false;

    uint64_t n = fs->block_size / 4;
    if (index < 12) {
        if (!in->blocks[index]) {
            uint32_t blk = ext2_alloc_block(fs);
            if (!blk) return false;
            in->blocks[index] = blk;
            in->i_blocks += (fs->block_size / 512);
        } else {
            if (!ext2_validate_block_mapping(fs, in->blocks[index])) {
                return false;
            }
        }
        *out = in->blocks[index];
        return true;
    }
    index -= 12;
    if (index < n) {
        if (!in->blocks[12]) {
            uint32_t ind_blk = ext2_alloc_block(fs);
            if (!ind_blk) return false;
            in->blocks[12] = ind_blk;
            in->i_blocks += (fs->block_size / 512);
            /* Flush barrier for newly linked indirect table */
            if (!write_inode_to_disk(in)) { fs->tainted = true; return false; }
            if (fs->dev->flush && !block_flush(fs->dev)) { fs->tainted = true; return false; }
        } else {
            if (!ext2_validate_block_mapping(fs, in->blocks[12])) {
                return false;
            }
        }
        uint32_t ind_blk = in->blocks[12];
        uint64_t entry_off = (uint64_t)ind_blk * fs->block_size + index * 4;
        uint8_t raw[4];
        if (!bytes(fs, entry_off, raw, 4)) return false;
        uint32_t blk = u32(raw);
        if (!blk) {
            blk = ext2_alloc_block(fs);
            if (!blk) return false;
            put32(raw, blk);
            if (!write_bytes(fs, entry_off, raw, 4)) {
                fs->tainted = true;
                return false;
            }
            if (fs->dev->flush && !block_flush(fs->dev)) {
                fs->tainted = true;
                return false;
            }
            in->i_blocks += (fs->block_size / 512);
        } else {
            if (!ext2_validate_block_mapping(fs, blk)) {
                return false;
            }
        }
        *out = blk;
        return true;
    }
    return false;
}

static int64_t read_inode(ext2_inode_t *in, uint64_t off, void *buf, size_t len) {
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;
    size_t done = 0;
    while (done < len) {
        uint32_t block;
        uint32_t bs = in->fs->block_size;
        size_t skip = off % bs, n = bs - skip;
        if (n > len - done) n = len - done;
        if (!file_block(in, off / bs, &block)) return -1;
        if (!block) memset((uint8_t *)buf + done, 0, n);
        else if (!bytes(in->fs, (uint64_t)block * bs + skip,
                        (uint8_t *)buf + done, n)) return -1;
        done += n; off += n;
    }
    return (int64_t)done;
}

static int64_t write_inode(ext2_inode_t *in, uint64_t off, const void *buf, size_t len) {
    ext2_fs_t *fs = in->fs;
    if (fs->tainted) return -5;  /* -EIO */
    if (fs->read_only) return -30; /* -EROFS */
    if (in->file_acl != 0) return -95; /* -EOPNOTSUPP */
    if (len > EXT2_MAX_WRITE) len = EXT2_MAX_WRITE;
    if (len == 0 && buf == NULL) {
        in->size = (uint32_t)off;
        if (!write_inode_to_disk(in)) { fs->tainted = true; return -5; }
        if (fs->dev->flush && !block_flush(fs->dev)) {
            fs->tainted = true;
            return -5;
        }
        return 0;
    }
    size_t done = 0;
    while (done < len) {
        uint32_t block = 0;
        uint32_t bs = fs->block_size;
        size_t skip = off % bs, n = bs - skip;
        if (n > len - done) n = len - done;
        if (!file_block_alloc(in, off / bs, &block) || !block) {
            /* If a prefix was already written and durably flushed, report that prefix */
            if (done > 0) {
                if (!write_inode_to_disk(in)) { fs->tainted = true; return -5; }
                if (fs->dev->flush && !block_flush(fs->dev)) { fs->tainted = true; return -5; }
                return (int64_t)done;
            }
            return fs->tainted ? -VFS_EIO : -VFS_ENOSPC;
        }
        if (!write_bytes(fs, (uint64_t)block * bs + skip, (const uint8_t *)buf + done, n)) {
            fs->tainted = true;
            return -5;
        }
        done += n;
        off += n;
        if (off > in->size) in->size = (uint32_t)off;
    }
    if (!write_inode_to_disk(in)) {
        fs->tainted = true;
        return -5;
    }
    if (fs->dev->flush && !block_flush(fs->dev)) {
        fs->tainted = true;
        return -5;
    }
    return (int64_t)done;
}

static int dir_entry(ext2_inode_t *dir, const char *name, uint64_t index,
                     char *out_name, ext2_inode_t *out_inode) {
    uint64_t off = 0, curr_idx = 0;
    while (off < dir->size) {
        uint8_t h[8];
        if (read_inode(dir, off, h, 8) != 8) return -1;
        uint32_t ino = u32(h);
        uint16_t rec = u16(h + 4);
        uint16_t len = dir->fs->incompat & 2 ? h[6] : u16(h + 6);
        if (rec < 8 || rec % 4 ||
            rec > dir->fs->block_size - (off % dir->fs->block_size) ||
            rec > dir->size - off || len > rec - 8 || len > 255) return -1;
        if (ino) {
            char found[256];
            if (!len || ino > dir->fs->inodes ||
                read_inode(dir, off + 8, found, len) != (int64_t)len) return -1;
            for (unsigned i = 0; i < len; i++) {
                if (!found[i] || found[i] == '/') return -1;
            }
            found[len] = '\0';
            if (name) {
                if (!strcmp(found, name)) {
                    if (out_name) {
                        if (len >= VFS_MAX_NAME) return -1;
                        memcpy(out_name, found, len + 1);
                    }
                    return inode(dir->fs, ino, out_inode) ? 1 : -1;
                }
            } else {
                if (strcmp(found, ".") && strcmp(found, "..")) {
                    if (curr_idx == index) {
                        if (out_name) {
                            if (len >= VFS_MAX_NAME) return -1;
                            memcpy(out_name, found, len + 1);
                        }
                        return inode(dir->fs, ino, out_inode) ? 1 : -1;
                    }
                    curr_idx++;
                }
            }
        }
        off += rec;
    }
    return 0;
}

static bool ext2_add_dir_entry(ext2_inode_t *dir, const char *name, uint32_t new_ino, uint8_t file_type) {
    ext2_fs_t *fs = dir->fs;
    if (fs->read_only) return false;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= VFS_MAX_NAME) return false;

    uint16_t new_rec_len = (uint16_t)((8 + name_len + 3) & ~3);
    uint64_t off = 0;
    while (off < dir->size) {
        uint8_t h[8];
        if (read_inode(dir, off, h, 8) != 8) return false;
        uint32_t ino = u32(h);
        uint16_t rec = u16(h + 4);
        uint16_t len = dir->fs->incompat & 2 ? h[6] : u16(h + 6);
        if (rec < 8 || rec % 4) return false;

        uint16_t actual = (uint16_t)((8 + len + 3) & ~3);
        if (ino == 0 && rec >= new_rec_len) {
            uint8_t entry[264];
            memset(entry, 0, new_rec_len);
            put32(entry, new_ino);
            put16(entry + 4, rec);
            if (dir->fs->incompat & 2) {
                entry[6] = (uint8_t)name_len;
                entry[7] = file_type;
            } else {
                put16(entry + 6, (uint16_t)name_len);
            }
            memcpy(entry + 8, name, name_len);
            if (write_inode(dir, off, entry, new_rec_len) != (int64_t)new_rec_len) return false;
            return true;
        }

        if (rec >= actual + new_rec_len) {
            uint16_t rem = rec - actual;
            put16(h + 4, actual);
            if (write_inode(dir, off, h, 8) != 8) return false;

            uint8_t new_entry[264];
            memset(new_entry, 0, new_rec_len);
            put32(new_entry, new_ino);
            put16(new_entry + 4, rem);
            if (dir->fs->incompat & 2) {
                new_entry[6] = (uint8_t)name_len;
                new_entry[7] = file_type;
            } else {
                put16(new_entry + 6, (uint16_t)name_len);
            }
            memcpy(new_entry + 8, name, name_len);
            if (write_inode(dir, off + actual, new_entry, new_rec_len) != (int64_t)new_rec_len) return false;
            return true;
        }
        off += rec;
    }

    /* Check directory capacity limit before allocating block */
    uint32_t dir_block_idx = dir->size / fs->block_size;
    if (dir_block_idx >= 12) return false;

    /* Preallocate block buffer before allocating block to prevent leaks on OOM */
    uint8_t *block_buf = kcalloc(1, fs->block_size);
    if (!block_buf) return false;

    /* Allocate new directory block */
    uint32_t new_blk = ext2_alloc_block(fs);
    if (!new_blk) {
        kfree(block_buf);
        return false;
    }
    dir->blocks[dir_block_idx] = new_blk;
    dir->i_blocks += (fs->block_size / 512);

    put32(block_buf, new_ino);
    put16(block_buf + 4, (uint16_t)fs->block_size);
    if (dir->fs->incompat & 2) {
        block_buf[6] = (uint8_t)name_len;
        block_buf[7] = file_type;
    } else {
        put16(block_buf + 6, (uint16_t)name_len);
    }
    memcpy(block_buf + 8, name, name_len);
    uint64_t new_block_off = (uint64_t)new_blk * fs->block_size;
    bool ok = write_bytes(fs, new_block_off, block_buf, fs->block_size);
    kfree(block_buf);
    if (!ok) return false;

    dir->size += fs->block_size;
    if (!write_inode_to_disk(dir)) return false;
    if (fs->dev->flush && !block_flush(fs->dev)) {
        fs->tainted = true;
        return false;
    }
    return true;
}

static int64_t ext_read(vfs_node_t *node, uint64_t off, void *buf, size_t len) {
    if (len > EXT2_MAX_READ) len = EXT2_MAX_READ;
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int64_t r = read_inode(node->fs_private, off, buf, len);
    spin_unlock_irqrestore(&ext2_lock, flags);
    return r;
}

static int64_t ext_write(vfs_node_t *node, uint64_t off, const void *buf, size_t len) {
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int64_t r = write_inode(node->fs_private, off, buf, len);
    if (r > 0) {
        ext2_inode_t *in = node->fs_private;
        node->size = in->size;
    }
    spin_unlock_irqrestore(&ext2_lock, flags);
    return r;
}

static int ext_truncate(vfs_node_t *node, uint64_t new_size) {
    if (!node || node->type != VFS_FILE) return -1;
    ext2_inode_t *in = node->fs_private;
    if (!in || !in->fs) return -1;
    ext2_fs_t *fs = in->fs;
    if (fs->tainted) return -5;  /* -EIO */
    if (fs->read_only) return -30; /* -EROFS */

    /* For Phase 9D, truncation is bounded to truncating to 0 */
    if (new_size != 0) return -22; /* -EINVAL */

    /* Guard: Double or triple indirection is unsupported */
    if (in->blocks[13] != 0 || in->blocks[14] != 0) {
        return -27; /* -EFBIG */
    }

    /* Guard: External extended attribute block is unsupported */
    if (in->file_acl != 0) {
        return -95; /* -EOPNOTSUPP */
    }

    uint64_t flags = spin_lock_irqsave(&ext2_lock);

    /* =========================================================================
     * Stage 1: Complete Pre-Validation (even if size == 0)
     * Collect all direct and indirect block numbers and validate bounds,
     * metadata exclusion, and absence of duplicate pointers.
     * ========================================================================= */
    size_t max_blocks = 12 + 1 + fs->block_size / 4;
    uint32_t *to_free = kmalloc(max_blocks * sizeof(uint32_t) + fs->block_size);
    if (!to_free) {
        spin_unlock_irqrestore(&ext2_lock, flags);
        return -12; /* -ENOMEM */
    }
    uint8_t *free_bitmap = (uint8_t *)(to_free + max_blocks);
    size_t count = 0;

    /* (a) Direct blocks */
    for (int i = 0; i < 12; i++) {
        uint32_t blk = in->blocks[i];
        if (blk) {
            if (ext2_is_metadata_block(fs, blk)) {
                kfree(to_free);
                spin_unlock_irqrestore(&ext2_lock, flags);
                return -22; /* -EINVAL */
            }
            for (size_t k = 0; k < count; k++) {
                if (to_free[k] == blk) {
                    kfree(to_free);
                    spin_unlock_irqrestore(&ext2_lock, flags);
                    return -22; /* -EINVAL duplicate pointer */
                }
            }
            to_free[count++] = blk;
        }
    }

    /* (b) Single indirect block */
    if (in->blocks[12]) {
        uint32_t indir_blk = in->blocks[12];
        if (ext2_is_metadata_block(fs, indir_blk)) {
            kfree(to_free);
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -22;
        }
        for (size_t k = 0; k < count; k++) {
            if (to_free[k] == indir_blk) {
                kfree(to_free);
                spin_unlock_irqrestore(&ext2_lock, flags);
                return -22;
            }
        }
        to_free[count++] = indir_blk;

        uint8_t *indir_buf = kmalloc(fs->block_size);
        if (!indir_buf) {
            kfree(to_free);
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -12;
        }
        if (!bytes(fs, (uint64_t)indir_blk * fs->block_size, indir_buf, fs->block_size)) {
            kfree(indir_buf);
            kfree(to_free);
            fs->tainted = true;
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -5;
        }

        size_t ptrs = fs->block_size / 4;
        for (size_t i = 0; i < ptrs; i++) {
            uint32_t leaf = u32(indir_buf + i * 4);
            if (leaf) {
                if (ext2_is_metadata_block(fs, leaf)) {
                    kfree(indir_buf);
                    kfree(to_free);
                    spin_unlock_irqrestore(&ext2_lock, flags);
                    return -22;
                }
                for (size_t k = 0; k < count; k++) {
                    if (to_free[k] == leaf) {
                        kfree(indir_buf);
                        kfree(to_free);
                        spin_unlock_irqrestore(&ext2_lock, flags);
                        return -22;
                    }
                }
                to_free[count++] = leaf;
            }
        }
        kfree(indir_buf);
    }

    /* Validate that all to_free blocks are valid, non-metadata, and marked allocated in bitmap */
    for (size_t i = 0; i < count; i++) {
        uint32_t blk = to_free[i];
        if (blk < fs->first || blk >= fs->blocks || ext2_is_metadata_block(fs, blk)) {
            kfree(to_free);
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -22;
        }
        uint32_t g = (blk - fs->first) / fs->bpg;
        if (g >= fs->groups) {
            kfree(to_free);
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -22;
        }
        uint32_t bmp = fs->group_descs[g].block_bitmap;
        uint32_t bit = (blk - fs->first) % fs->bpg;
        uint8_t byte_val;
        if (!bytes(fs, (uint64_t)bmp * fs->block_size + (bit / 8), &byte_val, 1)) {
            kfree(to_free);
            fs->tainted = true;
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -5;
        }
        if (!(byte_val & (1 << (bit % 8)))) {
            kfree(to_free);
            fs->tainted = true;
            spin_unlock_irqrestore(&ext2_lock, flags);
            return -22;
        }
    }

    /* =========================================================================
     * Stage 2: Persist detachment before reclamation
     * ========================================================================= */
    for (int i = 0; i < 15; i++) in->blocks[i] = 0;
    in->size = 0;
    in->i_blocks = 0;
    node->size = 0;

    if (!write_inode_to_disk(in)) {
        kfree(to_free);
        fs->tainted = true;
        spin_unlock_irqrestore(&ext2_lock, flags);
        return -5;
    }
    if (fs->dev->flush && !block_flush(fs->dev)) {
        kfree(to_free);
        fs->tainted = true;
        spin_unlock_irqrestore(&ext2_lock, flags);
        return -5;
    }

    /* =========================================================================
     * Stage 3: Reclamation
     * ========================================================================= */
    bool reclamation_failed = false;
    for (size_t i = 0; i < count; i++) {
        if (!ext2_free_block(fs, to_free[i], free_bitmap)) {
            reclamation_failed = true;
            fs->tainted = true;
            break;
        }
    }
    kfree(to_free);

    if (reclamation_failed) {
        spin_unlock_irqrestore(&ext2_lock, flags);
        return -5;
    }

    if (fs->dev->flush && !block_flush(fs->dev)) {
        fs->tainted = true;
        spin_unlock_irqrestore(&ext2_lock, flags);
        return -5;
    }

    spin_unlock_irqrestore(&ext2_lock, flags);
    return 0;
}

static int ext_readdir(vfs_node_t *dir, uint64_t index, void *out) {
    vfs_dirent_t *out_dent = out;
    if (!dir || dir->type != VFS_DIRECTORY || !out_dent) return -1;
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    ext2_inode_t in;
    int r = dir_entry(dir->fs_private, NULL, index, out_dent->name, &in);
    if (r == 1) {
        out_dent->size = in.size;
        out_dent->type = (in.mode & 0xf000) == 0x4000 ? VFS_DIRECTORY : VFS_FILE;
    }
    spin_unlock_irqrestore(&ext2_lock, flags);
    return r;
}

static vfs_node_t *ext_lookup(vfs_node_t *parent, const char *name);
static vfs_node_t *ext_create(vfs_node_t *dir_node, const char *name, vfs_node_type_t type);
static int ext_unlink(vfs_node_t *dir_node, const char *name);
static int ext_rename(vfs_node_t *old_dir_node, const char *old_name,
                      vfs_node_t *new_dir_node, const char *new_name);

static void setup(vfs_node_t *node, ext2_inode_t *in) {
    node->fs_private = in;
    node->size = in->size;
    node->type = (in->mode & 0xf000) == 0x4000 ? VFS_DIRECTORY : VFS_FILE;
    node->read = ext_read;
    if (in->fs->read_only) {
        node->write = NULL;
        node->truncate = NULL;
    } else {
        node->write = ext_write;
        node->truncate = ext_truncate;
    }
    if (node->type == VFS_DIRECTORY) {
        node->lookup = ext_lookup;
        node->readdir = ext_readdir;
        node->create = in->fs->read_only ? NULL : ext_create;
        node->unlink = in->fs->read_only ? NULL : ext_unlink;
        node->rename = in->fs->read_only ? NULL : ext_rename;
    }
}

static bool ext2_remove_dir_entry(ext2_inode_t *dir, const char *name, uint32_t *out_ino) {
    uint64_t off = 0;
    uint64_t prev_off = 0;
    uint16_t prev_rec = 0;

    while (off < dir->size) {
        uint8_t h[8];
        if (read_inode(dir, off, h, 8) != 8) return false;
        uint32_t ino = u32(h);
        uint16_t rec = u16(h + 4);
        uint16_t len = dir->fs->incompat & 2 ? h[6] : u16(h + 6);
        if (rec < 8 || rec % 4 || rec > dir->size - off) return false;

        if (ino) {
            char found_name[256];
            if (read_inode(dir, off + 8, found_name, len) != (int64_t)len) return false;
            found_name[len] = '\0';
            if (!strcmp(found_name, name)) {
                if (out_ino) *out_ino = ino;
                if ((off % dir->fs->block_size) != 0 && prev_rec > 0) {
                    uint16_t new_prev_rec = prev_rec + rec;
                    uint8_t raw[2];
                    put16(raw, new_prev_rec);
                    if (write_inode(dir, prev_off + 4, raw, 2) != 2) return false;
                } else {
                    uint8_t zero[4] = {0, 0, 0, 0};
                    if (write_inode(dir, off, zero, 4) != 4) return false;
                }
                return true;
            }
        }

        if (((off + rec) % dir->fs->block_size) == 0) {
            prev_off = 0;
            prev_rec = 0;
        } else {
            prev_off = off;
            prev_rec = rec;
        }
        off += rec;
    }
    return false;
}

static int ext_unlink(vfs_node_t *dir_node, const char *name) {
    if (!dir_node || !name || dir_node->type != VFS_DIRECTORY) return -VFS_EINVAL;
    ext2_inode_t *dir = dir_node->fs_private;
    if (!dir || !dir->fs) return -VFS_EIO;
    if (dir->fs->read_only) return -VFS_EROFS;
    if (dir->fs->tainted) return -VFS_EIO;

    ext2_fs_t *fs = dir->fs;
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int res = VFS_SUCCESS;

    ext2_inode_t target_in;
    int dres = dir_entry(dir, name, 0, NULL, &target_in);
    if (dres != 1) {
        res = -VFS_ENOENT;
        goto done;
    }

    bool is_dir = (target_in.mode & 0xf000) == 0x4000;
    if (is_dir) {
        /* Verify empty directory */
        uint64_t off = 0;
        while (off < target_in.size) {
            uint8_t h[8];
            if (read_inode(&target_in, off, h, 8) != 8) {
                res = -VFS_EIO;
                goto done;
            }
            uint32_t ino = u32(h);
            uint16_t rec = u16(h + 4);
            uint16_t len = fs->incompat & 2 ? h[6] : u16(h + 6);
            if (rec < 8 || rec % 4 || rec > target_in.size - off) {
                res = -VFS_EIO;
                goto done;
            }
            if (ino) {
                char ent_name[256];
                if (read_inode(&target_in, off + 8, ent_name, len) != (int64_t)len) {
                    res = -VFS_EIO;
                    goto done;
                }
                ent_name[len] = '\0';
                if (strcmp(ent_name, ".") != 0 && strcmp(ent_name, "..") != 0) {
                    res = -VFS_ENOTEMPTY;
                    goto done;
                }
            }
            off += rec;
        }
    }

    uint32_t removed_ino = 0;
    if (!ext2_remove_dir_entry(dir, name, &removed_ino)) {
        res = -VFS_EIO;
        goto done;
    }

    uint8_t *fb = kmalloc(fs->block_size);
    if (is_dir) {
        if (dir->links > 0) dir->links--;
        if (!write_inode_to_disk(dir)) {
            kfree(fb);
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
        if (target_in.blocks[0] && fb) {
            ext2_free_block(fs, target_in.blocks[0], fb);
        }
        if (!ext2_free_inode(fs, target_in.ino, true)) {
            kfree(fb);
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
        target_in.links = 0;
        target_in.size = 0;
        target_in.i_blocks = 0;
        memset(target_in.blocks, 0, sizeof(target_in.blocks));
        if (!write_inode_to_disk(&target_in)) {
            kfree(fb);
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
    } else {
        if (target_in.links > 0) target_in.links--;
        if (target_in.links == 0) {
            if (fb) {
                for (int i = 0; i < 12; i++) {
                    if (target_in.blocks[i]) {
                        ext2_free_block(fs, target_in.blocks[i], fb);
                    }
                }
                if (target_in.blocks[12]) {
                    uint32_t ind_blk = target_in.blocks[12];
                    uint8_t *ind_buf = kmalloc(fs->block_size);
                    if (ind_buf) {
                        if (bytes(fs, (uint64_t)ind_blk * fs->block_size, ind_buf, fs->block_size)) {
                            for (uint32_t j = 0; j < fs->block_size / 4; j++) {
                                uint32_t blk = u32(ind_buf + j * 4);
                                if (blk) ext2_free_block(fs, blk, fb);
                            }
                        }
                        kfree(ind_buf);
                    }
                    ext2_free_block(fs, ind_blk, fb);
                }
            }
            if (!ext2_free_inode(fs, target_in.ino, false)) {
                kfree(fb);
                fs->tainted = true;
                res = -VFS_EIO;
                goto done;
            }
            target_in.size = 0;
            target_in.i_blocks = 0;
            memset(target_in.blocks, 0, sizeof(target_in.blocks));
            if (!write_inode_to_disk(&target_in)) {
                kfree(fb);
                fs->tainted = true;
                res = -VFS_EIO;
                goto done;
            }
        } else {
            if (!write_inode_to_disk(&target_in)) {
                kfree(fb);
                fs->tainted = true;
                res = -VFS_EIO;
                goto done;
            }
        }
    }
    kfree(fb);

    if (fs->dev->flush && !block_flush(fs->dev)) {
        fs->tainted = true;
        res = -VFS_EIO;
        goto done;
    }
    if (fs->nodes > 0) fs->nodes--;

done:
    spin_unlock_irqrestore(&ext2_lock, flags);
    return res;
}

static int ext_rename(vfs_node_t *old_dir_node, const char *old_name,
                      vfs_node_t *new_dir_node, const char *new_name) {
    if (!old_dir_node || !new_dir_node || !old_name || !new_name) return -VFS_EINVAL;
    if (old_dir_node->type != VFS_DIRECTORY || new_dir_node->type != VFS_DIRECTORY) return -8;
    ext2_inode_t *old_dir = old_dir_node->fs_private;
    ext2_inode_t *new_dir = new_dir_node->fs_private;
    if (!old_dir || !new_dir || !old_dir->fs || !new_dir->fs) return -VFS_EIO;
    if (old_dir->fs != new_dir->fs) return -VFS_EROFS;
    ext2_fs_t *fs = old_dir->fs;
    if (fs->read_only) return -VFS_EROFS;
    if (fs->tainted) return -VFS_EIO;

    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int res = VFS_SUCCESS;

    ext2_inode_t old_in;
    int dres = dir_entry(old_dir, old_name, 0, NULL, &old_in);
    if (dres != 1) {
        res = -VFS_ENOENT;
        goto done;
    }
    bool is_dir = (old_in.mode & 0xf000) == 0x4000;

    /* Add entry to new_dir */
    uint8_t file_type = is_dir ? 2 : 1;
    if (!ext2_add_dir_entry(new_dir, new_name, old_in.ino, file_type)) {
        res = -VFS_ENOSPC;
        goto done;
    }

    /* If moving directory across parents, update ".." entry */
    if (is_dir && old_dir->ino != new_dir->ino) {
        uint8_t *dir_buf = kmalloc(fs->block_size);
        if (!dir_buf || !bytes(fs, (uint64_t)old_in.blocks[0] * fs->block_size, dir_buf, fs->block_size)) {
            kfree(dir_buf);
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
        put32(dir_buf + 12, new_dir->ino);
        if (!write_bytes(fs, (uint64_t)old_in.blocks[0] * fs->block_size, dir_buf, fs->block_size)) {
            kfree(dir_buf);
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
        kfree(dir_buf);

        if (old_dir->links > 0) old_dir->links--;
        new_dir->links++;
        if (!write_inode_to_disk(old_dir) || !write_inode_to_disk(new_dir)) {
            fs->tainted = true;
            res = -VFS_EIO;
            goto done;
        }
    }

    /* Remove old entry from old_dir */
    uint32_t dummy = 0;
    if (!ext2_remove_dir_entry(old_dir, old_name, &dummy)) {
        fs->tainted = true;
        res = -VFS_EIO;
        goto done;
    }

    if (fs->dev->flush && !block_flush(fs->dev)) {
        fs->tainted = true;
        res = -VFS_EIO;
        goto done;
    }

done:
    spin_unlock_irqrestore(&ext2_lock, flags);
    return res;
}

static vfs_node_t *ext_create(vfs_node_t *dir_node, const char *name, vfs_node_type_t type) {
    if (!dir_node || !name || dir_node->type != VFS_DIRECTORY) {
        vfs_set_last_create_error(-VFS_EINVAL);
        return NULL;
    }
    ext2_inode_t *dir = dir_node->fs_private;
    if (!dir || !dir->fs) {
        vfs_set_last_create_error(-VFS_EIO);
        return NULL;
    }
    if (dir->fs->read_only) {
        vfs_set_last_create_error(-VFS_EROFS);
        return NULL;
    }
    if (dir->fs->tainted) {
        vfs_set_last_create_error(-VFS_EIO);
        return NULL;
    }
    ext2_fs_t *fs = dir->fs;

    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    vfs_node_t *result = NULL;

    ext2_inode_t existing;
    char found_name[VFS_MAX_NAME];
    int dres = dir_entry(dir, name, 0, found_name, &existing);
    if (dres == 1) {
        vfs_set_last_create_error(-VFS_EEXIST);
        goto done;
    } else if (dres != 0) {
        vfs_set_last_create_error(-VFS_EIO);
        goto done;
    }

    size_t plen = strlen(dir_node->path), nlen = strlen(name);
    if (plen + 1 + nlen >= VFS_MAX_PATH) {
        vfs_set_last_create_error(-VFS_EINVAL);
        goto done;
    }

    /* Pre-reserve memory for the VFS node and cached inode BEFORE any disk mutations */
    result = kcalloc(1, sizeof(*result) + sizeof(ext2_inode_t));
    if (!result) {
        vfs_set_last_create_error(-VFS_ENOMEM);
        goto done;
    }

    bool is_dir = (type == VFS_DIRECTORY);
    uint32_t ino = ext2_alloc_inode(fs, is_dir);
    if (!ino) {
        vfs_set_last_create_error(-VFS_ENOSPC);
        kfree(result);
        result = NULL;
        goto done;
    }

    uint32_t dir_blk = 0;
    if (is_dir) {
        dir_blk = ext2_alloc_block(fs);
        if (!dir_blk) {
            ext2_free_inode(fs, ino, is_dir);
            vfs_set_last_create_error(-VFS_ENOSPC);
            kfree(result);
            result = NULL;
            goto done;
        }
        uint8_t *dir_buf = kcalloc(1, fs->block_size);
        if (!dir_buf) {
            uint8_t *fb = kmalloc(fs->block_size);
            if (fb) { ext2_free_block(fs, dir_blk, fb); kfree(fb); }
            ext2_free_inode(fs, ino, is_dir);
            vfs_set_last_create_error(-VFS_ENOMEM);
            kfree(result);
            result = NULL;
            goto done;
        }
        /* Entry 1: "." */
        put32(dir_buf, ino);
        put16(dir_buf + 4, 12);
        if (fs->incompat & 2) {
            dir_buf[6] = 1;
            dir_buf[7] = 2; /* EXT2_FT_DIR */
        } else {
            put16(dir_buf + 6, 1);
        }
        dir_buf[8] = '.';

        /* Entry 2: ".." */
        put32(dir_buf + 12, dir->ino);
        put16(dir_buf + 16, (uint16_t)(fs->block_size - 12));
        if (fs->incompat & 2) {
            dir_buf[18] = 2;
            dir_buf[19] = 2; /* EXT2_FT_DIR */
        } else {
            put16(dir_buf + 18, 2);
        }
        dir_buf[20] = '.';
        dir_buf[21] = '.';

        uint64_t dir_blk_off = (uint64_t)dir_blk * fs->block_size;
        if (!write_bytes(fs, dir_blk_off, dir_buf, fs->block_size)) {
            kfree(dir_buf);
            uint8_t *fb = kmalloc(fs->block_size);
            if (fb) { ext2_free_block(fs, dir_blk, fb); kfree(fb); }
            ext2_free_inode(fs, ino, is_dir);
            fs->tainted = true;
            vfs_set_last_create_error(-VFS_EIO);
            kfree(result);
            result = NULL;
            goto done;
        }
        kfree(dir_buf);
    }

    ext2_inode_t new_in;
    memset(&new_in, 0, sizeof(new_in));
    new_in.fs = fs;
    new_in.ino = ino;
    new_in.mode = is_dir ? (0x4000 | 0755) : (0x8000 | 0644);
    new_in.links = is_dir ? 2 : 1;
    new_in.size = is_dir ? fs->block_size : 0;
    new_in.i_blocks = is_dir ? (fs->block_size / 512) : 0;
    new_in.file_acl = 0;
    if (is_dir) new_in.blocks[0] = dir_blk;

    uint32_t group = (ino - 1) / fs->ipg;
    uint64_t inode_off = (uint64_t)fs->group_descs[group].inode_table * fs->block_size +
                         (uint64_t)((ino - 1) % fs->ipg) * fs->inode_size;
    uint8_t raw[256];
    memset(raw, 0, sizeof(raw));
    put16(raw, new_in.mode);
    put32(raw + 4, new_in.size);
    put32(raw + 8, 1726500000);
    put32(raw + 12, 1726500000);
    put32(raw + 16, 1726500000);
    put32(raw + 20, 0);
    put16(raw + 24, 0);
    put16(raw + 26, new_in.links);
    put32(raw + 28, new_in.i_blocks);
    put32(raw + 32, 0);
    if (is_dir) put32(raw + 40, dir_blk);

    if (!write_bytes(fs, inode_off, raw, fs->inode_size)) {
        vfs_set_last_create_error(-VFS_EIO);
        fs->tainted = true;
        kfree(result);
        result = NULL;
        goto done;
    }
    if (fs->dev->flush && !block_flush(fs->dev)) {
        vfs_set_last_create_error(-VFS_EIO);
        fs->tainted = true;
        kfree(result);
        result = NULL;
        goto done;
    }

    ext2_inode_t *stored = (ext2_inode_t *)(result + 1);
    *stored = new_in;
    setup(result, stored);
    memcpy(result->name, name, nlen + 1);
    memcpy(result->path, dir_node->path, plen);
    if (plen > 1) result->path[plen++] = '/';
    memcpy(result->path + plen, name, nlen + 1);
    result->parent = dir_node;

    uint8_t file_type = is_dir ? 2 : 1;
    if (!ext2_add_dir_entry(dir, name, ino, file_type)) {
        /* Roll back only a definitely unpublished inode on a healthy mount.
         * Every uncertain directory write/flush must already have tainted it. */
        if (!fs->tainted && !fs->read_only) {
            if (is_dir && dir_blk) {
                uint8_t *fb = kmalloc(fs->block_size);
                if (fb) { ext2_free_block(fs, dir_blk, fb); kfree(fb); }
            }
            if (!ext2_free_inode(fs, ino, is_dir) || !block_flush(fs->dev))
                fs->tainted = true;
        }
        vfs_set_last_create_error(fs->tainted ? -VFS_EIO : -VFS_ENOSPC);
        kfree(result);
        result = NULL;
        goto done;
    }

    if (is_dir) {
        dir->links++;
        if (!write_inode_to_disk(dir)) {
            fs->tainted = true;
        }
    }

    result->next = dir_node->children;
    dir_node->children = result;
    fs->nodes++;
    vfs_set_last_create_error(VFS_SUCCESS);

done:
    spin_unlock_irqrestore(&ext2_lock, flags);
    return result;
}

static vfs_node_t *ext_lookup(vfs_node_t *parent, const char *name) {
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    vfs_node_t *result = NULL;
    if (!strcmp(name, ".")) { result = parent; goto done; }
    if (!strcmp(name, "..")) { result = parent->parent; goto done; }
    for (vfs_node_t *n = parent->children; n; n = n->next) {
        if (!strcmp(n->name, name)) { result = n; goto done; }
    }
    ext2_inode_t *dir = parent->fs_private, in;
    char actual[VFS_MAX_NAME];
    if (dir->fs->nodes >= EXT2_MAX_NODES ||
        dir_entry(dir, name, 0, actual, &in) != 1) goto done;
    size_t plen = strlen(parent->path), nlen = strlen(actual);
    if (plen + 1 + nlen >= VFS_MAX_PATH) goto done;
    result = kcalloc(1, sizeof(*result) + sizeof(in));
    if (!result) goto done;
    ext2_inode_t *stored = (ext2_inode_t *)(result + 1);
    *stored = in;
    setup(result, stored);
    memcpy(result->name, actual, nlen + 1);
    memcpy(result->path, parent->path, plen);
    result->path[plen] = '/';
    memcpy(result->path + plen + 1, actual, nlen + 1);
    result->parent = parent;
    result->next = parent->children;
    parent->children = result;
    dir->fs->nodes++;
done:
    spin_unlock_irqrestore(&ext2_lock, flags);
    return result;
}

static bool ext2_mount_internal(block_dev_t *dev, const char *path, bool writable) {
    if (!dev || !dev->read_sector ||
        (dev->sector_size != 512 && dev->sector_size != 4096) ||
        dev->sector_count > UINT64_MAX / dev->sector_size || !path ||
        strcmp(path, "/mnt") || vfs_lookup(path)) return false;

    /* Enforce prerequisites for writable mount */
    if (writable) {
        if (!dev->write_sector || !dev->flush) return false;
    }

   ext2_fs_t fs = {.dev = dev, .read_only = !writable, .tainted = false};
uint8_t sb[1024];
if (!bytes(&fs, 1024, sb, sizeof(sb)) || u16(sb + 56) != 0xef53 ||
    u32(sb + 24) > 2 || u32(sb + 76) > 1) return false;
/* Dirty filesystems may be mounted read-only (with a warning),
 * but a writable mount must refuse them: the on-disk state is
 * unknown and a write could compound the corruption. */
if (writable && u16(sb + 58) != 1) return false;
    fs.blocks = u32(sb + 4); fs.inodes = u32(sb);
    fs.free_blocks = u32(sb + 12); fs.free_inodes = u32(sb + 16);
    fs.first = u32(sb + 20); fs.block_size = 1024U << u32(sb + 24);
    fs.bpg = u32(sb + 32); fs.ipg = u32(sb + 40);
    fs.first_ino = u32(sb + 76) ? u32(sb + 84) : 11;
    fs.inode_size = u32(sb + 76) ? u16(sb + 88) : 128;
    fs.incompat = u32(sb + 76) ? u32(sb + 96) : 0;
    fs.ro_compat = u32(sb + 76) ? u32(sb + 100) : 0;
    fs.reserved_gdt_blocks = (u32(sb + 76) && (u32(sb + 92) & 0x10)) ? u16(sb + 206) : 0;

    /* Feature compatibility audit */
    if (u32(sb + 76)) {
        if ((u32(sb + 92) & ~0x38U) || (fs.incompat & ~2U) || (fs.ro_compat & ~3U)) {
            return false;
        }
    }
    if (writable) {
    /* Writable mount strictly requires: only FILETYPE (2) in incompat */
    if (fs.incompat & ~2U) return false;
    /* Ro-compat allowed for write: strictly SPARSE_SUPER (1) and LARGE_FILE (2) */
    if (fs.ro_compat & ~3U) return false;
    /* s_state == EXT2_VALID_FS (1) means clean shutdown. Anything else
     * means the filesystem was not cleanly unmounted. Linux's ext2
     * driver warns but still mounts RW; match that behavior. A stronger
     * recovery path (fsck, journal) is deferred. */
    if (u16(sb + 58) != 1) {
        serial_puts("[ext2] WARNING: filesystem was not cleanly unmounted.\n");
        serial_puts("[ext2] Mounting read-write anyway. Run a consistency check if you notice problems.\n");
    }
}

    if (u32(sb + 28) != u32(sb + 24) || u32(sb + 36) != fs.bpg ||
        fs.first != (fs.block_size == 1024 ? 1U : 0U) ||
        fs.blocks <= fs.first || !fs.inodes || !fs.bpg || !fs.ipg ||
        fs.bpg > fs.block_size * 8 || fs.ipg > fs.block_size * 8 ||
        fs.inode_size < 128 || fs.inode_size > fs.block_size ||
        (fs.inode_size & (fs.inode_size - 1)) ||
        fs.blocks > (dev->sector_count * dev->sector_size) / fs.block_size) return false;
    fs.groups = (uint32_t)(((uint64_t)fs.blocks - fs.first + fs.bpg - 1) / fs.bpg);
    if (!fs.groups || fs.groups > EXT2_MAX_GROUPS ||
        ((uint64_t)fs.inodes + fs.ipg - 1) / fs.ipg != fs.groups) return false;
    uint64_t gdt = (uint64_t)(fs.first + 1) * fs.block_size;
    if (gdt + (uint64_t)fs.groups * 32 > (uint64_t)fs.blocks * fs.block_size) return false;
    fs.group_descs = kmalloc(fs.groups * sizeof(ext2_group_desc_t));
    if (!fs.group_descs) return false;
    uint64_t table_blocks = ((uint64_t)fs.ipg * fs.inode_size + fs.block_size - 1) / fs.block_size;
    for (uint32_t g = 0; g < fs.groups; g++) {
        uint8_t gd[32];
        if (!bytes(&fs, gdt + (uint64_t)g * 32, gd, sizeof(gd))) goto fail;
        uint64_t start = fs.first + (uint64_t)g * fs.bpg, end = start + fs.bpg;
        if (end > fs.blocks) end = fs.blocks;
        uint32_t bb = u32(gd), ib = u32(gd + 4), it = u32(gd + 8);
        if (bb < start || bb >= end || ib < start || ib >= end || bb == ib ||
            it < start || it >= end || table_blocks > end - it ||
            (bb >= it && bb < it + table_blocks) ||
            (ib >= it && ib < it + table_blocks)) goto fail;
        fs.group_descs[g].block_bitmap = bb;
        fs.group_descs[g].inode_bitmap = ib;
        fs.group_descs[g].inode_table  = it;
        fs.group_descs[g].free_blocks  = u16(gd + 12);
        fs.group_descs[g].free_inodes  = u16(gd + 14);
        fs.group_descs[g].used_dirs    = u16(gd + 16);
    }
    ext2_inode_t root;
    if (!inode(&fs, 2, &root) || (root.mode & 0xf000) != 0x4000) goto fail;
    ext2_fs_t *mounted = kmalloc(sizeof(fs) + sizeof(root));
    if (!mounted) goto fail;
    *mounted = fs;
    ext2_inode_t *ri = (ext2_inode_t *)(mounted + 1);
    *ri = root; ri->fs = mounted;
    /* /mnt is the only supported mountpoint. Reserve its detached node first. */
    vfs_node_t *parent = vfs_lookup("/");
    vfs_node_t *node = kcalloc(1, sizeof(*node));
    if (!node || !parent) { kfree(node); kfree(mounted); goto fail; }
    memcpy(node->name, "mnt", 4);
    memcpy(node->path, "/mnt", 5);
    node->parent = parent;
    setup(node, ri);
    if (writable) {
        uint8_t state[2];
        put16(state, 0);
        if (!write_bytes(mounted, 1024 + 58, state, 2) || !block_flush(dev)) {
            kfree(node);
            kfree(mounted);
            goto fail;
        }
    }
    /* No fallible operation remains after the durable dirty marker. */
    node->next = parent->children;
    parent->children = node;
    mounted->nodes = 1;
    if (writable) g_mounted_ext2 = mounted;

    return true;
fail:
    kfree(fs.group_descs);
    return false;
}

bool ext2_sync_all(void) {
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    ext2_fs_t *fs = g_mounted_ext2;
    bool ok = true;
    if (fs && fs->tainted) {
        ok = false; /* Never write or flush uncertain pending metadata. */
    } else if (fs && !fs->read_only) {
        uint8_t state[2];
        put16(state, 1);
        ok = block_flush(fs->dev) &&
             write_bytes(fs, 1024 + 58, state, 2) && block_flush(fs->dev);
        if (!ok) fs->tainted = true;
        /* Shutdown-only operation: no mutations may follow the clean marker. */
        fs->read_only = true;
    }
    spin_unlock_irqrestore(&ext2_lock, flags);
    return ok;
}

bool ext2_mount(block_dev_t *dev, const char *path) {
    return ext2_mount_internal(dev, path, false);
}

bool ext2_mount_rw(block_dev_t *dev, const char *path) {
    return ext2_mount_internal(dev, path, true);
}
