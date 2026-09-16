#include "ext2.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"

#define EXT2_MAX_GROUPS 4096U
#define EXT2_MAX_NODES 1024U
#define EXT2_MAX_DIRECTORY (1024U * 1024U)
#define EXT2_MAX_READ (64U * 1024U)

typedef struct {
    block_dev_t *dev;
    uint32_t blocks, inodes, first, block_size, bpg, ipg, groups, inode_size;
    uint32_t incompat;
    uint32_t *tables;
    size_t nodes;
} ext2_fs_t;

typedef struct {
    ext2_fs_t *fs;
    uint32_t ino;
    uint16_t mode;
    uint32_t size;
    uint32_t blocks[15];
} ext2_inode_t;

/* Serializes synchronous reads and cache publication on the bootstrap CPU.
 * Replace with sleepable I/O locking before asynchronous storage or SMP. */
static spinlock_t ext2_lock = SPINLOCK_RANKED(1, "ext2");
static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return u16(p) | ((uint32_t)u16(p + 2) << 16);
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

static bool inode(ext2_fs_t *fs, uint32_t number, ext2_inode_t *out) {
    if (!number || number > fs->inodes) return false;
    uint32_t group = (number - 1) / fs->ipg;
    if (group >= fs->groups) return false;
    uint64_t off = (uint64_t)fs->tables[group] * fs->block_size +
                   (uint64_t)((number - 1) % fs->ipg) * fs->inode_size;
    uint8_t raw[128];
    if (!bytes(fs, off, raw, sizeof(raw))) return false;
    memset(out, 0, sizeof(*out));
    out->fs = fs; out->ino = number; out->mode = u16(raw);
    out->size = u32(raw + 4);
    uint16_t type = out->mode & 0xf000;
    if (type != 0x8000 && type != 0x4000) return false;
    /* Reject indexed directories, compression, extents and other inode flags.
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

static bool file_block(ext2_inode_t *in, uint64_t index, uint32_t *out) {
    ext2_fs_t *fs = in->fs;
    uint64_t n = fs->block_size / 4, span = n;
    if (index < 12) { *out = in->blocks[index]; return true; }
    index -= 12;
    unsigned level;
    for (level = 1; level <= 3; level++) {
        if (index < span) break;
        index -= span;
        span *= n;
    }
    if (level > 3) return false;
    uint32_t block = in->blocks[11 + level];
    /* Fixed decreasing depth: repeated block numbers cannot cause recursion. */
    while (level--) {
        if (!block) { *out = 0; return true; }
        if (block < fs->first || block >= fs->blocks) return false;
        span /= n;
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

/* Directory scan is bounded by the 1 MiB supported directory limit. */
static int dir_entry(ext2_inode_t *dir, const char *wanted, uint64_t index,
                     char *name, ext2_inode_t *found) {
    uint64_t off = 0, visible = 0;
    while (off < dir->size) {
        uint8_t h[8];
        if (read_inode(dir, off, h, 8) != 8) return -1;
        uint32_t ino = u32(h);
        uint16_t rec = u16(h + 4);
        uint16_t len = dir->fs->incompat & 2 ? h[6] : u16(h + 6);
        if (rec < 8 || rec % 4 || rec > dir->fs->block_size - off % dir->fs->block_size ||
            rec > dir->size - off || len > rec - 8 || len > 255) return -1;
        if (ino) {
            char full[256];
            if (!len || ino > dir->fs->inodes ||
                read_inode(dir, off + 8, full, len) != len) return -1;
            for (unsigned i = 0; i < len; i++) if (!full[i] || full[i] == '/') return -1;
            full[len] = 0;
            if (strcmp(full, ".") && strcmp(full, "..")) {
                if ((wanted && !strcmp(full, wanted)) || (!wanted && visible == index)) {
                    if (len >= VFS_MAX_NAME || !inode(dir->fs, ino, found)) return -1;
                    memcpy(name, full, len + 1);
                    return 1;
                }
                visible++;
            }
        }
        off += rec;
    }
    return 0;
}

static int64_t ext_read(vfs_node_t *node, uint64_t off, void *buf, size_t len) {
    if (len > EXT2_MAX_READ) len = EXT2_MAX_READ;
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int64_t r = read_inode(node->fs_private, off, buf, len);
    spin_unlock_irqrestore(&ext2_lock, flags);
    return r;
}

static int ext_readdir(vfs_node_t *node, uint64_t index, void *output) {
    vfs_dirent_t *dent = output;
    ext2_inode_t in;
    uint64_t flags = spin_lock_irqsave(&ext2_lock);
    int r = dir_entry(node->fs_private, NULL, index, dent->name, &in);
    if (r == 1) {
        dent->size = in.size;
        dent->type = (in.mode & 0xf000) == 0x4000 ? VFS_DIRECTORY : VFS_FILE;
    }
    spin_unlock_irqrestore(&ext2_lock, flags);
    return r;
}

static vfs_node_t *ext_lookup(vfs_node_t *parent, const char *name);
static void setup(vfs_node_t *node, ext2_inode_t *in) {
    node->fs_private = in;
    node->size = in->size;
    node->type = (in->mode & 0xf000) == 0x4000 ? VFS_DIRECTORY : VFS_FILE;
    node->read = ext_read;
    if (node->type == VFS_DIRECTORY) {
        node->lookup = ext_lookup;
        node->readdir = ext_readdir;
    }
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
    /* One allocation owns both cache node and immutable inode snapshot. */
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

bool ext2_mount(block_dev_t *dev, const char *path) {
    if (!dev || !dev->read_sector ||
        (dev->sector_size != 512 && dev->sector_size != 4096) ||
        dev->sector_count > UINT64_MAX / dev->sector_size || !path ||
        strcmp(path, "/mnt") || vfs_lookup(path)) return false;
    ext2_fs_t fs = {.dev = dev};
    uint8_t sb[1024];
    if (!bytes(&fs, 1024, sb, sizeof(sb)) || u16(sb + 56) != 0xef53 ||
        u32(sb + 24) > 2 || u32(sb + 76) > 1 || u16(sb + 58) != 1) return false;
    fs.blocks = u32(sb + 4); fs.inodes = u32(sb);
    fs.first = u32(sb + 20); fs.block_size = 1024U << u32(sb + 24);
    fs.bpg = u32(sb + 32); fs.ipg = u32(sb + 40);
    fs.inode_size = u32(sb + 76) ? u16(sb + 88) : 128;
    fs.incompat = u32(sb + 76) ? u32(sb + 96) : 0;
    /* Supported compat: ext_attr, resize_inode, dir_index (linear directories
     * only). Supported ro_compat: sparse_super, large_file (low 32-bit sizes). */
    if (u32(sb + 76) && ((u32(sb + 92) & ~0x38U) ||
        (fs.incompat & ~2U) || (u32(sb + 100) & ~3U))) return false;
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
    fs.tables = kmalloc(fs.groups * sizeof(uint32_t));
    if (!fs.tables) return false;
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
        fs.tables[g] = it;
    }
    ext2_inode_t root;
    if (!inode(&fs, 2, &root) || (root.mode & 0xf000) != 0x4000) goto fail;
    ext2_fs_t *mounted = kmalloc(sizeof(fs) + sizeof(root));
    if (!mounted) goto fail;
    *mounted = fs;
    ext2_inode_t *ri = (ext2_inode_t *)(mounted + 1);
    *ri = root; ri->fs = mounted;
    /* Publication is last; all failures above leave the VFS untouched. */
    vfs_node_t *node = vfs_create_node(path, VFS_DIRECTORY, root.size, NULL);
    if (!node) { kfree(mounted); goto fail; }
    setup(node, ri);
    mounted->nodes = 1;
    return true;
fail:
    kfree(fs.tables);
    return false;
}
