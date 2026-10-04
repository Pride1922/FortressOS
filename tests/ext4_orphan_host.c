#define main phase7_main
#include "jbd2_write_host.c"
#undef main
static ext4_engine_t *observed;
static bool orphan_sector(block_dev_t *dev,uint64_t lba,const void *bytes) {
    spin_debug_assert_unheld();
    if (observed) for (unsigned i=0;i<observed->images;i++) {
        e4_image_t *image=&observed->image[i];if (image->ordered) continue;
        uint64_t lo=lba*dev->sector_size,hi=lo+dev->sector_size;
        uint64_t start=(uint64_t)image->block*observed->fs->bs,end=start+observed->fs->bs;
        if (lo<end && hi>start && jbd2_writer_state(observed->writer)!=JBD2_WRITE_CHECKPOINTING) {
            ext4_fault_disk_t *d=dev->priv;uint64_t a=lo>start ? lo : start,b=hi<end ? hi : end;
            assert(!memcmp((const uint8_t *)bytes+a-lo,d->volatile_bytes+a,(size_t)(b-a)));
        }
    }
    return ext4_fault_write(dev,lba,bytes);
}
static uint32_t lookup(ext4_engine_t *e,const char *name) {
    e4_inode_t root_inode;uint32_t ino;assert(!e4_inode(e->fs,2,&root_inode));
    assert(e4_scan(&root_inode,name,0,NULL,&ino)>=0);return ino;
}
/* Independent ownership census: build the allocated set from inode trees and
 * compare every bitmap bit/free count, rather than invoking admission's audit. */
static void ownership(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;uint8_t *owned=calloc((fs->blocks+7)/8,1);assert(owned);
    for (unsigned i=0;i<fs->ranges;i++) for (uint32_t k=0;k<fs->reserved[i].len;k++)
        e4_set_bit(owned,fs->reserved[i].lo+k,true);
    for (unsigned i=0;i<fs->journal_ranges;i++) for (uint32_t k=0;k<fs->journal_reserved[i].len;k++)
        e4_set_bit(owned,fs->journal_reserved[i].lo+k,true);
    for (uint32_t g=0;g<fs->groups;g++) {
        if (e4_u16(fs->gd[g].raw+18)&1) continue;
        assert(e4_bitmap(fs,g,true));uint8_t bitmap[4096];memcpy(bitmap,fs->scratch,fs->bs);unsigned free_inodes=0,dirs=0;
        for (uint32_t bit=0;bit<fs->ipg;bit++) {
            if (!e4_bit(bitmap,bit)) { free_inodes++;continue; }
            uint32_t ino=g*fs->ipg+bit+1;if (ino<11 && ino!=2) continue;
            e4_inode_t in;fs->orphan_access=e4_orphan_index(e,ino)>=0 ? ino : 0;
            assert(!e4_inode(fs,ino,&in));fs->orphan_access=0;dirs+=in.mode==0x4000;
            fs->visits=fs->pending_count=fs->physical_count=0;
            assert(!e4_tree(&in,in.extent,e4_u16(in.extent+6),true,0,1ULL<<32));
            uint64_t blocks=0;
            for (unsigned i=0;i<fs->physical_count;i++) for (uint32_t k=0;k<fs->physical[i].len;k++) {
                uint32_t block=fs->physical[i].lo+k;assert(!e4_bit(owned,block));e4_set_bit(owned,block,true);blocks++;
            }
            uint8_t raw[256];assert(e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+bit*256,raw,sizeof(raw)));
            assert(blocks*(fs->bs/512)==e4_u32(raw+28));
        }
        assert(free_inodes==e4_u16(fs->gd[g].raw+14) && dirs==e4_u16(fs->gd[g].raw+16));
    }
    uint64_t total=0;
    for (uint32_t g=0;g<fs->groups;g++) {
        uint32_t start=fs->first+g*fs->bpg,count=fs->blocks-start;if (count>fs->bpg) count=fs->bpg;
        bool lazy=(e4_u16(fs->gd[g].raw+18)&2)!=0;unsigned free_blocks=0;
        if (!lazy) assert(e4_bitmap(fs,g,false));
        for (uint32_t bit=0;bit<count;bit++) {
            bool expected=e4_bit(owned,start+bit);
            if (!lazy) assert(expected==e4_bit(fs->scratch,bit));
            else if (expected) {
                bool fixed=false;for (unsigned i=0;i<fs->ranges;i++) if (start+bit>=fs->reserved[i].lo &&
                    start+bit-fs->reserved[i].lo<fs->reserved[i].len) fixed=true;
                assert(fixed);
            }
            free_blocks+=!expected;
        }
        assert(free_blocks==e4_u16(fs->gd[g].raw+12));total+=free_blocks;
    }
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,sizeof(sb)) && total==e4_u32(sb+12));free(owned);
}
static int operation(ext4_engine_t *e,const uint8_t *cfg,unsigned profile) {
    uint32_t bs=e4_u32(cfg),victim=e4_u32(cfg+4),large=e4_u32(cfg+8),deep=e4_u32(cfg+20);
    observed=e;int r;
    if (profile==0) r=ext4_engine_file_truncate(e,victim,bs+17);
    else if (profile==1) r=ext4_engine_file_truncate(e,large,bs+17);
    else if (profile==2) r=ext4_engine_namespace_remove(e,2,"large.bin",false);
    else if (profile==3) r=ext4_engine_namespace_remove(e,2,"tree.bin",false);
    else if (profile==4) r=ext4_engine_namespace_remove(e,2,"empty",true);
    else if (profile==6) r=ext4_engine_file_truncate(e,deep,(uint64_t)e4_u32(cfg+24)*bs);
    else {
        uint64_t first,second;r=ext4_engine_handle_open(e,victim,&first);if (r) goto done;
        r=ext4_engine_handle_open(e,victim,&second);if (r) goto done;
        r=ext4_engine_handle_dup(e,first);if (r) goto done;
        r=ext4_engine_namespace_remove(e,2,"victim.bin",false);
        if (!r) {
            uint8_t data[4096];memset(data,'N',bs);
            int64_t n=ext4_engine_handle_write(e,first,true,data,bs);r=n<0 ? (int)n : 0;
        }
        if (!r) r=ext4_engine_handle_truncate(e,second,bs+17);
        if (!r) r=ext4_engine_handle_close(e,first);
        if (!r) r=ext4_engine_handle_close(e,second);
        if (!r) r=ext4_engine_handle_close(e,first);
    }
done:
    observed=NULL;return r;
}
static bool recover_check(block_dev_t *dev,const uint8_t *cfg,unsigned profile,bool success,const char *pending) {
    ext4_fault_disk_t *d=dev->priv;jbd2_plan_t *plan=NULL;jbd2_report_t report;
    assert(!ext4_journal_analyze(dev,&plan,&report));bool logged=report.transactions!=0;
    if (logged && pending) save(pending,d->stable,d->bytes);
    assert(!jbd2_replay(plan,true));jbd2_release(plan);
    ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_orphans(dev,true,&e));
    bool intent=e->orphan_count!=0;
    assert(!ext4_engine_orphan_recover(e) && !e->orphan_count);
    ownership(e);
    size_t events=d->events;assert(!ext4_engine_orphan_recover(e) && events==d->events);
    uint32_t bs=e4_u32(cfg);
    if (profile==0 || profile==1 || profile==6) {
        uint32_t ino=e4_u32(cfg+(profile==0 ? 4 : profile==1 ? 8 : 20));
        uint64_t before=profile==0 ? 3u*bs : profile==1 ? 65u*bs : ((uint64_t)e4_u32(cfg+24)+1)*bs;
        uint64_t after=profile==6 ? (uint64_t)e4_u32(cfg+24)*bs : bs+17;
        e4_inode_t in;assert(!e4_inode(e->fs,ino,&in));assert(in.size==before || in.size==after);
        if (success || intent) assert(in.size==after);
        assert(!e4_mapping(&in));
        for (unsigned i=0;i<in.map->count;i++) assert((uint64_t)in.map->entries[i].logical+in.map->entries[i].len<=
            (in.size+bs-1)/bs);
        if (profile==0) {
            uint64_t handle;assert(!ext4_engine_handle_open(e,ino,&handle));uint8_t data[12288];
            int64_t n=ext4_engine_handle_read(e,handle,data,sizeof(data));assert(n==(int64_t)in.size);
            for (int64_t i=0;i<n;i++) assert(data[i]=='O');
            assert(!ext4_engine_handle_close(e,handle));
            if (in.size==after) {
                e4_inode_t check;assert(!e4_inode(e->fs,ino,&check) && !e4_mapping(&check));
                const e4_extent_t *x=e4_find(&check,1);assert(x);
                const uint8_t *tail=d->stable+(uint64_t)(x->physical+1-x->logical)*bs;
                for (unsigned k=17;k<bs;k++) assert(!tail[k]);
            }
        }
    } else {
        const char *name=profile==2 ? "large.bin" : profile==3 ? "tree.bin" : profile==4 ? "empty" : "victim.bin";
        uint32_t ino=lookup(e,name);assert(ino==0 || ino==e4_u32(cfg+(profile==2 ? 8 : profile==3 ? 12 : profile==4 ? 16 : 4)));
        if (success || intent) assert(!ino);
    }
    /* Fresh 8.4 admission repeats independent whole allocation-set validation
     * after cleanup. Standard 8.3 admission also accepts the cleared chain. */
    ext4_engine_close(e);assert(!ext4_engine_open_journal_orphans(dev,true,&e));ext4_engine_close(e);
    assert(!ext4_engine_open_journal_namespace(dev,true,&e));ext4_engine_close(e);assert(!live);
    return logged;
}
static void lifecycle(block_dev_t *dev,const uint8_t *initial,const uint8_t *cfg,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_engine_t *e=NULL;
    assert(!ext4_engine_open_journal_orphans(dev,true,&e));uint32_t victim=e4_u32(cfg+4),bs=e4_u32(cfg);
    e4_inode_t before;assert(!e4_inode(e->fs,victim,&before) && !e4_mapping(&before));uint32_t old_block=e4_find(&before,0)->physical;
    uint64_t a,b,old;assert(!ext4_engine_handle_open(e,victim,&a));old=a;
    assert(!ext4_engine_handle_dup(e,a) && !ext4_engine_handle_open(e,victim,&b));
    uint8_t bytes[4096];assert(ext4_engine_handle_read(e,a,bytes,17)==17);
    assert(e->handles[e4_handle_slot(e,a)].offset==17 && !e->handles[e4_handle_slot(e,b)].offset);
    assert(!ext4_engine_namespace_remove(e,2,"victim.bin",false) && !lookup(e,"victim.bin"));
    assert(e->orphan_count==1);size_t events=d->events;
    ownership(e);
    assert(!ext4_engine_orphan_recover(e) && e->orphan_count==1 && d->events==events);
    assert(ext4_engine_handle_read(e,a,bytes,19)==19);
    memset(bytes,'N',bs);assert(ext4_engine_handle_write(e,b,true,bytes,bs)==bs);
    uint64_t offset=e->handles[e4_handle_slot(e,b)].offset;
    assert(!ext4_engine_handle_truncate(e,b,bs+17) && e->handles[e4_handle_slot(e,b)].offset==offset && e->orphan_count==1);
    assert(ext4_engine_handle_write(e,b,true,bytes,bs)==bs);
    ownership(e);
    uint32_t created=0;assert(!ext4_engine_namespace_create(e,2,"replacement",false,&created) && created!=victim);
    assert(!ext4_engine_handle_close(e,a) && !ext4_engine_handle_close(e,b) && e->orphan_count==1);
    assert(!ext4_engine_handle_close(e,a) && !e->orphan_count);
    events=d->events;assert(ext4_engine_handle_close(e,old)==-VFS_EINVAL && d->events==events);
    assert(!ext4_engine_namespace_create(e,2,"reuse",false,&created) && created==victim);
    assert(!ext4_engine_handle_open(e,created,&a) && a!=old);
    assert(ext4_engine_handle_read(e,old,bytes,1)==-VFS_EINVAL);
    assert(ext4_engine_handle_write(e,a,false,bytes,bs)==bs);
    e4_inode_t reused;assert(!e4_inode(e->fs,created,&reused) && !e4_mapping(&reused));
    assert(e4_find(&reused,0)->physical==old_block);
    assert(!ext4_engine_handle_open(e,created,&b));assert(ext4_engine_handle_read(e,b,bytes,bs)==bs);
    for (unsigned i=0;i<bs;i++) assert(bytes[i]=='N');
    assert(!ext4_engine_handle_close(e,b));
    assert(!ext4_engine_handle_close(e,a));
    char path[1024];snprintf(path,sizeof(path),"%s-lifetime-reuse.img",prefix);save(path,d->stable,d->bytes);
    /* Non-head chain removal while the newer unlinked inode stays pinned. */
    uint32_t ino;assert(!ext4_engine_namespace_create(e,2,"older",false,&ino));
    assert(!ext4_engine_handle_open(e,ino,&a));assert(!ext4_engine_handle_open(e,created,&b));
    assert(!ext4_engine_namespace_remove(e,2,"older",false));assert(!ext4_engine_namespace_remove(e,2,"reuse",false));
    assert(e->orphan_count==2 && !ext4_engine_handle_close(e,a) && e->orphan_count==1);
    assert(!ext4_engine_handle_close(e,b) && !e->orphan_count);
    uint64_t handles[16];for (unsigned i=0;i<16;i++) assert(!ext4_engine_handle_open(e,e4_u32(cfg+8),&handles[i]));
    a=UINT64_MAX;events=d->events;assert(ext4_engine_handle_open(e,e4_u32(cfg+8),&a)==-VFS_EFBIG && a==UINT64_MAX && d->events==events);
    for (unsigned i=0;i<16;i++) assert(!ext4_engine_handle_close(e,handles[i]));
    events=d->events;assert(ext4_engine_file_truncate(e,e4_u32(cfg+8),66u*bs)==-VFS_EOPNOTSUPP && d->events==events);
    ext4_engine_close(e);assert(!ext4_engine_open_journal_orphans(dev,true,&e));ext4_engine_close(e);assert(!live);
}
/* Create a valid intent without running cleanup, for recovery and resource
 * tests. Uses the exact production planner/seal/commit boundary; no disk edits. */
static void intent_only(ext4_engine_t *e,const char *name,uint32_t ino) {
    assert(!e4_orphan_begin(e));e4_inode_t dir,in;uint8_t *raw;
    assert(!e4_namespace_parent(e,2,&dir) && !e4_load_plan(e,ino,&in,&raw));
    assert(!e4_orphan_add(e,ino,raw));e4_p16(raw+26,0);e4_inode_sum(e->fs,ino,raw);
    assert(!e4_dir_edit(e,&dir,name,0,0) && !e4_namespace_end(e,0));e4_orphan_added(e,ino);
}
static void recovery_faults(block_dev_t *dev,const uint8_t *initial,const uint8_t *cfg,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_engine_t *e=NULL;
    assert(!ext4_engine_open_journal_orphans(dev,true,&e));intent_only(e,"large.bin",e4_u32(cfg+8));ownership(e);
    uint8_t *orphan=malloc(d->bytes);assert(orphan);memcpy(orphan,d->stable,d->bytes);ext4_engine_close(e);
    assert(!ext4_engine_open_journal_orphans(dev,true,&e));size_t prior=d->events;uint32_t out=UINT32_MAX;
    assert(ext4_engine_namespace_create(e,2,"blocked",false,&out)==-VFS_EAGAIN && out==UINT32_MAX && d->events==prior);
    d->events=0;reads=alloc_calls=0;assert(!ext4_engine_orphan_recover(e));size_t events=d->events;
    ext4_engine_close(e);
    printf("orphan recovery campaign events=%zu cuts=%zu\n",events,events*8);fflush(stdout);
    for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
        reset(d,orphan);assert(!ext4_engine_open_journal_orphans(dev,true,&e));
        d->cut=(long)cut;d->persistence=mode;d->after=after;
        assert(ext4_engine_orphan_recover(e)==-VFS_EIO && e->tainted);
        ext4_engine_close(e);ext4_fault_restart(d);recover_check(dev,cfg,2,true,NULL);
    }
    /* Traditional chain failures reject before publication and leak nothing. */
    for (unsigned bad=0;bad<13;bad++) {
        reset(d,orphan);assert(!ext4_engine_open_journal_orphans(dev,true,&e));
        uint32_t ino=e4_u32(cfg+8),g=(ino-1)/e->fs->ipg,index=(ino-1)%e->fs->ipg;
        uint8_t *raw=d->volatile_bytes+(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+index*256;
        uint8_t *sb=d->volatile_bytes+1024;
        if (bad==0) e4_p32(sb+232,2);
        if (bad==1) e4_p32(sb+232,e->fs->inodes+1);
        if (bad==2) e4_p32(sb+232,e->fs->journal_ino);
        if (bad==3) e4_p32(raw+20,ino);
        if (bad==4) e4_p32(raw+20,2);
        if (bad==5) e4_p16(raw+26,2);
        if (bad==6 || bad==8 || bad==9) {
            uint32_t victim=e4_u32(cfg+4),vg=(victim-1)/e->fs->ipg,vi=(victim-1)%e->fs->ipg;
            uint8_t *source=d->volatile_bytes+(uint64_t)e4_u32(e->fs->gd[vg].raw+8)*e->fs->bs+vi*256;
            if (bad==6) { memcpy(raw+40,source+40,60);memcpy(raw+28,source+28,4); }
            else {
                e4_p16(source+26,0);
                if (bad==9) { e4_p32(source+20,ino);e4_p32(sb+232,victim); }
                e4_inode_sum(e->fs,victim,source);
            }
        }
        e4_inode_sum(e->fs,ino,raw);if (bad==7) raw[124]^=1;
        if (bad==10 || bad==11) {
            e4_inode_t dir;uint32_t dirino=bad==10 ? 2 : e4_u32(cfg+16);
            assert(!e4_inode(e->fs,dirino,&dir) && !e4_mapping(&dir));
            uint8_t *b=d->volatile_bytes+(uint64_t)e4_find(&dir,0)->physical*e->fs->bs;
            if (bad==11) e4_p32(b+12,dirino);
            else for (unsigned at=0;at<e->fs->bs-12;at+=e4_u16(b+at+4))
                if (e4_u32(b+at)==e4_u32(cfg+12)) { e4_p32(b+at,e4_u32(cfg+4));break; }
            e4_dir_sum(&dir,b);
        }
        if (bad==12) { e4_p16(raw+116,1);e4_inode_sum(e->fs,ino,raw); }
        e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));memcpy(d->stable,d->volatile_bytes,d->bytes);
        ext4_engine_close(e);d->events=0;e=NULL;
        assert(ext4_engine_open_journal_orphans(dev,true,&e)<0 && !e && !d->events && !live);
    }
    reset(d,orphan);assert(!ext4_engine_open_journal_orphans(dev,true,&e));size_t open_reads=reads,open_allocs=alloc_calls;
    ext4_engine_close(e);
    for (unsigned kind=0;kind<2;kind++) for (size_t at=0;at<(kind ? open_reads : open_allocs);at++) {
        reset(d,orphan);if (kind) fail_read=(long)at;else fail_alloc=(long)at;
        assert(ext4_engine_open_journal_orphans(dev,true,&e)<0 && !e && !d->events && !live);
    }
    reset(d,initial);assert(!ext4_engine_open_journal_orphans(dev,true,&e));
    for (unsigned i=0;i<64;i++) {
        char name[32];snprintf(name,sizeof(name),"orphan-%u",i);uint32_t ino;
        assert(!ext4_engine_namespace_create(e,2,name,false,&ino));intent_only(e,name,ino);
    }
    size_t stopped=d->events;
    assert(ext4_engine_file_truncate(e,e4_u32(cfg+4),0)==-VFS_EFBIG && d->events==stopped && !e->tainted);
    assert(ext4_engine_namespace_remove(e,2,"victim.bin",false)==-VFS_EFBIG && d->events==stopped);
    ownership(e);ext4_engine_close(e);assert(!ext4_engine_open_journal_orphans(dev,true,&e));
    assert(!ext4_engine_orphan_recover(e) && !e->orphan_count);ownership(e);ext4_engine_close(e);
    char path[1024];snprintf(path,sizeof(path),"%s-credit-recovered.img",prefix);save(path,d->stable,d->bytes);
    /* Foreign linked truncate intent, with a nonzero old partial tail. Recovery
     * must seal that tail even though this workbench did not create the intent. */
    reset(d,initial);assert(!ext4_engine_open_journal_orphans(dev,true,&e));
    uint32_t victim=e4_u32(cfg+4);e4_inode_t in;uint8_t *raw;assert(!e4_orphan_begin(e));
    assert(!e4_load_plan(e,victim,&in,&raw) && !e4_orphan_add(e,victim,raw));
    e4_p32(raw+4,e4_u32(cfg)+17);e4_inode_sum(e->fs,victim,raw);
    assert(!e4_namespace_end(e,0));e4_orphan_added(e,victim);ext4_engine_close(e);
    recover_check(dev,cfg,0,true,NULL);
    snprintf(path,sizeof(path),"%s-foreign-recovered.img",prefix);save(path,d->stable,d->bytes);
    printf("orphan recovery PASS cuts=%zu, 13 zero-write rejections, %zu admission read/%zu allocation failures, 64 credits\n",
        events*8,open_reads,open_allocs);fflush(stdout);free(orphan);
}
#ifdef EXT4_ORPHAN_LIBRARY
#define main phase84_main
#endif
int main(int argc,char **argv) {
    assert(argc==5 || argc==7);unsigned first=argc==7 ? (unsigned)strtoul(argv[5],NULL,10) : 0;assert(first<=7);
    size_t n,cn;uint8_t *initial=load(argv[1],&n),*cfg=load(argv[2],&cn);assert(cn==28);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=orphan_sector,.flush=ext4_fault_flush,.priv=&d};
    size_t cuts=argc==7 ? (size_t)strtoull(argv[6],NULL,10) : 0;
    /* Runner resumption uses retained, completed independent profile gates.
     * Every remaining profile still resets from the immutable source image. */
    if (!first) { lifecycle(&dev,initial,cfg,argv[4]);recovery_faults(&dev,initial,cfg,argv[4]); }
    for (unsigned profile=first;profile<7;profile++) {
        reset(&d,initial);ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
        reads=alloc_calls=0;assert(!operation(e,cfg,profile));size_t events=d.events,operation_reads=reads,allocs=alloc_calls;
        ext4_engine_close(e);assert(!live);char path[1024];
        snprintf(path,sizeof(path),"%s-%u-complete.img",argv[4],profile);save(path,d.stable,n);
        bool saved=false;
        for (unsigned persistence=0;persistence<4;persistence++) for (unsigned after=0;after<2;after++)
            for (size_t cut=0;cut<events;cut++) {
                reset(&d,initial);assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
                d.cut=(long)cut;d.after=after!=0;d.persistence=persistence;
                assert(operation(e,cfg,profile)<0 && e->tainted);
                size_t stopped=d.events;assert(ext4_engine_orphan_recover(e)==-VFS_EIO && d.events==stopped);
                ext4_engine_abort(e);assert(e->tainted);ext4_engine_close(e);assert(d.events==stopped && !live);
                ext4_fault_restart(&d);snprintf(path,sizeof(path),"%s-%u-pending.img",argv[4],profile);
                if (recover_check(&dev,cfg,profile,false,saved ? NULL : path)) saved=true;
                cuts++;
            }
        assert(saved);
        /* Every operation read/OOM failure is prepublication or leaves a
         * restartable committed prefix. Never clear taint on failed I/O. */
        for (unsigned kind=0;kind<2;kind++) for (size_t at=0;at<(kind ? operation_reads : allocs);at++) {
            reset(&d,initial);assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
            if (kind) fail_read=(long)at;else fail_alloc=(long)at;
            assert(operation(e,cfg,profile)<0);fail_read=fail_alloc=-1;
            if (!e->tainted) assert(!ext4_engine_orphan_recover(e));
            ext4_engine_close(e);ext4_fault_restart(&d);recover_check(&dev,cfg,profile,false,NULL);
        }
        reset(&d,initial);assert(!ext4_engine_open_journal_orphans(&dev,true,&e));assert(!operation(e,cfg,profile));
        ext4_engine_close(e);ext4_fault_restart(&d);recover_check(&dev,cfg,profile,true,NULL);
        snprintf(path,sizeof(path),"%s-%u-recovered.img",argv[4],profile);save(path,d.stable,n);
        printf("orphan profile=%u PASS events=%zu reads=%zu allocations=%zu\n",profile,events,operation_reads,allocs);fflush(stdout);
    }
    printf("EXT4 orphan host PASS block=%u sector=%u cuts=%zu; no mounted/physical journal claim\n",e4_u32(cfg),ss,cuts);
    free(initial);free(cfg);free(d.stable);free(d.volatile_bytes);assert(!live);return 0;
}
#ifdef EXT4_ORPHAN_LIBRARY
#undef main
#endif
