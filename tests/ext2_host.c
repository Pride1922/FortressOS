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
static uint8_t *pending_disk;
static uint8_t *durable_disk;
#define disk pending_disk
static size_t disk_size;
static bool io_failure;
static bool write_failure;
static bool flush_failure;
static size_t reads;
static size_t writes;
static size_t flushes;
static size_t fail_write_at, fail_flush_at, writes_at_failure;

static bool read_sector(block_dev_t *dev, uint64_t lba, void *out) {
    assert(lba < dev->sector_count);
    reads++;
    if (io_failure) return false;
    memcpy(out, pending_disk + lba * dev->sector_size, dev->sector_size);
    return true;
}
bool block_read_sector(block_dev_t *dev, uint64_t lba, void *out) {
    return dev && out && lba < dev->sector_count && dev->read_sector(dev, lba, out);
}
static bool write_sector(block_dev_t *dev, uint64_t lba, const void *in) {
    assert(lba < dev->sector_count);
    writes++;
    if (io_failure || write_failure || writes == fail_write_at) {
        writes_at_failure = writes; return false;
    }
    memcpy(pending_disk + lba * dev->sector_size, in, dev->sector_size);
    return true;
}
bool block_write_sector(block_dev_t *dev, uint64_t lba, const void *in) {
    return dev && in && lba < dev->sector_count && dev->write_sector && dev->write_sector(dev, lba, in);
}
static bool flush_sector(block_dev_t *dev) {
    (void)dev;
    flushes++;
    if (io_failure || flush_failure || flushes == fail_flush_at) {
        writes_at_failure = writes; return false;
    }
    /* Durability barrier: successful flush advances durable state */
    memcpy(durable_disk, pending_disk, disk_size);
    return true;
}
bool block_flush(block_dev_t *dev) {
    return dev && (!dev->flush || dev->flush(dev));
}
static void simulate_crash(void) {
    /* Power loss/crash: discard all un-flushed pending writes */
    memcpy(pending_disk, durable_disk, disk_size);
}

#include "../src/fs/vfs.c"
#include "../src/fs/ext2.c"

static void reset(void) {
    while (live) free(allocations[--live]);
    g_vfs_root = NULL;
    g_mounted_ext2 = NULL;
    fail_after = -1;
    io_failure = false;
    write_failure = false;
    flush_failure = false;
    fail_write_at = fail_flush_at = 0;
    vfs_init();
}

static void rejected_field(block_dev_t *dev, size_t offset, uint32_t value) {
    uint8_t saved[4];
    memcpy(saved, disk + offset, 4);
    put32(disk + offset, value);
    memcpy(durable_disk, pending_disk, disk_size);
    size_t baseline = live;
    assert(!ext2_mount(dev, "/mnt"));
    assert(!vfs_lookup("/mnt") && live == baseline);
    memcpy(disk + offset, saved, 4);
    memcpy(durable_disk, pending_disk, disk_size);
}
/* Each case starts from the exact original disk and fresh mount/cache state. */
static void review_regressions(block_dev_t *dev) {
    uint8_t *original = malloc(disk_size);
    assert(original);
    memcpy(original, disk, disk_size);
    for (int test = 0; test < 14; test++) {
        reset();
        memcpy(disk, original, disk_size);
        memcpy(durable_disk, original, disk_size);
        size_t w = writes, f = flushes;
        if (test == 0) {
            assert(ext2_sync_all());
            assert(ext2_mount(dev, "/mnt"));
            assert(ext2_sync_all());
            assert(writes == w && flushes == f);
            continue;
        }
        if (test >= 1 && test <= 3) {
            fail_after = test - 1;
            assert(!ext2_mount_rw(dev, "/mnt"));
            assert(!g_mounted_ext2 && !vfs_lookup("/mnt"));
            assert(writes == w && flushes == f && live == 1);
            assert(!memcmp(disk, original, disk_size));
            continue;
        }
        if (test == 4 || test == 12) {
            if (test == 4) fail_flush_at = flushes + 1;
            else fail_write_at = writes + 1;
            assert(!ext2_mount_rw(dev, "/mnt"));
            assert(!g_mounted_ext2 && !vfs_lookup("/mnt") && live == 1);
            w = writes; f = flushes;
            assert(ext2_sync_all());
            assert(writes == w && flushes == f);
            continue;
        }
        assert(ext2_mount_rw(dev, "/mnt"));
        ext2_fs_t *fs = g_mounted_ext2;
        if (test == 11) {
            fail_flush_at = flushes + 2; /* final clean-marker flush */
            assert(!ext2_sync_all() && fs->tainted);
            w = writes; f = flushes;
            assert(!ext2_sync_all() && writes == w && flushes == f);
            assert(u16(durable_disk + 1082) == 0);
            continue;
        }
        if (test == 13) {
            file_t *frozen = vfs_open("/mnt/frozen", VFS_O_CREAT | VFS_O_RDWR);
            assert(frozen && ext2_sync_all());
            w = writes; f = flushes;
            assert(vfs_write(frozen, "x", 1) == -VFS_EROFS);
            assert(vfs_truncate(frozen->node, 0) == -VFS_EROFS);
            assert(ext2_sync_all() && writes == w && flushes == f);
            vfs_close(frozen);
            continue;
        }
        if (test == 5) {
            /* Inode reservation succeeds; initialization flush fails. */
            fail_flush_at = flushes + 2;
            assert(!vfs_open("/mnt/review.txt", VFS_O_CREAT | VFS_O_RDWR));
            assert(fs->tainted && writes == writes_at_failure);
        } else {
            file_t *file = vfs_open("/mnt/review.txt", VFS_O_CREAT | VFS_O_RDWR);
            assert(file);
            ext2_inode_t *in = file->node->fs_private;
            if (test == 6) {
                uint32_t free_before = fs->free_blocks;
                w = writes;
                fail_after = 1; /* zero buffer succeeds, bitmap buffer fails */
                assert(vfs_write(file, "x", 1) < 0);
                fail_after = -1;
                assert(fs->free_blocks == free_before && writes == w && !fs->tainted);
                assert(in->blocks[0] == 0);
            } else if (test == 7) {
                in->blocks[0] = fs->group_descs[0].block_bitmap;
                w = writes;
                assert(vfs_write(file, "x", 1) == -VFS_EIO);
                assert(writes == w && fs->tainted);
            } else {
                assert(vfs_write(file, "x", 1) == 1);
                uint32_t block = in->blocks[0], free_before = fs->free_blocks;
                w = writes;
                if (test == 8) {
                    fail_after = 0;
                    assert(vfs_truncate(file->node, 0) == -VFS_ENOMEM);
                    fail_after = -1;
                    assert(in->size == 1 && in->blocks[0] == block && writes == w);
                    assert(fs->free_blocks == free_before && !fs->tainted);
                    /* Only one allocation is needed for direct reclamation. */
                    fail_after = 1;
                    assert(vfs_truncate(file->node, 0) == 0);
                    fail_after = -1;
                    assert(fs->free_blocks == free_before + 1);
                } else if (test == 9) {
                    fail_write_at = writes + 2; /* inode detaches, bitmap write fails */
                    assert(vfs_truncate(file->node, 0) == -VFS_EIO);
                    assert(fs->tainted && writes == writes_at_failure);
                } else {
                    /* Reserved GDT blocks must not be accepted as file data. */
                    uint16_t saved = fs->reserved_gdt_blocks;
                    fs->reserved_gdt_blocks = 1;
                    uint32_t reserved = fs->first + 1 +
                        (fs->groups * 32 + fs->block_size - 1) / fs->block_size;
                    assert(ext2_is_metadata_block(fs, reserved));
                    fs->reserved_gdt_blocks = saved;
                }
            }
            vfs_close(file);
        }
        w = writes; f = flushes;
        if (fs->tainted) {
            assert(!ext2_sync_all());
            assert(writes == w && flushes == f);
            assert(u16(durable_disk + 1082) == 0);
        }
    }
    reset();
    memcpy(disk, original, disk_size);
    memcpy(durable_disk, original, disk_size);
    free(original);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *fp = fopen(argv[1], "rb");
    assert(fp);
    assert(!fseek(fp, 0, SEEK_END));
    disk_size = ftell(fp);
    rewind(fp);
    pending_disk = malloc(disk_size);
    durable_disk = malloc(disk_size);
    assert(pending_disk && durable_disk && fread(pending_disk, 1, disk_size, fp) == disk_size);
    memcpy(durable_disk, pending_disk, disk_size);
    fclose(fp);
    uint32_t ss = (uint32_t)strtoul(argv[2], NULL, 10);
    block_dev_t dev = {.sector_size = ss, .sector_count = disk_size / ss,
                       .read_sector = read_sector, .write_sector = write_sector,
                       .flush = flush_sector};
    review_regressions(&dev);
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

    /* On read-only mount, write opens and creation MUST be rejected */
    assert(vfs_open("/mnt/hello.txt", VFS_O_WRONLY) == NULL);
    assert(vfs_open("/mnt/newfile.txt", VFS_O_WRONLY | VFS_O_CREAT) == NULL);
    int ro_err = 0;
    assert(vfs_open_ext("/mnt/hello.txt", VFS_O_WRONLY, &ro_err) == NULL && ro_err == -VFS_EROFS);
    assert(vfs_open_ext("/mnt/nonexistent.txt", VFS_O_RDONLY, &ro_err) == NULL && ro_err == -VFS_ENOENT);
    vfs_node_t *ro_node = vfs_lookup("/mnt/hello.txt");
    assert(ro_node);
    assert(vfs_truncate(ro_node, 0) == -30); /* -EROFS */
    assert(ext_truncate(ro_node, 0) == -30); /* -EROFS */
    assert(ext_write(ro_node, 0, "fail", 4) == -30); /* -EROFS */

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

    /* =========================================================================
     * Explicit Opt-In Read-Write Mount & Mutation Tests (Checkpoint 9D.1 & 9D.2)
     * ========================================================================= */
    reset();

    /* Device without write_sector MUST reject RW mount */
    dev.write_sector = NULL;
    assert(!ext2_mount_rw(&dev, "/mnt"));
    dev.write_sector = write_sector;

    /* Mount read-write explicitly */
    assert(ext2_mount_rw(&dev, "/mnt"));
    vfs_node_t *mnt_node = vfs_lookup("/mnt");
    assert(mnt_node);
    ext2_inode_t *mnt_in = mnt_node->fs_private;
    ext2_fs_t *fs = mnt_in->fs;
    assert(!fs->read_only);

    /* 1. Basic write, readback, and EBADF mode enforcement */
    file_t *fw = vfs_open("/mnt/newfile.txt", VFS_O_WRONLY | VFS_O_CREAT);
    assert(fw);
    const char *new_text = "Phase 9D write test file content!";
    assert(vfs_write(fw, new_text, strlen(new_text)) == (int64_t)strlen(new_text));
    /* Reading from write-only descriptor must fail with -9 (EBADF) */
    assert(vfs_read(fw, out, sizeof(out)) == -9);
    vfs_close(fw);

    file_t *fr = vfs_open("/mnt/newfile.txt", VFS_O_RDONLY);
    assert(fr);
    char new_buf[64];
    assert(vfs_read(fr, new_buf, sizeof(new_buf)) == (int64_t)strlen(new_text));
    assert(!memcmp(new_buf, new_text, strlen(new_text)));
    /* Writing to read-only descriptor must fail with -9 (EBADF) */
    assert(vfs_write(fr, "forbidden", 9) == -9);
    vfs_close(fr);

    /* 2. Direct-to-indirect boundary test & exact allocation auditing */
    uint32_t free_blks_before = fs->free_blocks;
    uint32_t bg_free_blks_before = fs->group_descs[0].free_blocks;
    uint32_t free_inos_before = fs->free_inodes;

    file_t *findir = vfs_open("/mnt/indir.bin", VFS_O_WRONLY | VFS_O_CREAT);
    assert(findir);
    size_t indir_bytes = 14 * bs; /* 12 direct + 2 indirect blocks */
    uint8_t *indir_pattern = malloc(indir_bytes);
    assert(indir_pattern);
    for (size_t i = 0; i < indir_bytes; i++) indir_pattern[i] = (uint8_t)(i * 37 + 11);
    assert(vfs_write(findir, indir_pattern, indir_bytes) == (int64_t)indir_bytes);
    vfs_close(findir);

    /* 14 data blocks + 1 indirect block table (blocks[12]) = 15 blocks allocated; 1 inode allocated */
    assert(fs->free_blocks == free_blks_before - 15);
    assert(fs->group_descs[0].free_blocks == bg_free_blks_before - 15);
    assert(fs->free_inodes == free_inos_before - 1);

    /* Verify data read back across direct and indirect blocks */
    findir = vfs_open("/mnt/indir.bin", VFS_O_RDONLY);
    assert(findir);
    uint8_t *read_back = malloc(indir_bytes);
    assert(read_back);
    assert(vfs_read(findir, read_back, indir_bytes) == (int64_t)indir_bytes);
    assert(!memcmp(read_back, indir_pattern, indir_bytes));
    vfs_close(findir);
    free(read_back);
    free(indir_pattern);

    /* 3. Truncation with complete direct and indirect block reclamation */
    findir = vfs_open("/mnt/indir.bin", VFS_O_WRONLY | VFS_O_TRUNC);
    assert(findir);
    vfs_close(findir);
    /* All 15 blocks reclaimed */
    assert(fs->free_blocks == free_blks_before);
    assert(fs->group_descs[0].free_blocks == bg_free_blks_before);
    findir = vfs_open("/mnt/indir.bin", VFS_O_RDONLY);
    assert(findir && findir->node->size == 0);
    vfs_close(findir);

    /* 4. Repeated shorter and empty saves on existing file */
    /* (a) Write 500 bytes (1 block) */
    file_t *fshort = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(fshort);
    char buf500[500];
    memset(buf500, 'K', sizeof(buf500));
    assert(vfs_write(fshort, buf500, sizeof(buf500)) == sizeof(buf500));
    vfs_close(fshort);
    assert(fs->free_blocks == free_blks_before - 1);

    /* (b) Reopen with O_TRUNC and write 50 bytes */
    fshort = vfs_open("/mnt/indir.bin", VFS_O_WRONLY | VFS_O_TRUNC);
    assert(fshort);
    char buf50[50];
    memset(buf50, 'Z', sizeof(buf50));
    assert(vfs_write(fshort, buf50, sizeof(buf50)) == sizeof(buf50));
    vfs_close(fshort);
    fshort = vfs_open("/mnt/indir.bin", VFS_O_RDONLY);
    assert(fshort && fshort->node->size == 50);
    vfs_close(fshort);

    /* (c) Reopen with O_TRUNC and save empty (0 bytes) */
    fshort = vfs_open("/mnt/indir.bin", VFS_O_WRONLY | VFS_O_TRUNC);
    assert(fshort);
    vfs_close(fshort);
    assert(fs->free_blocks == free_blks_before);
    fshort = vfs_open("/mnt/indir.bin", VFS_O_RDONLY);
    assert(fshort && fshort->node->size == 0);
    vfs_close(fshort);

    /* 5. Inode and block exhaustion (disk full / inode full) */
    uint32_t saved_free_inos = fs->free_inodes;
    fs->free_inodes = 0;
    assert(vfs_open("/mnt/no_inode.txt", VFS_O_WRONLY | VFS_O_CREAT) == NULL);
    fs->free_inodes = saved_free_inos;

    uint32_t saved_free_blks = fs->free_blocks;
    fs->free_blocks = 0;
    file_t *fnospc = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(fnospc);
    assert(vfs_write(fnospc, "block_fail", 10) < 0);
    vfs_close(fnospc);
    fs->free_blocks = saved_free_blks;

    /* 6. Injected kmalloc failure during write */
    file_t *foom = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(foom);
    fail_after = 0;
    assert(vfs_write(foom, "data", 4) < 0);
    fail_after = -1;
    vfs_close(foom);

    /* 7. Unsupported indirection: double/triple indirect file write rejection */
    file_t *flarge = vfs_open("/mnt/large.bin", VFS_O_WRONLY);
    assert(flarge);
    ext2_inode_t *in_large = flarge->node->fs_private;
    if (in_large->blocks[13] != 0 || in_large->blocks[14] != 0) {
        assert(vfs_write(flarge, "xyz", 3) < 0);
    } else {
        uint32_t saved_b13 = in_large->blocks[13];
        in_large->blocks[13] = 9999;
        assert(vfs_write(flarge, "xyz", 3) < 0);
        in_large->blocks[13] = saved_b13;
    }
    vfs_close(flarge);

    /* 8. Pre-rejection of duplicate block pointers during truncation (-EINVAL / -22) */
    file_t *fdup = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(fdup);
    ext2_inode_t *in_dup = fdup->node->fs_private;
    uint32_t saved_b0 = in_dup->blocks[0];
    uint32_t saved_b1 = in_dup->blocks[1];
    in_dup->blocks[0] = 500;
    in_dup->blocks[1] = 500;
    assert(vfs_truncate(fdup->node, 0) == -22);
    in_dup->blocks[0] = saved_b0;
    in_dup->blocks[1] = saved_b1;

    /* 9. Pre-rejection of metadata block pointers during truncation (-EINVAL / -22) */
    in_dup->blocks[0] = fs->group_descs[0].block_bitmap;
    assert(vfs_truncate(fdup->node, 0) == -22);
    in_dup->blocks[0] = fs->group_descs[0].inode_table;
    assert(vfs_truncate(fdup->node, 0) == -22);
    in_dup->blocks[0] = saved_b0;

    /* 10. Pre-rejection of external extended-attribute block (file_acl != 0) with -EOPNOTSUPP (-95) */
    in_dup->file_acl = 1000;
    assert(vfs_truncate(fdup->node, 0) == -95);
    assert(vfs_write(fdup, "data", 4) == -95);
    in_dup->file_acl = 0;

    /* 11. Truncation pre-validation on zero-size file with invalid block pointer */
    uint32_t saved_sz = in_dup->size;
    in_dup->size = 0;
    in_dup->blocks[0] = fs->group_descs[0].inode_bitmap;
    assert(vfs_truncate(fdup->node, 0) == -22);
    in_dup->blocks[0] = saved_b0;
    in_dup->size = saved_sz;
    vfs_close(fdup);

    /* 11b. Phase 9E: Directory creation, nesting, non-empty rejection, rename, and unlink */
    assert(vfs_mkdir("/mnt/testdir", 0755) == VFS_SUCCESS);
    vfs_node_t *tdir = vfs_lookup("/mnt/testdir");
    assert(tdir && tdir->type == VFS_DIRECTORY);
    /* Duplicate directory creation must fail */
    assert(vfs_mkdir("/mnt/testdir", 0755) != VFS_SUCCESS);

    /* Create file inside newly created directory */
    file_t *fsub = vfs_open("/mnt/testdir/hello.txt", VFS_O_WRONLY | VFS_O_CREAT);
    assert(fsub);
    assert(vfs_write(fsub, "inside_dir", 10) == 10);
    vfs_close(fsub);

    /* Directory unlink MUST be rejected with -ENOTEMPTY while child exists */
    assert(vfs_unlink("/mnt/testdir") == -VFS_ENOTEMPTY);

    /* Rename file within same directory */
    assert(vfs_rename("/mnt/testdir/hello.txt", "/mnt/testdir/renamed.txt") == VFS_SUCCESS);
    assert(vfs_lookup("/mnt/testdir/hello.txt") == NULL);
    file_t *fren = vfs_open("/mnt/testdir/renamed.txt", VFS_O_RDONLY);
    assert(fren);
    char r_buf[16];
    assert(vfs_read(fren, r_buf, 10) == 10);
    assert(!memcmp(r_buf, "inside_dir", 10));
    vfs_close(fren);

    /* Unlink file inside directory */
    assert(vfs_unlink("/mnt/testdir/renamed.txt") == VFS_SUCCESS);
    assert(vfs_lookup("/mnt/testdir/renamed.txt") == NULL);

    /* Unlink empty directory now succeeds */
    assert(vfs_unlink("/mnt/testdir") == VFS_SUCCESS);
    assert(vfs_lookup("/mnt/testdir") == NULL);

    /* Cross-directory rename */
    assert(vfs_mkdir("/mnt/dir1", 0755) == VFS_SUCCESS);
    assert(vfs_mkdir("/mnt/dir2", 0755) == VFS_SUCCESS);
    file_t *fmove = vfs_open("/mnt/dir1/item.txt", VFS_O_WRONLY | VFS_O_CREAT);
    assert(fmove);
    assert(vfs_write(fmove, "moveme", 6) == 6);
    vfs_close(fmove);

    /* Edge case: trailing slash on file must fail with -ENOTDIR (-8) */
    assert(vfs_rename("/mnt/dir1/item.txt/", "/mnt/dir2/item.txt") == -8);
    assert(vfs_rename("/mnt/dir1/item.txt", "/mnt/dir2/item.txt/") == -8);

    /* Edge case: rename onto self succeeds immediately as a no-op */
    assert(vfs_rename("/mnt/dir1/item.txt", "/mnt/dir1/item.txt") == VFS_SUCCESS);

    /* Edge case: moving directory into subdirectory of itself must fail with -EINVAL */
    assert(vfs_rename("/mnt/dir1", "/mnt/dir1/sub") == -VFS_EINVAL);

    assert(vfs_rename("/mnt/dir1/item.txt", "/mnt/dir2/item.txt") == VFS_SUCCESS);
    assert(vfs_lookup("/mnt/dir1/item.txt") == NULL);
    file_t *fmoved = vfs_open("/mnt/dir2/item.txt", VFS_O_RDONLY);
    assert(fmoved);
    assert(vfs_read(fmoved, r_buf, 6) == 6);
    assert(!memcmp(r_buf, "moveme", 6));
    vfs_close(fmoved);

    /* Edge case: rename directory onto existing non-empty directory must fail with -ENOTEMPTY */
    assert(vfs_rename("/mnt/dir1", "/mnt/dir2") == -VFS_ENOTEMPTY);

    assert(vfs_unlink("/mnt/dir2/item.txt") == VFS_SUCCESS);
    assert(vfs_unlink("/mnt/dir1") == VFS_SUCCESS);
    assert(vfs_unlink("/mnt/dir2") == VFS_SUCCESS);

    /* 12. Clean shutdown synchronization (s_state: 0 active -> 1 clean) */
    assert(u16(disk + 1024 + 58) == 0); /* EXT2_VALID_FS cleared on RW mount */
    ext2_sync_all();
    assert(u16(disk + 1024 + 58) == 1); /* Restored to clean on sync */
    assert(u16(durable_disk + 1024 + 58) == 1);

    /* Verify that an unclean filesystem (s_state = 2 or s_state = 0) rejects writable mount */
    put16(disk + 1024 + 58, 2);
    memcpy(durable_disk, pending_disk, disk_size);
    reset();
    assert(!ext2_mount_rw(&dev, "/mnt"));
    put16(disk + 1024 + 58, 0);
    memcpy(durable_disk, pending_disk, disk_size);
    reset();
    assert(!ext2_mount_rw(&dev, "/mnt"));

    /* Verify rejection of BTREE_DIR (0x04) in ro_compat */
    put16(disk + 1024 + 58, 1);
    put32(disk + 1024 + 100, 4);
    memcpy(durable_disk, pending_disk, disk_size);
    reset();
    assert(!ext2_mount_rw(&dev, "/mnt"));
    put32(disk + 1024 + 100, 0);
    memcpy(durable_disk, pending_disk, disk_size);

    /* Restore clean state for subsequent tests */
    reset();
    put16(disk + 1024 + 58, 1);
    memcpy(durable_disk, pending_disk, disk_size);
    assert(ext2_mount_rw(&dev, "/mnt"));
    vfs_node_t *mnt_node_cl = vfs_lookup("/mnt");
    assert(mnt_node_cl);
    ext2_inode_t *mnt_in_cl = mnt_node_cl->fs_private;
    fs = mnt_in_cl->fs;

    /* 13. Injected flush failure -> tainted state (-EIO / -5) */
    file_t *fflush = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(fflush);
    flush_failure = true;
    assert(vfs_write(fflush, "data", 4) < 0);
    flush_failure = false;
    assert(fs->tainted);
    assert(!fs->read_only); /* Tainted is distinct from read-only! */
    /* Subsequent writes and truncations return -EIO (-5) */
    assert(vfs_write(fflush, "data", 4) == -5);
    assert(vfs_truncate(fflush->node, 0) == -5);
    vfs_close(fflush);
    int tainted_err = 0;
    assert(vfs_open_ext("/mnt/after_flush.txt", VFS_O_WRONLY | VFS_O_CREAT, &tainted_err) == NULL);
    assert(tainted_err == -VFS_EIO);

    /* 14. Crash simulation: unflushed writes in pending_disk are rolled back by simulate_crash */
    reset();
    put16(disk + 1024 + 58, 1);
    memcpy(durable_disk, pending_disk, disk_size);
    assert(ext2_mount_rw(&dev, "/mnt"));
    file_t *fcrash = vfs_open("/mnt/indir.bin", VFS_O_WRONLY);
    assert(fcrash);
    flush_failure = true; /* Prevents durable advance */
    assert(vfs_write(fcrash, "crashdata", 9) < 0);
    flush_failure = false;
    vfs_close(fcrash);
    simulate_crash(); /* Crash discards pending writes */
    reset();
    put16(disk + 1024 + 58, 1);
    memcpy(durable_disk, pending_disk, disk_size);
    assert(ext2_mount_rw(&dev, "/mnt"));
    vfs_node_t *mnt_node2 = vfs_lookup("/mnt");
    assert(mnt_node2);
    ext2_inode_t *mnt_in2 = mnt_node2->fs_private;
    fs = mnt_in2->fs;

    /* 15. Injected sector-write failure -> tainted state */
    file_t *fwr_fail = vfs_open("/mnt/taint.txt", VFS_O_WRONLY | VFS_O_CREAT);
    assert(fwr_fail);
    write_failure = true;
    assert(vfs_write(fwr_fail, "fail", 4) < 0);
    write_failure = false;
    assert(fs->tainted);
    assert(vfs_write(fwr_fail, "fail", 4) == -5);
    vfs_close(fwr_fail);
    assert(vfs_open("/mnt/taint2.txt", VFS_O_WRONLY | VFS_O_CREAT) == NULL);

    assert(writes > 0);
    assert(flushes > 0);

    reset();
    while (live) free(allocations[--live]);
    free(pending_disk);
    free(durable_disk);
    printf("PASS ext2: block=%u sector=%u, malformed metadata, shutdown/mount barriers, I/O/OOM ownership, indirect bounds\n", bs, ss);
}
