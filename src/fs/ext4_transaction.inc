/* Phase 8.1: exclusive, unlocked block-image transaction foundation.
 * No VFS journal mount or semantic operation is admitted by this workbench. */
int ext4_engine_open_journal(block_dev_t *dev,bool admitted,ext4_engine_t **out) {
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !dev || !dev->read_sector ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    if (!admitted || !dev->write_sector || !dev->flush) return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || e4_engine_busy) { spin_unlock_irqrestore(&e4_lock,flags);return -VFS_EEXIST; }
    e4_engine_busy=true;spin_unlock_irqrestore(&e4_lock,flags);
    ext4_engine_t *e=kcalloc(1,sizeof(*e));
    ext4_mount_t *fs=kcalloc(1,sizeof(*fs));
    e4_journal_guard_t *guard=kcalloc(1,sizeof(*guard));
    uint32_t *map=NULL;int r=-VFS_ENOMEM;
    if (!e || !fs || !guard) goto fail;
    e->fs=fs;e->journal_guard=guard;fs->dev=dev;
    fs->bs=dev->sector_size;fs->blocks=(uint32_t)dev->sector_count;fs->journal_bootstrap=true;
    jbd2_source_t source;
    r=e4_journal_source(fs,guard,&map,&source);
    if (!r) r=jbd2_writer_open(dev,&source,true,&e->writer);
    kfree(map);map=NULL;
    if (r) goto fail;
    /* Bootstrap snapshots must remain immutable: guard owns the map/tree
     * identity through close. Ordinary mutators cannot enter plan_begin. */
    fs->metadata_enabled=true;*out=e;return 0;
fail:
    kfree(map);if (e) jbd2_writer_close(e->writer);
    kfree(guard);e4_discard(fs);kfree(e);
    flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags);
    return r;
}
int ext4_engine_transaction_begin(ext4_engine_t *e,jbd2_credits_t credits) {
    spin_debug_assert_unheld();if (!e || !e->writer) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->ready) return -VFS_EAGAIN;
    if (credits.metadata>E4_IMAGES || credits.ordered_data>E4_IMAGES ||
        credits.metadata+credits.ordered_data>E4_IMAGES) return -VFS_EFBIG;
    int r=jbd2_writer_begin(e->writer,credits);if (r) return r;
    e->credits=credits;e->images=e->revoke_count=0;e->error=0;e->ready=true;return 0;
}
static int e4_transaction_stage(ext4_engine_t *e,uint32_t block,const void *bytes,unsigned role) {
    spin_debug_assert_unheld();if (!e || !e->writer) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (!e->ready) return -VFS_EINVAL;
    if (e->error) return e->error;
    if (!bytes || block>=e->fs->blocks) return e->error=-VFS_EINVAL;
    if (role==E4_DATA) {
        if (!e4_data_range(e->fs,block,1)) return e->error=-VFS_EINVAL;
        for (unsigned i=0;i<e->fs->physical_count;i++)
            if (block>=e->fs->physical[i].lo && block-e->fs->physical[i].lo<e->fs->physical[i].len)
                return e->error=-VFS_EINVAL;
    }
    for (unsigned i=0;i<e->revoke_count;i++) if (e->revokes[i]==block) return e->error=-VFS_EINVAL;
    unsigned count=0;
    for (unsigned i=0;i<e->images;i++) {
        e4_image_t *image=&e->image[i];
        if (image->block==block) {
            if (image->role!=role) return e->error=-VFS_EINVAL;
            memcpy(image->bytes,bytes,e->fs->bs);return 0;
        }
        if (image->role==role) count++;
    }
    if (e->images==E4_IMAGES || count==(role==E4_DATA ? e->credits.ordered_data : e->credits.metadata))
        return e->error=-VFS_EFBIG;
    e4_image_t *image=&e->image[e->images++];image->block=block;image->role=role;
    memcpy(image->bytes,bytes,e->fs->bs);return 0;
}
int ext4_engine_transaction_metadata(ext4_engine_t *e,uint32_t block,const void *bytes) {
    return e4_transaction_stage(e,block,bytes,E4_REFERENCE);
}
int ext4_engine_transaction_data(ext4_engine_t *e,uint32_t block,const void *bytes) {
    return e4_transaction_stage(e,block,bytes,E4_DATA);
}
int ext4_engine_transaction_revoke(ext4_engine_t *e,uint32_t block) {
    spin_debug_assert_unheld();if (!e || !e->writer) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (!e->ready) return -VFS_EINVAL;
    if (e->error) return e->error;
    if (block>=e->fs->blocks || !e4_data_range(e->fs,block,1)) return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->fs->physical_count;i++)
        if (block>=e->fs->physical[i].lo && block-e->fs->physical[i].lo<e->fs->physical[i].len)
            return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->revoke_count;i++) if (e->revokes[i]==block) return 0;
    if (e->revoke_count==e->credits.revokes) return e->error=-VFS_EFBIG;
    e->revokes[e->revoke_count++]=block;return 0;
}
static int e4_transaction_commit(ext4_engine_t *e) {
    spin_debug_assert_unheld();if (e->tainted) return -VFS_EIO;
    if (!e->ready) return -VFS_EINVAL;
    int r=e->error;if (r) return r;
    unsigned metadata=0;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].role!=E4_DATA) metadata++;
    if (!metadata && !e->revoke_count) return e->error=-VFS_EINVAL;
    /* Snapshot and identity/credit checks ALL precede the first device write. */
    for (unsigned i=0;!r && i<e->images;i++) {
        e4_image_t *image=&e->image[i];
        r=image->role==E4_DATA ? jbd2_writer_data(e->writer,image->block,image->bytes) :
            jbd2_writer_metadata(e->writer,image->block,image->bytes);
    }
    for (unsigned i=0;!r && i<e->revoke_count;i++) r=jbd2_writer_revoke(e->writer,e->revokes[i]);
    if (r) { e->error=r;return r; } /* Abort required, no publication or taint. */
    e4_cache_drop(e->fs,false);
    r=jbd2_writer_commit(e->writer);
    if (!r) r=jbd2_writer_checkpoint(e->writer);
    e->ready=false;
    if (r) { e->tainted=true;e4_cache_drop(e->fs,true);return r; }
    for (uint32_t g=0;g<e->fs->groups;g++) {
        uint32_t offset=g*32,block=e->fs->first+1+offset/e->fs->bs;
        for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block)
            memcpy(e->fs->gd[g].raw,e->image[i].bytes+offset%e->fs->bs,32);
    }
    for (unsigned i=0;i<e->images;i++)
        if (e->image[i].role!=E4_DATA) e4_cache_committed(e->fs,e->image[i].block,e->image[i].bytes);
    return 0;
}
