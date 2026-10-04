/* Reuse the independent fixture byte oracle and syscall/allocation adapters.
 * The Phase-7 main is compiled but not run by this transaction-engine gate. */
#define main phase7_main
#include "jbd2_write_host.c"
#undef main
static ext4_engine_t *observed;
static bool transaction_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    if (observed) {
        spin_debug_assert_unheld();
        uint64_t lo=lba*dev->sector_size,hi=lo+dev->sector_size;
        for (unsigned i=0;i<observed->images;i++) {
            e4_image_t *image=&observed->image[i];uint64_t start=(uint64_t)image->block*observed->fs->bs;
            if (image->role!=E4_DATA && lo<start+observed->fs->bs && hi>start &&
                jbd2_writer_state(observed->writer)!=JBD2_WRITE_CHECKPOINTING) {
                /* A larger sector may also contain ordered data. Its RMW must
                 * preserve every overlapping metadata-home byte pre-commit. */
                uint64_t from=lo>start ? lo : start,to=hi<start+observed->fs->bs ? hi : start+observed->fs->bs;
                ext4_fault_disk_t *disk=dev->priv;
                assert(!memcmp((const uint8_t *)bytes+from-lo,disk->volatile_bytes+from,(size_t)(to-from)));
            }
        }
    }
    return ext4_fault_write(dev,lba,bytes);
}
static int checked_commit(ext4_engine_t *e) {
    observed=e;int r=ext4_engine_commit(e);observed=NULL;return r;
}
#define ext4_engine_commit checked_commit
static ext4_engine_t *transaction(block_dev_t *dev,fixture_t *f) {
    ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal(dev,true,&e));
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){2,1,1}));
    assert(!ext4_engine_transaction_metadata(e,f->inodeblock,f->inode));
    assert(!ext4_engine_transaction_metadata(e,f->targets[0],f->escaped));
    assert(!ext4_engine_transaction_data(e,f->targets[1],f->ordered));
    assert(!ext4_engine_transaction_revoke(e,f->targets[2]));return e;
}
int main(int argc,char **argv) {
    assert(argc==7);size_t n,cn;uint8_t *initial=load(argv[1],&n),*config=load(argv[2],&cn);assert(cn==20);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=transaction_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);fixture_t f=fixture(&dev,initial,config);ext4_engine_t *e=NULL,*second=NULL;
    alloc_calls=reads=0;
    assert(ext4_engine_open_journal(&dev,false,&e)==-VFS_EROFS && !e && !d.events);
    e=transaction(&dev,&f);size_t open_allocs=alloc_calls,open_reads=reads;
    assert(ext4_engine_open_journal(&dev,true,&second)==-VFS_EEXIST && !second);
    assert(ext4_engine_grow(e,f.fileino,0,1)==-VFS_EOPNOTSUPP);
    assert(ext4_engine_trim(e,f.fileino,0)==-VFS_EOPNOTSUPP);
    uint32_t ino;assert(ext4_engine_reserve_inode(e,&ino)==-VFS_EOPNOTSUPP);
    assert(ext4_engine_release_inode(e,f.fileino)==-VFS_EOPNOTSUPP);
    assert(ext4_engine_finish(e)==-VFS_EOPNOTSUPP);
    assert(!e4_write_bytes(e,1024,f.inode,1024) && !d.events);
    assert(!ext4_engine_transaction_metadata(e,f.inodeblock,f.inode));
    assert(ext4_engine_transaction_metadata(e,f.targets[2],f.escaped)==-VFS_EINVAL);
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !e->tainted && !d.events);
    ext4_engine_abort(e);
    assert(ext4_engine_transaction_begin(e,(jbd2_credits_t){64,1,0})==-VFS_EFBIG);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0}));
    assert(!ext4_engine_transaction_metadata(e,f.inodeblock,f.inode));
    assert(ext4_engine_transaction_metadata(e,f.targets[0],f.escaped)==-VFS_EFBIG);
    assert(ext4_engine_commit(e)==-VFS_EFBIG && !e->tainted && !d.events);
    ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0}));
    assert(!ext4_engine_transaction_metadata(e,f.inodeblock,f.inode));
    fail_alloc=0;assert(ext4_engine_commit(e)==-VFS_ENOMEM && !e->tainted && !d.events);
    assert(ext4_engine_commit(e)==-VFS_ENOMEM);fail_alloc=-1;ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0}));
    assert(!ext4_engine_transaction_metadata(e,f.map[0],f.inode));
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !e->tainted && !d.events);
    ext4_engine_abort(e);ext4_engine_close(e);assert(!live);
    reset(&d,initial);assert(!ext4_engine_open_journal(&dev,true,&e));
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,1,1}));
    assert(!ext4_engine_transaction_metadata(e,f.inodeblock,f.inode));
    assert(ext4_engine_transaction_data(e,f.inodeblock,f.ordered)==-VFS_EINVAL);
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !d.events);ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,1,1}));
    assert(ext4_engine_transaction_data(e,f.map[1],f.ordered)==-VFS_EINVAL);
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !d.events);ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,1}));
    assert(ext4_engine_transaction_revoke(e,f.map[1])==-VFS_EINVAL);
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !d.events);ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){0,0,1}));
    assert(!ext4_engine_transaction_revoke(e,f.targets[2]));
    assert(!ext4_engine_transaction_revoke(e,f.targets[2]));
    assert(ext4_engine_transaction_revoke(e,f.targets[1])==-VFS_EFBIG);
    assert(ext4_engine_commit(e)==-VFS_EFBIG && !e->tainted && !d.events);
    ext4_engine_abort(e);ext4_engine_close(e);assert(!live);
    reset(&d,initial);assert(!ext4_engine_open_journal(&dev,true,&e));
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){64,0,64}));
    unsigned homes[128],at=0;
    for (unsigned block=f.source.filesystem_blocks-256;at<128;block++) {
        assert(block<f.source.filesystem_blocks);
        if (e4_data_range(e->fs,block,1) && j_target(e->writer->io,block)) homes[at++]=block;
    }
    for (unsigned i=0;i<64;i++) {
        assert(!ext4_engine_transaction_metadata(e,homes[i],f.escaped));
        assert(!ext4_engine_transaction_revoke(e,homes[i+64]));
    }
    assert(!ext4_engine_commit(e));
    for (unsigned i=0;i<64;i++) {
        assert(!memcmp(d.stable+(uint64_t)homes[i]*f.bs,f.escaped,f.bs));
        assert(!memcmp(d.stable+(uint64_t)homes[i+64]*f.bs,initial+(uint64_t)homes[i+64]*f.bs,f.bs));
    }
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,63,0}));
    assert(!ext4_engine_transaction_metadata(e,f.inodeblock,f.inode));
    for (unsigned i=0;i<63;i++) assert(!ext4_engine_transaction_data(e,homes[i],f.ordered));
    size_t previous=d.events;
    assert(ext4_engine_transaction_data(e,homes[63],f.ordered)==-VFS_EFBIG);
    assert(ext4_engine_commit(e)==-VFS_EFBIG && d.events==previous && !e->tainted);
    ext4_engine_abort(e);ext4_engine_close(e);assert(!live);
    reset(&d,initial);
    uint8_t *small=d.volatile_bytes+(uint64_t)f.map[0]*f.bs;
    j_put(small+20,f.source.journal_blocks-3);j_put(small+88,f.source.journal_blocks-3);
    jw_seal(small,1024,252,UINT32_MAX);
    assert(!ext4_engine_open_journal(&dev,true,&e));
    assert(ext4_engine_transaction_begin(e,(jbd2_credits_t){2,0,0})==-VFS_ENOSPC && !e->ready && !d.events);
    ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal(&dev,true,&e));
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0}));
    assert(ext4_engine_commit(e)==-VFS_EINVAL && !e->tainted && !d.events);
    ext4_engine_abort(e);
    assert(!ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0}));
    e4_journal_guard_t *guard=e->journal_guard;
    memcpy(e->fs->scratch,initial+(uint64_t)guard->inode_block*f.bs,f.bs);
    e->fs->scratch[guard->inode_offset]^=1;
    assert(!ext4_engine_transaction_metadata(e,guard->inode_block,e->fs->scratch));
    assert(ext4_engine_commit(e)==-VFS_EIO && !e->tainted && !d.events);
    ext4_engine_abort(e);ext4_engine_close(e);assert(!live);
    reset(&d,initial);e=transaction(&dev,&f);alloc_calls=0;reads=0;
    e4_cache_committed(e->fs,f.inodeblock,initial+(uint64_t)f.inodeblock*f.bs);
    d.trace=true;assert(!ext4_engine_commit(e));d.trace=false;
    size_t events=d.events,commit_allocs=alloc_calls,commit_reads=reads;
    assert(!e->ready && !e->tainted);compare(&f,d.stable,initial,true);
    uint8_t cache[256];assert(e4_cached_bytes(e->fs,(uint64_t)f.inodeblock*f.bs+f.offset,cache,256,0));
    assert(!memcmp(cache,f.inode+f.offset,256));
    save(argv[4],d.stable,n);save(argv[5],d.stable,n);
    ext4_engine_close(e);assert(!live);
    for (size_t cut=0;cut<commit_allocs;cut++) {
        reset(&d,initial);e=transaction(&dev,&f);fail_alloc=(long)cut;
        assert(ext4_engine_commit(e)==-VFS_ENOMEM && !e->tainted && !d.events);
        fail_alloc=-1;ext4_engine_abort(e);ext4_engine_close(e);assert(!live);
    }
    for (size_t cut=0;cut<open_allocs;cut++) {
        reset(&d,initial);fail_alloc=(long)cut;
        assert(ext4_engine_open_journal(&dev,true,&e)==-VFS_ENOMEM && !e && !d.events && !live && !e4_engine_busy);
    }
    for (size_t cut=0;cut<open_reads;cut++) {
        reset(&d,initial);fail_read=(long)cut;
        assert(ext4_engine_open_journal(&dev,true,&e)==-VFS_EIO && !e && !d.events && !live && !e4_engine_busy);
    }
    for (size_t cut=0;cut<commit_reads;cut++) {
        reset(&d,initial);e=transaction(&dev,&f);fail_read=(long)cut;
        assert(ext4_engine_commit(e)==-VFS_EIO && e->tainted && !e->fs->metadata_enabled);
        fail_read=-1;ext4_engine_close(e);ext4_fault_restart(&d);recover(&dev,&f,initial,false);
    }
    for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
        reset(&d,initial);d.persistence=mode;e=transaction(&dev,&f);d.cut=(long)cut;d.after=after;
        assert(ext4_engine_commit(e)==-VFS_EIO && e->tainted && !e->fs->metadata_enabled);
        size_t stopped=d.events;
        assert(ext4_engine_commit(e)==-VFS_EIO && ext4_engine_finish(e)==-VFS_EIO);
        assert(ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0})==-VFS_EIO && d.events==stopped);
        ext4_engine_abort(e);assert(e->tainted);ext4_engine_close(e);
        ext4_fault_restart(&d);recover(&dev,&f,initial,false);
    }
    reset(&d,initial);e=transaction(&dev,&f);assert(!ext4_engine_commit(e));ext4_engine_close(e);
    ext4_fault_restart(&d);recover(&dev,&f,initial,true);save(argv[6],d.stable,n);
    printf("PASS EXT4 8.1 bs=%u sector=%u: %zu events, %zu atomic crash cuts; admission/ownership/credits/sticky staging/OOM/read/write/flush/taint/cache/recovery\n",f.bs,ss,events,events*8);
    free(f.map);free(d.stable);free(d.volatile_bytes);free(initial);free(config);assert(!live);return 0;
}
