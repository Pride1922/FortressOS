#define main phase7_main
#include "jbd2_write_host.c"
#undef main
static ext4_engine_t *observed;
static bool file_write_sector(block_dev_t *dev,uint64_t lba,const void *bytes) {
    if (observed) {
        spin_debug_assert_unheld();ext4_fault_disk_t *d=dev->priv;
        uint64_t lo=lba*dev->sector_size,hi=lo+dev->sector_size;
        for (unsigned i=0;i<observed->images;i++) if (!observed->image[i].ordered) {
            uint64_t start=(uint64_t)observed->image[i].block*observed->fs->bs,end=start+observed->fs->bs;
            if (lo<end && hi>start && jbd2_writer_state(observed->writer)!=JBD2_WRITE_CHECKPOINTING) {
                uint64_t a=lo>start ? lo : start,b=hi<end ? hi : end;
                assert(!memcmp((const uint8_t *)bytes+a-lo,d->volatile_bytes+a,(size_t)(b-a)));
            }
        }
    }
    return ext4_fault_write(dev,lba,bytes);
}
static int64_t write_file(ext4_engine_t *e,uint32_t ino,uint64_t *off,bool append,const void *bytes,size_t len) {
    observed=e;int64_t r=ext4_engine_file_write(e,ino,off,append,bytes,len);observed=NULL;return r;
}
static void check_bytes(ext4_engine_t *e,uint32_t ino,const uint8_t *expected,size_t size,bool overwrite_old,
                         uint64_t offset,size_t count) {
    e4_node_t node={0};assert(!e4_inode(e->fs,ino,&node.inode));
    node.node.fs_private=&node.inode;assert(node.inode.size==size);
    uint8_t buffer[4096];
    for (size_t at=0;at<size;) {
        size_t take=size-at;if (take>sizeof(buffer)) take=sizeof(buffer);
        assert(e4_read(&node.node,at,buffer,take)==(int64_t)take);
        for (size_t k=0;k<take;k++) {
            if (overwrite_old && at+k>=offset && at+k-offset<count) assert(buffer[k]=='O' || buffer[k]=='J');
            else assert(buffer[k]==expected[at+k]);
        }
        at+=take;
    }
    assert(e4_read(&node.node,size,buffer,1)==0);
}
typedef struct { unsigned ino;uint64_t offset;bool append;unsigned len;uint8_t value; } operation_t;
static int64_t operate(ext4_engine_t *e,operation_t op,uint64_t *off) {
    uint8_t data[4096];assert(op.len<=sizeof(data));memset(data,op.value,op.len);
    return write_file(e,op.ino,off,op.append,data,op.len);
}
/* Private negative fixture: initialized preallocation beyond a shortened EOF
 * forces gap zeroing to consume more than the bounded image budget. */
static void initialize_tree(uint8_t *disk,unsigned bs,uint8_t *h,uint32_t seed,bool external) {
    unsigned depth=e4_u16(h+6),count=e4_u16(h+2);
    for (unsigned i=0;i<count;i++) {
        uint8_t *entry=h+12+i*12;
        if (depth) initialize_tree(disk,bs,disk+(uint64_t)e4_u32(entry+4)*bs,seed,true);
        else if (e4_u16(entry+4)>32768) e4_p16(entry+4,(uint16_t)(e4_u16(entry+4)-32768));
    }
    if (external) { unsigned tail=12+((bs-12)/12)*12;e4_p32(h+tail,e4_crc(seed,h,tail)); }
}
static bool recover_file(block_dev_t *dev,const uint8_t *initial,const e4_image_t *images,unsigned count,
                          unsigned bs,operation_t op,const uint8_t *expected,size_t old_size,size_t new_size,
                          bool must_commit,const char *pending) {
    ext4_fault_disk_t *d=dev->priv;jbd2_plan_t *plan=NULL;jbd2_report_t report;
    assert(!ext4_journal_analyze(dev,&plan,&report));
    bool logged=report.transactions!=0;
    if (logged && pending) save(pending,d->stable,d->bytes);
    assert(!jbd2_replay(plan,true));jbd2_release(plan);
    bool changed=false;
    for (unsigned i=0;i<count;i++) if (!images[i].ordered &&
        memcmp(initial+(uint64_t)images[i].block*bs,images[i].bytes,bs)) {
        changed=memcmp(d->stable+(uint64_t)images[i].block*bs,initial+(uint64_t)images[i].block*bs,bs)!=0;break;
    }
    if (must_commit || logged) assert(changed);
    for (unsigned i=0;i<count;i++) if (!images[i].ordered)
        assert(!memcmp(d->stable+(uint64_t)images[i].block*bs,
            changed ? images[i].bytes : initial+(uint64_t)images[i].block*bs,bs));
    ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_files(dev,true,&e));
    if (changed) check_bytes(e,op.ino,expected,new_size,false,0,0);
    else {
        uint8_t *old=malloc(old_size);assert(old);memset(old,op.value=='H' ? 0 : 'O',old_size);
        check_bytes(e,op.ino,old,old_size,op.offset<old_size && op.value!='H',op.offset,op.len);free(old);
    }
    ext4_engine_close(e);
    assert(!ext4_journal_analyze(dev,&plan,&report) && !report.transactions);
    size_t events=d->events;assert(!jbd2_replay(plan,true) && events==d->events);jbd2_release(plan);
    assert(!live);return logged;
}
int main(int argc,char **argv) {
    assert(argc==7);size_t n,cn;uint8_t *initial=load(argv[1],&n),*config=load(argv[2],&cn);assert(cn==24);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10),bs=e4_u32(config),ino=e4_u32(config+4),frag=e4_u32(config+20);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=file_write_sector,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_files(&dev,true,&e));
    size_t open_allocs=alloc_calls,open_reads=reads;
    /* Seed free initialized-group blocks with hostile old media bytes without
     * changing allocation metadata; no simulated device writes occur. */
    for (uint32_t g=0;g<e->fs->groups;g++) if (!(e4_u16(e->fs->gd[g].raw+18)&2)) {
        assert(e4_bitmap(e->fs,g,false));uint32_t start=e->fs->first+g*e->fs->bpg;
        for (uint32_t bit=0;bit<e->fs->bpg && start+bit<e->fs->blocks;bit++)
            if (!e4_bit(e->fs->scratch,bit)) memset(initial+(uint64_t)(start+bit)*bs,0xa5,bs);
    }
    e4_inode_t inode;assert(!e4_inode(e->fs,frag,&inode));size_t fragment_size=(size_t)inode.size;
    assert(e4_u16(inode.extent+6)==2 && !e4_mapping(&inode));
    for (unsigned i=0;i<inode.map->count;i++) {
        e4_extent_t x=inode.map->entries[i];assert(x.unwritten);
        for (uint32_t k=0;k<x.len;k++) memset(initial+(uint64_t)(x.physical+k)*bs,0xa5,bs);
    }
    ext4_engine_close(e);
    for (size_t cut=0;cut<open_allocs;cut++) {
        reset(&d,initial);fail_alloc=(long)cut;
        assert(ext4_engine_open_journal_files(&dev,true,&e)==-VFS_ENOMEM && !e && !live && !d.events && !e4_engine_busy);
    }
    for (size_t cut=0;cut<open_reads;cut++) {
        reset(&d,initial);fail_read=(long)cut;
        assert(ext4_engine_open_journal_files(&dev,true,&e)==-VFS_EIO && !e && !live && !d.events && !e4_engine_busy);
    }
    reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
    assert(ext4_engine_transaction_begin(e,(jbd2_credits_t){1,0,0})==-VFS_EOPNOTSUPP);
    uint64_t shared=0,independent=0;
    assert(write_file(e,ino,&shared,true,"abc",3)==3 && shared==3*bs+3);
    assert(write_file(e,ino,&independent,true,"def",3)==3 && independent==3*bs+6);
    assert(write_file(e,ino,&shared,true,"g",1)==1 && shared==3*bs+7);
    assert(write_file(e,ino,&independent,false,"x",1)==1 && independent==3*bs+7);
    uint64_t bad=UINT64_MAX;size_t events=d.events;
    assert(write_file(e,ino,&bad,false,"x",1)==-VFS_EFBIG && bad==UINT64_MAX && d.events==events);
    assert(ext4_engine_trim(e,ino,0)==-VFS_EOPNOTSUPP && ext4_engine_finish(e)==-VFS_EOPNOTSUPP);
    ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
    for (unsigned k=0;k<5;k++) {
        uint64_t off=(10u+k*2u)*bs;
        assert(write_file(e,ino,&off,false,"P",1)==1);
    }
    assert(!e4_inode(e->fs,ino,&inode) && e4_u16(inode.extent+6)==1 && !e4_mapping(&inode));
    assert(inode.map->count==6);ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
    unsigned group=(frag-1)/e->fs->ipg,index=(frag-1)%e->fs->ipg;
    uint8_t *raw=d.volatile_bytes+(uint64_t)e4_u32(e->fs->gd[group].raw+8)*bs+index*256;
    initialize_tree(d.volatile_bytes,bs,raw+40,e4_inode_seed(e->fs,frag,e4_u32(raw+100)),false);
    e4_p32(raw+4,1);e4_p32(raw+108,0);e4_inode_sum(e->fs,frag,raw);
    memcpy(d.stable,d.volatile_bytes,n);ext4_engine_close(e);
    assert(!ext4_engine_open_journal_files(&dev,true,&e));
    uint64_t exhausted=fragment_size;
    assert(write_file(e,frag,&exhausted,false,"x",1)==-VFS_EFBIG);
    assert(exhausted==fragment_size && !e->tainted && !d.events);
    ext4_engine_close(e);assert(!live);
    /* Reserve all free storage in a checksummed synthetic admission fixture.
     * Allocation failure must remain a write-free planning failure. */
    reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
    for (uint32_t g=0;g<e->fs->groups;g++) {
        uint8_t *gd=d.volatile_bytes+(uint64_t)(e->fs->first+1)*bs+g*32;
        uint8_t *bitmap=d.volatile_bytes+(uint64_t)e4_u32(gd)*bs;
        memset(bitmap,255,bs);e4_p16(gd+12,0);e4_p16(gd+18,(uint16_t)(e4_u16(gd+18)&~2u));
        e4_p16(gd+24,(uint16_t)e4_crc(e->fs->seed,bitmap,e->fs->bpg/8));
        uint8_t number[4];e4_p32(number,g);e4_p16(gd+30,0);
        e4_p16(gd+30,(uint16_t)e4_crc(e4_crc(e->fs->seed,number,4),gd,32));
    }
    uint8_t *sb=d.volatile_bytes+1024;e4_p32(sb+12,0);e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));
    memcpy(d.stable,d.volatile_bytes,n);ext4_engine_close(e);
    assert(!ext4_engine_open_journal_files(&dev,true,&e));exhausted=5u*bs;
    assert(write_file(e,ino,&exhausted,false,"x",1)==-VFS_ENOSPC);
    assert(exhausted==5u*bs && !e->tainted && !d.events);ext4_engine_close(e);assert(!live);
    operation_t operations[]={{ino,5u*bs+23,false,33,'J'},{ino,0,true,33,'J'},
        {ino,bs-7,false,33,'J'},{frag,2u*bs,false,bs,'H'}};
    size_t cuts=0;bool saved_pending=false;
    for (unsigned profile=0;profile<4;profile++) {
        operation_t op=operations[profile];size_t old_size=profile==3 ? fragment_size : 3u*bs;
        uint64_t off=op.offset;uint64_t effective=op.append ? old_size : off;
        size_t new_size=old_size;if (effective+op.len>new_size) new_size=(size_t)effective+op.len;
        uint8_t *expected=calloc(1,new_size);assert(expected);
        if (profile!=3) memset(expected,'O',old_size);
        memset(expected+effective,op.value,op.len);
        reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
        alloc_calls=reads=0;d.trace=profile==0;
        assert(operate(e,op,&off)==op.len && off==effective+op.len);d.trace=false;
        size_t count=e->images,events=d.events,write_allocs=alloc_calls,write_reads=reads;
        e4_image_t *images=malloc(count*sizeof(*images));assert(images);memcpy(images,e->image,count*sizeof(*images));
        check_bytes(e,op.ino,expected,new_size,false,0,0);
        if (profile==0) save(argv[5],d.stable,n);
        if (profile==3) {
            char path[4096];assert(snprintf(path,sizeof(path),"%s.fragment.img",argv[5])>0);save(path,d.stable,n);
            assert(e->revoke_count && e->old_count && e->fs->journal_ranges);
        }
        ext4_engine_close(e);
        for (size_t cut=0;cut<write_allocs;cut++) {
            reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));off=op.offset;fail_alloc=(long)cut;
            assert(operate(e,op,&off)==-VFS_ENOMEM && off==op.offset && !e->tainted && !d.events);
            ext4_engine_close(e);assert(!live);
        }
        for (size_t cut=0;cut<write_reads;cut++) {
            reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));off=op.offset;fail_read=(long)cut;
            assert(operate(e,op,&off)==-VFS_EIO && off==op.offset);
            bool mutated=d.events!=0;assert(!mutated || e->tainted);fail_read=-1;
            ext4_engine_close(e);ext4_fault_restart(&d);
            recover_file(&dev,initial,images,(unsigned)count,bs,op,expected,old_size,new_size,false,NULL);
        }
        for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
            reset(&d,initial);d.persistence=mode;assert(!ext4_engine_open_journal_files(&dev,true,&e));
            off=op.offset;d.cut=(long)cut;d.after=after;
            assert(operate(e,op,&off)==-VFS_EIO && off==op.offset && e->tainted && !e->fs->metadata_enabled);
            size_t stopped=d.events;assert(operate(e,op,&off)==-VFS_EIO && d.events==stopped);
            ext4_engine_close(e);ext4_fault_restart(&d);
            bool pending=recover_file(&dev,initial,images,(unsigned)count,bs,op,expected,old_size,new_size,false,
                profile==0 && !saved_pending ? argv[4] : NULL);
            if (profile==0 && pending) saved_pending=true;
            cuts++;
        }
        if (profile==0) {
            reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));off=op.offset;
            assert(operate(e,op,&off)==op.len);ext4_engine_close(e);ext4_fault_restart(&d);
            recover_file(&dev,initial,images,(unsigned)count,bs,op,expected,old_size,new_size,true,NULL);save(argv[6],d.stable,n);
        }
        printf("PASS file profile=%u bs=%u sector=%u: %zu images, %zu events, %zu crash cuts, %zu read/%zu snapshot failures\n",
            profile,bs,ss,count,events,events*8,write_reads,write_allocs);
        free(images);free(expected);
    }
    assert(saved_pending);printf("PASS EXT4 8.2: %zu crash cuts; shared/independent append, ordered sparse/unwritten/depth-2 allocation, exact ownership, zero stale bytes, no precommit home change\n",cuts);
    free(d.stable);free(d.volatile_bytes);free(initial);free(config);assert(!live);return 0;
}
