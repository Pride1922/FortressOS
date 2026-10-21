/* Private serialized planner gate. Numeric recovery floor is a fixture input,
 * not an authenticated production R_acc bound or a task/session capability. */
static bool compete_active,compete_ordinary;
static uint64_t compete_floor[2];
static uint32_t compete_parent,compete_inode;
static void compete_free(ext4_mount_t *fs,uint64_t f[2]){
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof sb));
    f[0]=e4_u32(sb+12);f[1]=e4_u32(sb+16);
}
static bool compete_fits(ext4_mount_t *fs,const uint64_t f[2],const uint64_t g[2]){
    const uint64_t a0[2]={(EXT4_ACCOUNT_RESERVE_BYTES+fs->bs-1)/fs->bs,EXT4_ACCOUNT_RESERVE_INODES};
    for(unsigned i=0;i<2;i++){
        uint64_t floor=compete_floor[i]>a0[i]?compete_floor[i]:a0[i];
        if(f[i]<g[i]||f[i]-g[i]<floor)return false;
    }
    return true;
}
static int e4_semantic_commit(ext4_engine_t *e){
    if(!compete_active||!life_active)return pg_compete_lifecycle_commit(e);
    pg_view v;if(life_read(e->fs,&v))return -VFS_EINVAL;
    if(compete_ordinary){
        /* The complete real plan includes directory/extent-node allocations. */
        uint8_t *sb=e4_super(e);if(!sb)return e->error;
        uint64_t next[2]={e4_u32(sb+12),e4_u32(sb+16)};
        if(!compete_fits(e->fs,next,v.remaining))return -VFS_ENOSPC;
        return pg_life_core_commit(e);
    }
    if(v.phase!=PG_RESERVED)return -VFS_EINVAL;
    uint64_t f[2];compete_free(e->fs,f);
    /* Tagged F and G move together; lifecycle wrapper checks exact staged
     * free-counter delta against actual tagged inode/bitmap debit or refund. */
    if(!compete_fits(e->fs,f,v.remaining))return -VFS_ENOSPC;
    return pg_compete_lifecycle_commit(e);
}
static int compete_empty(ext4_mount_t *fs){
    e4_inode_t dir;vfs_dirent_t entry;uint32_t found;
    if(e4_inode(fs,ids[3],&dir))return -1;
    for(unsigned i=0;;i++){
        int r=e4_scan(&dir,NULL,i,&entry,&found);if(r<0)return r;if(!r)break;
        if(strcmp(entry.name,".")&&strcmp(entry.name,".."))return -1;
    }
    if(e4_scan(&dir,"..",0,NULL,&found)!=1)return -1;
    if(e4_inode(fs,found,&dir)||e4_scan(&dir,"pending.v1",0,NULL,&found)<0||found)return -1;
    return 0;
}
/* Expected sequence is a compare-and-admit fence, not writer identity. A real
 * task-retained nontransferable session still requires separate integration. */
static int compete_admit(ext4_mount_t *fs,uint64_t expected,uint8_t tx){
    uint64_t lock=spin_lock_irqsave(&e4_lock);ext4_engine_t *e=fs->engine;pg_view v;
    int r=-VFS_EINVAL;
    if(e->tainted){r=-VFS_EIO;goto done;}
    if(!tx||e->ready||jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE||
       life_validate(e,&v)||v.phase!=PG_IDLE||v.sequence!=expected||
       v.sequence==UINT64_MAX||e->orphan_count||e->orphan_recovery_required||compete_empty(fs))goto done;
    uint64_t f[2],grant[2]={8,1};compete_free(fs,f);
    if(!compete_fits(fs,f,grant)){r=-VFS_ENOSPC;goto done;}
    r=e4_namespace_begin(e);if(r)goto done;
    e4_p16(ledger+22,PG_RESERVED);p64(ledger+24,v.sequence+1);
    memset(ledger+40,tx,16);memset(table+32,tx,16);p64(table+48,v.sequence+1);
    memset(ledger+200,0,32);p64(ledger+368,0);e4_p16(ledger+348,1);
    p64(ledger+296,8);p64(ledger+304,1);p64(ledger+312,8);p64(ledger+320,1);
    memset(ledger+392,'N',32);seal_records();
    pg_view next;r=pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&next);
    if(!r)r=life_stage(e,1,ledger,sizeof ledger);
    if(!r)r=life_stage(e,2,table,sizeof table);
    e->file_planning=false;if(!r)r=pg_life_core_commit(e);
    if(r)ext4_engine_abort(e);
    e4_semantic_invalidate(e);
done:
    spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static int compete_create(ext4_mount_t *fs,const char *name,uint32_t *ino){
    uint64_t lock=spin_lock_irqsave(&e4_lock);compete_ordinary=true;
    vfs_create_attrs_t attrs={.mode=0600,.uid=0,.gid=0};
    int r=ext4_engine_namespace_create_attrs(fs->engine,compete_parent,name,false,&attrs,ino);
    compete_ordinary=false;spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static int64_t compete_write(ext4_mount_t *fs,uint64_t *off){
    uint64_t lock=spin_lock_irqsave(&e4_lock);compete_ordinary=true;memset(payload,'C',fs->bs);
    int64_t r=ext4_engine_file_write(fs->engine,compete_inode,off,false,payload,fs->bs);
    compete_ordinary=false;spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static void compete_defaults(ext4_mount_t *fs){
    compete_floor[0]=(EXT4_ACCOUNT_RESERVE_BYTES+fs->bs-1)/fs->bs;
    compete_floor[1]=EXT4_ACCOUNT_RESERVE_INODES;
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);size_t size;uint8_t *initial=load(argv[1],&size);
    unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));life_setup(fs);
    assert(!idle_release(fs));e4_inode_t dir;uint32_t parent;
    assert(!e4_inode(fs,ids[3],&dir)&&e4_scan(&dir,"..",0,NULL,&parent)==1);compete_parent=parent;
    compete_active=true;compete_defaults(fs);assert(!compete_create(fs,"competitor",&compete_inode));
    uint64_t f[2];compete_free(fs,f);uint64_t seq=u64(ledger+24);size_t events;
    uint32_t pending;assert(!compete_create(fs,"pending.v1",&pending));events=d.events;
    assert(compete_admit(fs,seq,'A')&&d.events==events);
    compete_ordinary=true;assert(!vfs_unlink("/mnt/pkg/pending.v1"));compete_ordinary=false;
    compete_free(fs,f);events=d.events;assert(compete_admit(fs,seq,0)&&d.events==events);
    /* Admission at exactly the floor succeeds; +1 in either dimension refuses. */
    for(unsigned dim=0;dim<2;dim++){
        compete_defaults(fs);compete_floor[dim]=f[dim]-(dim?1:8)+1;
        events=d.events;assert(compete_admit(fs,seq,'A')==-VFS_ENOSPC&&events==d.events);idle_check(fs);
    }
    compete_floor[0]=f[0]-8;compete_floor[1]=f[1]-1;
    assert(!compete_admit(fs,seq,'E'));assert(!idle_release(fs));idle_check(fs);
    uint64_t unchanged[2];compete_free(fs,unchanged);assert(!memcmp(f,unchanged,sizeof f));seq=u64(ledger+24);
    compete_floor[0]=f[0]-9;compete_floor[1]=f[1]-2;
    assert(!compete_admit(fs,seq,'A'));life_check(fs);
    events=d.events;assert(compete_admit(fs,seq,'B')&&events==d.events);
    uint64_t off=0;assert(compete_write(fs,&off)==(int64_t)fs->bs);
    events=d.events;uint64_t oldoff=off;assert(compete_write(fs,&off)==-VFS_ENOSPC&&off==oldoff&&d.events==events);
    uint32_t other=0;assert(!compete_create(fs,"other",&other));uint32_t untouched=UINT32_MAX;
    events=d.events;assert(compete_create(fs,"denied",&untouched)==-VFS_ENOSPC&&untouched==UINT32_MAX&&d.events==events);
    /* Both ordinary dimensions are now exactly at the private floor. */
    assert(!life_create(fs));file_t *held=vfs_open(life_path,VFS_O_RDWR);assert(held);
    memset(payload,'P',fs->bs);assert(vfs_write(held,payload,fs->bs)==(int64_t)fs->bs);
    assert(!vfs_unlink(life_path));events=d.events;
    assert(compete_admit(fs,u64(ledger+24),'B')&&d.events==events);
    assert(!vfs_close(held));life_refunded(fs);assert(!idle_release(fs));idle_check(fs);
    assert(compete_write(fs,&off)==(int64_t)fs->bs);assert(!compete_create(fs,"released",&untouched));
    /* An ordinary winner can exhaust admission headroom; admission re-reads F. */
    compete_free(fs,f);compete_floor[0]=f[0]-8;compete_floor[1]=f[1]-1;
    assert(compete_write(fs,&off)==(int64_t)fs->bs);events=d.events;
    assert(compete_admit(fs,u64(ledger+24),'B')==-VFS_ENOSPC&&d.events==events);
    compete_defaults(fs);seq=u64(ledger+24);uint64_t stale=seq-1;
    events=d.events;assert(compete_admit(fs,stale,'B')&&d.events==events);
    /* Save empty IDLE before admission for exhaustive real commit cuts. */
    uint8_t *idle=malloc(size);assert(idle);memcpy(idle,d.stable,size);
    compete_free(fs,f);d.events=0;assert(!compete_admit(fs,seq,'B'));size_t total=d.events;life_check(fs);
    compete_free(fs,unchanged);assert(!memcmp(f,unchanged,sizeof f));
    assert(u64(ledger+24)==seq+1&&ledger[40]=='B');
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"admitted",d.stable,size);
    unsigned cuts=0;
    for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<total;cut++){
        reset(&d,idle);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
        d.events=d.writes=d.flushes=0;d.persistence=persistence;d.after=after!=0;d.cut=(long)cut;
        assert(compete_admit(fs,seq,'B')==-VFS_EIO);
        for(unsigned pass=0;pass<2;pass++){
            life_boot(&dev,&fs);unsigned phase=e4_u16(ledger+22);assert(phase==PG_IDLE||phase==PG_RESERVED);
            compete_free(fs,unchanged);assert(!memcmp(f,unchanged,sizeof f));
            assert(u64(ledger+24)==seq+(phase==PG_RESERVED));
            if(phase==PG_RESERVED){assert(ledger[40]=='B');events=d.events;assert(compete_admit(fs,seq,'C')&&d.events==events);}
            assert(!ext4_freeze_and_sync(fs));
        }cuts++;
    }
    /* Finish a recovered IDLE, reject old request, then prove second tagged use. */
    life_boot(&dev,&fs);if(e4_u16(ledger+22)==PG_IDLE)assert(!compete_admit(fs,seq,'B'));
    assert(!life_create(fs));assert(!vfs_unlink(life_path));life_refunded(fs);
    assert(!idle_release(fs));idle_check(fs);assert(!ext4_freeze_and_sync(fs));save(argv[3],"idle",d.stable,size);
    compete_active=life_active=false;teardown();free(initial);free(idle);free(d.stable);free(d.volatile_bytes);
    printf("PASS compete cuts=%u events=%zu two-restarts-per-cut boundary-dimensions=2\n",cuts,total);return 0;
}
