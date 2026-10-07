/* Transaction foundation and semantic planners. Phase 8.5 reuses these under
 * ranked mounted exclusion; production journal mounts remain disabled. */
static int e4_open_journal_owned(block_dev_t *dev,bool admitted,ext4_engine_t **out,bool reserved) {
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !dev || !dev->read_sector ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    if (!admitted || !dev->write_sector || !dev->flush) return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || (e4_engine_busy && !reserved)) { spin_unlock_irqrestore(&e4_lock,flags);return -VFS_EEXIST; }
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
    if (!reserved) { flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags); }
    return r;
}
int ext4_engine_open_journal(block_dev_t *dev,bool admitted,ext4_engine_t **out) {
    return e4_open_journal_owned(dev,admitted,out,false);
}
int ext4_engine_transaction_begin(ext4_engine_t *e,jbd2_credits_t credits) {
    spin_debug_assert_unheld();if (!e || !e->writer) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->file_enabled) return -VFS_EOPNOTSUPP;
    if (e->ready) return -VFS_EAGAIN;
    if (credits.metadata>E4_IMAGES || credits.ordered_data>E4_IMAGES ||
        credits.metadata+credits.ordered_data>E4_IMAGES) return -VFS_EFBIG;
    int r=jbd2_writer_begin(e->writer,credits);if (r) return r;
    e->credits=credits;e->images=e->revoke_count=0;e->error=0;e->ready=true;return 0;
}
static int e4_transaction_stage(ext4_engine_t *e,uint32_t block,const void *bytes,unsigned role) {
    spin_debug_assert_unheld();if (!e || !e->writer) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->file_enabled) return -VFS_EOPNOTSUPP;
    if (!e->ready) return -VFS_EINVAL;
    if (e->error) return e->error;
    if (!bytes || block>=e->fs->blocks) return e->error=-VFS_EINVAL;
    if (role==E4_DATA) {
        if (!e4_data_range(e->fs,block,1)) return e->error=-VFS_EINVAL;
        for (unsigned i=0;i<e->fs->journal_ranges;i++)
            if (block>=e->fs->journal_reserved[i].lo && block-e->fs->journal_reserved[i].lo<e->fs->journal_reserved[i].len)
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
    image->ordered=role==E4_DATA;
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
    if (e->file_enabled) return -VFS_EOPNOTSUPP;
    if (!e->ready) return -VFS_EINVAL;
    if (e->error) return e->error;
    if (block>=e->fs->blocks || !e4_data_range(e->fs,block,1)) return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->fs->journal_ranges;i++)
        if (block>=e->fs->journal_reserved[i].lo && block-e->fs->journal_reserved[i].lo<e->fs->journal_reserved[i].len)
            return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) return e->error=-VFS_EINVAL;
    for (unsigned i=0;i<e->revoke_count;i++) if (e->revokes[i]==block) return 0;
    if (e->revoke_count==e->credits.revokes) return e->error=-VFS_EFBIG;
    e->revokes[e->revoke_count++]=block;return 0;
}
static int e4_transaction_commit(ext4_engine_t *e) {
    e4_engine_owner(e);if (e->tainted) return -VFS_EIO;
    if (!e->ready) return -VFS_EINVAL;
    int r=e->error;if (r) return r;
    unsigned metadata=0;
    for (unsigned i=0;i<e->images;i++) if (!e->image[i].ordered) metadata++;
    if (!metadata && !e->revoke_count) return e->error=-VFS_EINVAL;
    /* Snapshot and identity/credit checks ALL precede the first device write. */
    for (unsigned i=0;!r && i<e->images;i++) {
        e4_image_t *image=&e->image[i];
        r=image->ordered ? jbd2_writer_data(e->writer,image->block,image->bytes) :
            jbd2_writer_metadata(e->writer,image->block,image->bytes);
    }
    for (unsigned i=0;!r && i<e->revoke_count;i++) r=jbd2_writer_revoke(e->writer,e->revokes[i]);
    if (r) { e->error=r;return r; } /* Abort required, no publication or taint. */
    e4_cache_drop(e->fs,false);
    r=jbd2_writer_commit(e->writer);
#ifdef FORTRESS_EXT4_COMMIT_PAUSE_TEST
    if (!r && e4_pause_callback && e4_pause_mount==e->fs) {
        void (*pause)(void)=e4_pause_callback;
        e4_pause_callback=NULL;e4_pause_mount=NULL;
        pause();
    }
#endif
    if (!r) r=jbd2_writer_checkpoint(e->writer);
    e->ready=false;
    if (r) { e->tainted=true;e4_cache_drop(e->fs,true);return r; }
    for (uint32_t g=0;g<e->fs->groups;g++) {
        uint32_t offset=g*32,block=e->fs->first+1+offset/e->fs->bs;
        for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block)
            memcpy(e->fs->gd[g].raw,e->image[i].bytes+offset%e->fs->bs,32);
    }
    for (unsigned i=0;i<e->images;i++)
        if (!e->image[i].ordered) e4_cache_committed(e->fs,e->image[i].block,e->image[i].bytes);
    return 0;
}
/* Phase 8.2 needs consistent allocation ownership, unlike recovery bootstrap.
 * No recovery or RECOVER activation is implicit in file admission. */
static int e4_open_journal_files_owned(block_dev_t *dev,bool admitted,bool orphans,ext4_engine_t **out,bool reserved,bool lifecycle) {
    int r=e4_open_journal_owned(dev,admitted,out,reserved);if (r) return r;
    ext4_engine_t *e=*out;ext4_mount_t *fs=e->fs;uint8_t sb[1024];
    if (!e4_bytes(fs,1024,sb,1024) || e4_u32(sb+1020)!=e4_crc(UINT32_MAX,sb,1020)) { r=-VFS_EIO;goto fail; }
    if ((e4_u32(sb+96)!=0x46 && !(lifecycle && e4_u32(sb+96)==0x42 && e4_u16(sb+58)==1 && !e4_u32(sb+232))) ||
        (!orphans && e4_u32(sb+232)) || e4_u32(sb+336) || e4_u32(sb+340) || e4_u32(sb+344)) {
        r=-VFS_EOPNOTSUPP;goto fail;
    }
    uint64_t blocks=0,inodes=0;
    for (uint32_t g=0;g<fs->groups;g++) {
        blocks+=e4_u16(fs->gd[g].raw+12);inodes+=e4_u16(fs->gd[g].raw+14);
        for (unsigned kind=0;kind<2;kind++) if (!(e4_u16(fs->gd[g].raw+18)&(kind ? 1u : 2u))) {
            if (!e4_bitmap(fs,g,kind!=0)) { r=-VFS_EIO;goto fail; }
            uint32_t start=fs->first+g*fs->bpg,count=kind ? fs->ipg : fs->blocks-start;
            if (!kind && count>fs->bpg) count=fs->bpg;
            unsigned free=0;for (uint32_t bit=0;bit<count;bit++) free+=!e4_bit(fs->scratch,bit);
            if (free!=e4_u16(fs->gd[g].raw+(kind ? 14 : 12))) { r=-VFS_EIO;goto fail; }
            if (!kind) for (unsigned i=0;i<fs->ranges;i++) {
                e4_range_t range=fs->reserved[i];
                if (range.lo>=start && range.lo<start+count) for (unsigned k=0;k<range.len;k++)
                    if (!e4_bit(fs->scratch,range.lo-start+k)) { r=-VFS_EIO;goto fail; }
            }
        }
    }
    if (blocks!=e4_u32(sb+12) || inodes!=e4_u32(sb+16)) { r=-VFS_EIO;goto fail; }
    if (!e4_bitmap(fs,(fs->journal_ino-1)/fs->ipg,true) ||
        !e4_bit(fs->scratch,(fs->journal_ino-1)%fs->ipg)) { r=-VFS_EIO;goto fail; }
    for (unsigned i=0;i<fs->journal_ranges;i++) for (uint32_t k=0;k<fs->journal_reserved[i].len;k++) {
        uint32_t block=fs->journal_reserved[i].lo+k,g=(block-fs->first)/fs->bpg;
        /* Cache the validated bitmap for a whole journal range's group. */
        if (!k || g!=(block-1-fs->first)/fs->bpg) {
            if (!e4_bitmap(fs,g,false)) { r=-VFS_EIO;goto fail; }
        }
        if (!e4_bit(fs->scratch,(block-fs->first)%fs->bpg)) { r=-VFS_EIO;goto fail; }
    }
    fs->journal_bootstrap=false;e->file_enabled=true;e->dirty=e4_u16(sb+58)==0;
    ((e4_journal_guard_t *)e->journal_guard)->lifecycle=lifecycle;
    return 0;
fail:
    if (reserved) e4_engine_dispose(e);else ext4_engine_close(e);
    *out=NULL;return r;
}
static int e4_open_journal_files(block_dev_t *dev,bool admitted,bool orphans,ext4_engine_t **out) {
    return e4_open_journal_files_owned(dev,admitted,orphans,out,false,false);
}
int ext4_engine_open_journal_files(block_dev_t *dev,bool admitted,ext4_engine_t **out) {
    return e4_open_journal_files(dev,admitted,false,out);
}
/* Complete a sealed semantic operation as one bounded journal transaction.
 * Every released block is revoked; no staged home image may overlap a revoke.
 * The tail barrier retires this transaction before a subsequent allocator. */
static int e4_semantic_commit(ext4_engine_t *e) {
    int r=e4_plan_end(e);if (r) return r;
    e4_seal(e);if (e->error) return e->error;
    jbd2_credits_t credits={0};e->revoke_count=0;
    for (unsigned i=0;i<e->images;i++) {
        if (e->image[i].ordered) credits.ordered_data++;else credits.metadata++;
    }
    /* Reclaimed data and old external tree nodes are revoked with their
     * reference changes. Checkpoint before subsequent allocation may reuse. */
    for (unsigned i=0;i<e->frees;i++) for (uint32_t k=0;k<e->released[i].len;k++) {
        if (e->revoke_count==64) return -VFS_EFBIG;
        e->revokes[e->revoke_count++]=e->released[i].lo+k;
    }
    credits.revokes=e->revoke_count;
    r=jbd2_writer_begin(e->writer,credits);if (r) return r;
    e->credits=credits;r=e4_transaction_commit(e);
    if (!r) e->dirty=true;
    return r;
}
/* Semantic workbenches expose no VFS nodes or map handles. Drop temporary
 * reader maps between operations instead of charging them to mount lifetime. */
static void e4_semantic_invalidate(ext4_engine_t *e) {
    e4_invalidate(e->fs);
    while (e->fs->maps) { e4_map_t *map=e->fs->maps;e->fs->maps=map->next;kfree(map); }
    e->fs->map_entries=0;
}
int64_t ext4_engine_file_write(ext4_engine_t *e,uint32_t ino,uint64_t *offset,bool append,const void *data,size_t len) {
    e4_engine_owner(e);if (!e || !e->file_enabled || !offset || (!data && len)) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->orphan_recovery_required) return -VFS_EAGAIN;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    if (ino==e->fs->journal_ino) return -VFS_EOPNOTSUPP;
    /* Linked truncate intents must be completed before any growth. Unlinked
     * open files may still be written through their validated handle. */
    for (unsigned i=0;i<e->orphan_count;i++) if (e->orphans[i]==ino && e->fs->orphan_access!=ino)
        return -VFS_EAGAIN;
    if (len>32768) len=32768;
    uint64_t next=*offset;e->file_planning=true;
    int r=e4_file_plan(e,ino,&next,append,data,len);e->file_planning=false;
    if (r || !len) goto done;
    r=e4_semantic_commit(e);
    if (!r) *offset=next;
done:
    if (r || !len) ext4_engine_abort(e);
    e4_semantic_invalidate(e);return r ? r : (int64_t)len;
}
