/* Private overlay hooks only. Shared production sources are never rewritten. */
static bool life_active;
static uint32_t life_target;
static const char *life_path="/mnt/pkg/transaction.v1/unit";
static unsigned life_commits;
/* Constructor recovery has an explicit engine before fs->engine publication.
 * Never fake mount/trust flags or depend on the public mounted-root adapter. */
static int life_stage(ext4_engine_t *e,unsigned index,uint8_t *bytes,size_t n){
    ext4_mount_t *fs=e->fs;e4_inode_t in;
    if(e4_inode(fs,ids[index],&in)||in.size!=n)return -1;
    fs->rw_ino=0;if(e4_mapping(&in))return -1;
    uint32_t blocks[8];unsigned count=(unsigned)((n+fs->bs-1)/fs->bs);if(count>8)return -1;
    for(unsigned i=0;i<count;i++){
        const e4_extent_t *x=e4_find(&in,i);if(!x||x->unwritten)return -1;
        blocks[i]=x->physical+i-x->logical;
    }
    for(unsigned i=0;i<count;i++){
        size_t take=n-(size_t)i*fs->bs;if(take>fs->bs)take=fs->bs;
        uint8_t *p=e4_stage(e,blocks[i],E4_REFERENCE);if(!p)return -1;
        memcpy(p,bytes+(size_t)i*fs->bs,take);
    }return 0;
}
static int life_read(ext4_mount_t *fs,pg_view *v){
    if(!eligible(fs)||record_io(fs,0,marker,sizeof marker,false)||
       record_io(fs,1,ledger,sizeof ledger,false)||record_io(fs,2,table,sizeof table,false))return -1;
    return pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,v);
}
static int life_validate(ext4_engine_t *e,pg_view *out){
    ext4_mount_t *fs=e->fs;pg_view v;if(life_read(fs,&v)||e4_orphan_validate(e))return -1;
    e4_inode_t parent;uint32_t named=0;
    if(e4_inode(fs,ids[3],&parent)||e4_scan(&parent,"unit",0,NULL,&named)<0)return -1;
    unsigned count=e4_u16(table+22);
    uint64_t qb=0,qi=0;
    if(!count){if(named)return -1;life_target=0;}
    else {
        const uint8_t *p=table+256;uint32_t ino=e4_u32(p+4),gen=e4_u32(p+8);
        unsigned cls=e4_u16(p+2),flags=e4_u32(p+12);
        if(count!=1||e4_u16(p)!=1||ino<11||ino>fs->inodes)return -1;
        uint32_t g=(ino-1)/fs->ipg,bit=(ino-1)%fs->ipg;
        if(!e4_bitmap(fs,g,true))return -1;
        bool allocated=e4_bit(fs->scratch,bit);uint8_t *raw=raw_inode(fs,ino);
        if(!raw||e4_u32(raw+100)!=gen)return -1;
        if(cls==3){if(allocated||named)return -1;}
        else {
            if(cls!=1||!allocated)return -1;
            bool orphan=e4_orphan_index(e,ino)>=0;
            if((flags==2)!=orphan || (flags==1 ? named!=ino : named!=0) ||
               e4_u16(raw+26)!=(flags==1?1:0) || e4_u16(raw)!=(0x8000|0600) ||
               e4_u16(raw+2)||e4_u16(raw+24)||e4_u16(raw+120)||e4_u16(raw+122))return -1;
            uint32_t sectors=e4_u32(raw+28);
            uint64_t size=e4_u32(raw+4)|(uint64_t)e4_u32(raw+108)<<32;
            if(size!=u64(p+32)||sectors%(fs->bs/512))return -1;
            e4_inode_t in;fs->orphan_access=orphan?ino:0;
            int r=e4_inode(fs,ino,&in);fs->orphan_access=0;if(r)return -1;
            fs->rw_ino=0;if(e4_mapping(&in))return -1;
            for(unsigned i=0;i<fs->physical_count;i++)qb+=fs->physical[i].len;
            if(qb!=sectors/(fs->bs/512)||qb!=u64(p+40)||u64(p+48)!=1)return -1;
            qi=1;
        }
        life_target=ino;
    }
    if(v.charged[0]!=qb||v.charged[1]!=qi||v.total[0]!=8||v.total[1]!=1)return -1;
    *out=v;return 0;
}
static int pg_life_reconstruct(struct ext4_engine *e){
    if(!life_active)return 0;
    pg_view v;return life_validate(e,&v);
}
/* This hook runs after the real planner but before its first write. Observe
 * the staged inode/bitmap; record blocks join the original semantic commit. */
static int e4_semantic_commit(struct ext4_engine *e){
    if(!life_active)return pg_life_core_commit(e);
    ext4_mount_t *fs=e->fs;pg_view before;
    if(life_read(fs,&before)||!life_target||before.sequence==UINT64_MAX)return -VFS_EINVAL;
    uint32_t ino=life_target,g=(ino-1)/fs->ipg,bit=(ino-1)%fs->ipg;
    uint8_t *map=e4_stage_bitmap(e,g,true),*raw=e4_inode_image(e,ino);
    if(!map||!raw)return e->error;
    bool allocated=e4_bit(map,bit);
    uint64_t qb=allocated?e4_u32(raw+28)/(fs->bs/512):0,qi=allocated?1:0;
    if(qb>8||e4_u32(raw+28)%(fs->bs/512))return -VFS_EFBIG;
    uint8_t sb[1024];if(!e4_bytes(fs,1024,sb,sizeof sb))return -VFS_EIO;
    uint8_t *next=e4_super(e);if(!next)return e->error;
    /* This bounded profile permits no unrelated parent growth/allocation. */
    if((int64_t)e4_u32(next+12)-(int64_t)e4_u32(sb+12)!=(int64_t)before.charged[0]-(int64_t)qb ||
       (int64_t)e4_u32(next+16)-(int64_t)e4_u32(sb+16)!=(int64_t)before.charged[1]-(int64_t)qi)return -VFS_EINVAL;
    uint32_t generation=e4_u32(raw+100);unsigned links=e4_u16(raw+26);
    if(allocated&&(links>1||e4_u16(raw)!=(0x8000|0600)))return -VFS_EINVAL;
    p64(ledger+24,before.sequence+1);p64(table+48,before.sequence+1);
    p64(ledger+312,8-qb);p64(ledger+320,1-qi);p64(ledger+328,qb);p64(ledger+336,qi);
    e4_p16(table+22,1);memset(table+256,0,64);e4_p16(table+256,1);e4_p16(table+258,allocated?1:3);
    e4_p32(table+260,ino);e4_p32(table+264,generation);memcpy(table+272,ledger+40,16);
    if(allocated){e4_p32(table+268,links?1:2);p64(table+288,e4_u32(raw+4)|(uint64_t)e4_u32(raw+108)<<32);p64(table+296,qb);p64(table+304,1);}
    seal_records();
    if(life_stage(e,1,ledger,sizeof ledger)||life_stage(e,2,table,sizeof table))return -VFS_EIO;
    pg_view verify;if(pg_validate_set(marker,sizeof marker,ledger,sizeof ledger,table,sizeof table,&binding,&verify))return -VFS_EINVAL;
    int r=pg_life_core_commit(e);if(!r)life_commits++;return r;
}
static void life_check(ext4_mount_t *fs){
    uint64_t lock=spin_lock_irqsave(&e4_lock);pg_view v;ext4_fault_disk_t *d=fs->dev->priv;size_t events=d->events;
    assert(!life_validate(fs->engine,&v)&&events==d->events);spin_unlock_irqrestore(&e4_lock,lock);
}
static void life_setup(ext4_mount_t *fs){
    life_active=false;seed(fs);assert(!vfs_unlink(paths[4]));
    p64(ledger+320,1);p64(ledger+336,0);e4_p16(table+22,0);memset(table+256,0,64);seal_records();
    write_file(1,ledger,sizeof ledger);write_file(2,table,sizeof table);assert(!ext4_sync(fs));
    life_active=true;life_target=0;life_check(fs);
}
static int life_create(ext4_mount_t *fs){
    uint64_t lock=spin_lock_irqsave(&e4_lock);life_target=0;
    for(uint32_t g=0;g<fs->groups&&!life_target;g++){
        assert(e4_bitmap(fs,g,true));for(uint32_t bit=0;bit<fs->ipg;bit++)
            if(g*fs->ipg+bit+1>=11&&!e4_bit(fs->scratch,bit)){life_target=g*fs->ipg+bit+1;break;}
    }
    assert(life_target);uint32_t ino=0;vfs_create_attrs_t attrs={.mode=0600,.uid=0,.gid=0};
    int r=ext4_engine_namespace_create_attrs(fs->engine,ids[3],"unit",false,&attrs,&ino);
    if(!r)assert(ino==life_target);
    spin_unlock_irqrestore(&e4_lock,lock);return r;
}
static void life_boot(block_dev_t *dev,ext4_mount_t **fs){
    teardown();ext4_fault_restart(dev->priv);vfs_init();const char *stage=NULL;
    int r=e4_mount_journal_admitted(dev,"/mnt",true,fs,&stage,false);
    if(r)fprintf(stderr,"LIFE BOOT FAIL stage=%s error=%d\n",stage,r);
    assert(!r);life_check(*fs);
}
static void life_refunded(ext4_mount_t *fs){
    life_check(fs);assert(e4_u16(table+258)==3&&u64(ledger+312)==8&&u64(ledger+320)==1);
    uint64_t lock=spin_lock_irqsave(&e4_lock);pg_view v;size_t events=((ext4_fault_disk_t *)fs->dev->priv)->events;
    assert(!ext4_engine_orphan_recover(fs->engine));assert(events==((ext4_fault_disk_t *)fs->dev->priv)->events);
    assert(!life_validate(fs->engine,&v));spin_unlock_irqrestore(&e4_lock,lock);
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);size_t size;uint8_t *initial=load(argv[1],&size);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    ext4_mount_t *fs;reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));life_setup(fs);
    uint8_t *empty=malloc(size),*linked=malloc(size),*orphan=malloc(size);assert(empty&&linked&&orphan);memcpy(empty,d.stable,size);
    d.events=0;assert(!life_create(fs));size_t create_events=d.events;life_check(fs);
    file_t *held=vfs_open(life_path,VFS_O_RDWR);assert(held);memset(payload,'L',fs->bs);assert(vfs_write(held,payload,fs->bs)==(int64_t)fs->bs);
    memcpy(linked,d.stable,size);assert(vfs_file_get(held));uint64_t sequence=u64(ledger+24);
    d.events=0;assert(!vfs_unlink(life_path));size_t unlink_events=d.events;life_check(fs);
    assert(u64(ledger+328)==1&&u64(ledger+336)==1&&e4_u32(table+268)==2&&u64(ledger+24)==sequence+1);
    held->offset=0;memset(payload,0,fs->bs);assert(vfs_read(held,payload,fs->bs)==(int64_t)fs->bs);
    for(unsigned i=0;i<fs->bs;i++)assert(payload[i]=='L');
    size_t events=d.events;assert(!vfs_close(held)&&d.events==events); /* duplicate ref still pins */
    memcpy(orphan,d.stable,size);d.events=0;assert(!vfs_close(held));size_t close_events=d.events;life_refunded(fs);
    assert(!ext4_freeze_and_sync(fs));save(argv[3],"refunded",d.stable,size);
    reset(&d,orphan);d.events=0;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));size_t recovery_events=d.events;life_refunded(fs);
    unsigned cuts=0;
    for(unsigned op=0;op<4;op++){
        size_t total=op==0?create_events:op==1?unlink_events:op==2?close_events:recovery_events;
        for(unsigned persistence=0;persistence<4;persistence++)for(unsigned after=0;after<2;after++)for(size_t cut=0;cut<total;cut++){
            reset(&d,op==0?empty:op==3?orphan:linked);held=NULL;
            if(op!=3){assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));if(op==1||op==2){held=vfs_open(life_path,VFS_O_RDONLY);assert(held);if(op==2)assert(!vfs_unlink(life_path));}}
            d.events=d.writes=d.flushes=0;d.persistence=persistence;d.after=after!=0;d.cut=(long)cut;
            int r;
            if(op==0)r=life_create(fs);
            else if(op==1)r=vfs_unlink(life_path);
            else if(op==2){assert(!vfs_close(held));held=NULL;r=fs->engine->tainted?-VFS_EIO:0;}
            else {fs=NULL;r=ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs);assert(!fs&&!e4_active);}
            if(r!=-VFS_EIO)fprintf(stderr,"LIFE CUT FAIL op=%u persistence=%u after=%u cut=%zu total=%zu error=%d\n",op,persistence,after,cut,total,r);
            assert(r==-VFS_EIO);if(held)assert(!vfs_close(held));
            for(unsigned pass=0;pass<2;pass++){
                life_boot(&dev,&fs);if(op>=2)life_refunded(fs);
                assert(!ext4_freeze_and_sync(fs));
            }cuts++;
        }
        printf("LIFE op=%u events=%zu cuts=%zu\n",op,total,total*8);
    }
    save(argv[3],"recovered",d.stable,size);life_active=false;teardown();free(initial);free(empty);free(linked);free(orphan);free(d.stable);free(d.volatile_bytes);
    printf("PASS lifecycle cuts=%u commits=%u two-restarts-per-cut\n",cuts,life_commits);return 0;
}
