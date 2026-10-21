/* Private pre-intent abort only; no production admission or allocator hook. */
static int idle_release(ext4_mount_t *fs){
    uint64_t lock=spin_lock_irqsave(&e4_lock);ext4_engine_t *e=fs->engine;pg_view v;
    int r=-VFS_EINVAL;
    if(e->tainted){r=-VFS_EIO;goto done;}
    if(e->ready||jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE||
       life_validate(e,&v)||e->orphan_count||e->orphan_recovery_required)goto done;
    /* Reject every name, including untracked children, before releasing escrow. */
    e4_inode_t dir;vfs_dirent_t entry;
    if(e4_inode(fs,ids[3],&dir))goto done;
    for(unsigned i=0;;i++){
        uint32_t found;int n=e4_scan(&dir,NULL,i,&entry,&found);if(n<0)goto done;if(!n)break;
        if(strcmp(entry.name,".")&&strcmp(entry.name,".."))goto done;
    }
    if(v.phase==PG_IDLE){r=0;goto done;}
    if(v.phase!=PG_RESERVED||v.charged[0]||v.charged[1]||v.sequence==UINT64_MAX)goto done;
    r=e4_namespace_begin(e);if(r)goto done;
    e4_p16(ledger+22,PG_IDLE);p64(ledger+24,v.sequence+1);
    memset(ledger+40,0,16);memcpy(ledger+200,ledger+168,32);
    memset(ledger+296,0,48);e4_p16(ledger+348,0);
    p64(ledger+368,u64(ledger+360));memset(ledger+392,0,40);
    e4_p16(table+22,0);memset(table+32,0,16);p64(table+48,v.sequence+1);
    memset(table+256,0,4096);seal_records();
    pg_view next;
    r=pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&next);
    if(!r)r=life_stage(e,1,ledger,sizeof ledger);
    if(!r)r=life_stage(e,2,table,sizeof table);
    e->file_planning=false;
    if(!r)r=pg_life_core_commit(e);
    if(r)ext4_engine_abort(e);
    e4_semantic_invalidate(e);
done:
    spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static void idle_check(ext4_mount_t *fs){
    life_check(fs);assert(e4_u16(ledger+22)==PG_IDLE&&!e4_u16(table+22));
    ext4_fault_disk_t *d=fs->dev->priv;size_t events=d->events;
    uint64_t seq=u64(ledger+24);assert(!idle_release(fs));
    assert(d->events==events&&u64(ledger+24)==seq);
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);size_t size;uint8_t *initial=load(argv[1],&size);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    uint8_t *reserved=malloc(size);assert(reserved);unsigned cuts=0;
    for(unsigned retired=0;retired<2;retired++){
        life_active=false;ext4_mount_t *fs;reset(&d,initial);
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));life_setup(fs);
        if(retired){
            assert(!life_create(fs));file_t *f=vfs_open(life_path,VFS_O_RDWR);assert(f);
            memset(payload,'I',fs->bs);assert(vfs_write(f,payload,fs->bs)==(int64_t)fs->bs);
            size_t events=d.events;assert(idle_release(fs)&&d.events==events);
            assert(!vfs_unlink(life_path));events=d.events;assert(idle_release(fs)&&d.events==events);
            assert(!vfs_close(f));life_refunded(fs);
        }
        /* Sequence exhaustion refuses before storage; restore the fixture using
         * unwrapped fixed-record writes, never the package planner. */
        uint64_t saved_seq=u64(ledger+24);life_active=false;
        p64(ledger+24,UINT64_MAX);p64(table+48,UINT64_MAX);seal_records();
        write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);life_active=true;
        size_t noio=d.events;assert(idle_release(fs)&&d.events==noio);
        life_active=false;p64(ledger+24,saved_seq);p64(table+48,saved_seq);seal_records();
        write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);life_active=true;
        memcpy(reserved,d.stable,size);uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof sb));
        uint32_t fb=e4_u32(sb+12),fi=e4_u32(sb+16);uint64_t seq=u64(ledger+24);
        d.events=0;assert(!idle_release(fs));size_t total=d.events;idle_check(fs);
        assert(u64(ledger+24)==seq+1&&e4_bytes(fs,1024,sb,sizeof sb)&&e4_u32(sb+12)==fb&&e4_u32(sb+16)==fi);
        assert(!ext4_freeze_and_sync(fs));save(argv[3],retired?"retired-idle":"empty-idle",d.stable,size);
        for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<total;cut++){
            reset(&d,reserved);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
            d.events=d.writes=d.flushes=0;d.persistence=persistence;d.after=after!=0;d.cut=(long)cut;
            assert(idle_release(fs)==-VFS_EIO);
            for(unsigned pass=0;pass<2;pass++){
                life_boot(&dev,&fs);assert(e4_bytes(fs,1024,sb,sizeof sb)&&e4_u32(sb+12)==fb&&e4_u32(sb+16)==fi);
                unsigned phase=e4_u16(ledger+22);assert(phase==PG_IDLE||phase==PG_RESERVED);
                assert(u64(ledger+24)==seq+(phase==PG_IDLE));
                assert(!idle_release(fs));idle_check(fs);assert(!ext4_freeze_and_sync(fs));
            }cuts++;
        }
        printf("IDLE retired=%u events=%zu cuts=%zu\n",retired,total,total*8);
    }
    life_active=false;teardown();free(initial);free(reserved);free(d.stable);free(d.volatile_bytes);
    printf("PASS idle cuts=%u two-restarts-per-cut\n",cuts);return 0;
}
