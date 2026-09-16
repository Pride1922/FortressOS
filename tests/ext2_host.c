/* Compile the actual filesystem sources with host allocation/I/O adapters.
 * Hosted headers are confined to tests; no host runtime enters the kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "types.h"
#include "block.h"

static void *allocations[4096];
static size_t live;
static int fail_after = -1;
void *kmalloc(size_t n) {
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(n);
    assert(p && live < 4096);
    allocations[live++] = p;
    return p;
}
void *kcalloc(size_t a, size_t b) {
    if (a && b > SIZE_MAX / a) return NULL;
    void *p = kmalloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}
void kfree(void *p) {
    if (!p) return;
    size_t i;
    for (i = 0; i < live && allocations[i] != p; i++);
    assert(i < live);
    allocations[i] = allocations[--live];
    free(p);
}
void serial_puts(const char *s) { (void)s; }
static uint8_t *disk;
static size_t disk_size;
static bool io_failure;
static size_t reads;
static bool read_sector(block_dev_t *dev, uint64_t lba, void *out) {
    assert(lba < dev->sector_count);
    reads++;
    if (io_failure) return false;
    memcpy(out, disk + lba * dev->sector_size, dev->sector_size);
    return true;
}
bool block_read_sector(block_dev_t *dev, uint64_t lba, void *out) {
    return dev && out && lba < dev->sector_count && dev->read_sector(dev, lba, out);
}

#include "../src/fs/vfs.c"
#include "../src/fs/ext2.c"

static void reset(void) {
    while (live) free(allocations[--live]);
    g_vfs_root = NULL;
    fail_after = -1;
    io_failure = false;
    vfs_init();
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
static void rejected_field(block_dev_t *dev, size_t offset, uint32_t value) {
    uint8_t saved[4];
    memcpy(saved, disk + offset, 4);
    put32(disk + offset, value);
    size_t baseline = live;
    assert(!ext2_mount(dev, "/mnt"));
    assert(!vfs_lookup("/mnt") && live == baseline);
    memcpy(disk + offset, saved, 4);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *fp = fopen(argv[1], "rb");
    assert(fp);
    assert(!fseek(fp, 0, SEEK_END));
    disk_size = ftell(fp);
    rewind(fp);
    disk = malloc(disk_size);
    assert(disk && fread(disk, 1, disk_size, fp) == disk_size);
    fclose(fp);
    uint32_t ss = (uint32_t)strtoul(argv[2], NULL, 10);
    block_dev_t dev = {.sector_size = ss, .sector_count = disk_size / ss,
                       .read_sector = read_sector};
    reset();
    rejected_field(&dev, 1024 + 56, 0);          /* magic */
    rejected_field(&dev, 1024 + 24, 32);         /* shift overflow */
    rejected_field(&dev, 1024 + 32, 0);          /* zero bpg */
    rejected_field(&dev, 1024 + 40, 0);          /* zero ipg */
    rejected_field(&dev, 1024 + 4, UINT32_MAX);  /* capacity */
    rejected_field(&dev, 1024 + 96, 0x40);       /* extents */
    rejected_field(&dev, 1024 + 92, 4);          /* journal */
    rejected_field(&dev, 1024 + 100, 0x400);     /* unknown ro feature */
    rejected_field(&dev, 1024 + 88, 129);        /* inode size */
    rejected_field(&dev, 1024 + 58, 0);          /* unclean filesystem */
    uint32_t bs = 1024U << u32(disk + 1024 + 24);
    uint32_t first = u32(disk + 1024 + 20);
    size_t gdt = (first + 1) * bs;
    rejected_field(&dev, gdt + 8, UINT32_MAX);   /* inode table */
    uint64_t old_capacity = dev.sector_count;
    dev.sector_count = 1;
    assert(!ext2_mount(&dev, "/mnt") && live == 1);
    dev.sector_count = UINT64_MAX;
    assert(!ext2_mount(&dev, "/mnt") && live == 1);
    dev.sector_count = old_capacity;
    io_failure = true;
    assert(!ext2_mount(&dev, "/mnt") && live == 1);
    io_failure = false;
    /* All three allocations in mount fail transactionally. */
    for (int i = 0; i < 3; i++) {
        fail_after = i;
        assert(!ext2_mount(&dev, "/mnt") && live == 1);
    }
    fail_after = -1;
    assert(ext2_mount(&dev, "/mnt"));
    size_t mounted = live;
    fail_after = 0;
    assert(!vfs_lookup("/mnt/hello.txt") && live == mounted);
    fail_after = -1;
    file_t *f = vfs_open("/mnt/hello.txt", 0);
    assert(f);
    char out[1024];
    const char *expected = "Hello from FortressOS ext2 NVMe partition!\n";
    assert(vfs_read(f, out, sizeof(out)) == (int64_t)strlen(expected));
    assert(!memcmp(out, expected, strlen(expected)));
    assert(vfs_read(f, out, sizeof(out)) == 0);
    vfs_close(f);
    assert(vfs_lookup("/mnt/nested/note.txt"));
    assert(!vfs_lookup("/mnt/hello.txt/child"));
    assert(!vfs_open("/mnt/hello.txt", 1));
    f = vfs_open("/mnt/sparse.bin", 0);
    assert(f);
    size_t sparse_off = 0;
    int64_t sparse_n;
    while ((sparse_n = vfs_read(f, out, sizeof(out))) > 0) {
        for (int64_t i = 0; i < sparse_n; i++) {
            size_t pos = sparse_off + i;
            assert(pos < 20003);
            assert(out[i] == (pos < 20000 ? 0 : "END"[pos - 20000]));
        }
        sparse_off += sparse_n;
    }
    assert(sparse_n == 0 && sparse_off == 20003);
    vfs_close(f);
    f = vfs_open("/mnt/large.bin", 0);
    assert(f);
    size_t off = 0;
    int64_t n;
    while ((n = vfs_read(f, out, sizeof(out))) > 0) {
        for (int64_t i = 0; i < n; i++) assert((uint8_t)out[i] == (uint8_t)((off + i) * 17 + 3));
        off += n;
    }
    assert(n == 0 && off == 400000);
    ext2_inode_t *in = f->node->fs_private;
    /* Corrupt indirect entry: fail without reading outside the partition. */
    size_t indirect = (size_t)in->blocks[12] * bs;
    uint32_t saved = u32(disk + indirect);
    put32(disk + indirect, UINT32_MAX);
    f->offset = 12 * bs;
    assert(vfs_read(f, out, 1) < 0 && f->offset == 12 * bs);
    put32(disk + indirect, saved);
    /* Repeated references still terminate at fixed depth. */
    ext2_inode_t synthetic = *in;
    synthetic.blocks[14] = in->blocks[12];
    put32(disk + indirect, in->blocks[12]);
    uint32_t mapped;
    size_t calls = reads;
    uint64_t per = bs / 4;
    assert(file_block(&synthetic, 12 + per + per * per, &mapped));
    assert(reads - calls <= 3);
    put32(disk + indirect, saved);
    vfs_close(f);
    vfs_node_t *root = vfs_lookup("/mnt");
    ext2_inode_t *ri = root->fs_private;
    size_t dir = (size_t)ri->blocks[0] * bs;
    uint8_t rec[4];
    memcpy(rec, disk + dir + 4, 4);
    put32(disk + dir + 4, 0); /* zero record cannot loop forever */
    vfs_dirent_t dent;
    assert(vfs_readdir(root, 0, &dent) < 0);
    memcpy(disk + dir + 4, rec, 4);
    io_failure = true;
    f = vfs_open("/mnt/hello.txt", 0); /* cached lookup succeeds */
    assert(f && vfs_read(f, out, 1) < 0 && f->offset == 0);
    vfs_close(f);
    io_failure = false;
    reset();
    while (live) free(allocations[--live]);
    free(disk);
    printf("PASS ext2: block=%u sector=%u, malformed metadata, I/O/OOM rollback, indirect bounds\n", bs, ss);
}
