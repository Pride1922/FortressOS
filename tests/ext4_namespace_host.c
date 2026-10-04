#define main phase7_main
#include "jbd2_write_host.c"
#undef main
static ext4_engine_t *observed;
static char long_name[64];
static bool namespace_sector(block_dev_t *dev,uint64_t lba,const void *bytes) {
    spin_debug_assert_unheld();
    if (observed) {
        ext4_fault_disk_t *d=dev->priv;uint64_t lo=lba*dev->sector_size,hi=lo+dev->sector_size;
        for (unsigned i=0;i<observed->images;i++) {
            assert(!observed->image[i].ordered);
            uint64_t start=(uint64_t)observed->image[i].block*observed->fs->bs,end=start+observed->fs->bs;
            if (lo<end && hi>start && jbd2_writer_state(observed->writer)!=JBD2_WRITE_CHECKPOINTING) {
                uint64_t a=lo>start ? lo : start,b=hi<end ? hi : end;
                assert(!memcmp((const uint8_t *)bytes+a-lo,d->volatile_bytes+a,(size_t)(b-a)));
            }
        }
    }
    return ext4_fault_write(dev,lba,bytes);
}
static uint32_t lookup_inode(ext4_engine_t *e,uint32_t parent,const char *name) {
    e4_inode_t dir;uint32_t ino;assert(!e4_inode(e->fs,parent,&dir));
    assert(e4_scan(&dir,name,0,NULL,&ino)>=0);return ino;
}
static int operation(ext4_engine_t *e,const uint8_t *config,unsigned profile,uint32_t *out) {
    uint32_t source=e4_u32(config+4),dest=e4_u32(config+8),full=e4_u32(config+20);
    observed=e;int r=-VFS_EINVAL;
    switch (profile) {
    case 0:r=ext4_engine_namespace_create(e,2,"newfile",false,out);break;
    case 1:r=ext4_engine_namespace_create(e,2,"newdir",true,out);break;
    case 2:r=ext4_engine_namespace_rename(e,source,"file.bin",source,"renamed.bin");break;
    case 3:r=ext4_engine_namespace_rename(e,source,"file.bin",dest,"renamed.bin");break;
    case 4:r=ext4_engine_namespace_remove(e,2,"victim.bin",false);break;
    case 5:r=ext4_engine_namespace_remove(e,2,"empty",true);break;
    case 6:r=ext4_engine_namespace_create(e,full,long_name,false,out);break;
    case 7:r=ext4_engine_namespace_rename(e,source,"file.bin",full,long_name);break;
    case 8:r=ext4_engine_namespace_remove(e,2,"tree.bin",false);break;
    }
    observed=NULL;return r;
}
/* Independent allocation-set audit, not just equal free totals: all allocated
 * data/tree blocks belong to exactly one inode; only fixed/journal metadata
 * may be allocated without appearing in a normal inode's extent tree. */
static void audit(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;uint8_t *owned=calloc(1,(fs->blocks+7)/8);assert(owned);
    for (unsigned i=0;i<fs->ranges;i++) for (uint32_t k=0;k<fs->reserved[i].len;k++)
        e4_set_bit(owned,fs->reserved[i].lo+k,true);
    for (unsigned i=0;i<fs->journal_ranges;i++) for (uint32_t k=0;k<fs->journal_reserved[i].len;k++)
        e4_set_bit(owned,fs->journal_reserved[i].lo+k,true);
    for (uint32_t g=0;g<fs->groups;g++) {
        if (e4_u16(fs->gd[g].raw+18)&1) continue;
        assert(e4_bitmap(fs,g,true));uint8_t allocated[4096];memcpy(allocated,fs->scratch,fs->bs);
        for (uint32_t bit=0;bit<fs->ipg;bit++) {
            uint32_t ino=g*fs->ipg+bit+1;if (!e4_bit(allocated,bit) || (ino<11 && ino!=2)) continue;
            e4_inode_t in;assert(!e4_inode(fs,ino,&in));
            fs->pending_count=fs->physical_count=fs->visits=0;
            assert(!e4_tree(&in,in.extent,e4_u16(in.extent+6),true,0,1ULL<<32));
            for (unsigned i=0;i<fs->physical_count;i++) for (uint32_t k=0;k<fs->physical[i].len;k++) {
                uint32_t block=fs->physical[i].lo+k;assert(!e4_bit(owned,block));e4_set_bit(owned,block,true);
            }
            if (in.mode==0x4000) {
                uint32_t child;vfs_dirent_t ent;unsigned links=2,dots=0;
                for (uint64_t cookie=0;;cookie++) {
                    int r=e4_scan(&in,NULL,cookie,&ent,&child);assert(r>=0);if (!r) break;
                    e4_inode_t target;assert(!e4_inode(fs,child,&target));
                    assert(target.mode==(ent.type==VFS_DIRECTORY ? 0x4000 : 0x8000));
                    if (!strcmp(ent.name,".")) { assert(child==ino);dots|=1; }
                    else if (!strcmp(ent.name,"..")) { assert(target.mode==0x4000);dots|=2; }
                    else if (ent.type==VFS_DIRECTORY) links++;
                }
                uint8_t raw[256];assert(e4_bytes(fs,(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+bit*256,raw,256));
                assert(dots==3 && e4_u16(raw+26)==links);
            }
        }
    }
    for (uint32_t g=0;g<fs->groups;g++) {
        uint32_t start=fs->first+g*fs->bpg,count=fs->blocks-start;if (count>fs->bpg) count=fs->bpg;
        if (e4_u16(fs->gd[g].raw+18)&2) {
            for (uint32_t bit=0;bit<count;bit++) if (e4_bit(owned,start+bit)) {
                bool fixed=false;
                for (unsigned i=0;i<fs->ranges;i++) if (start+bit>=fs->reserved[i].lo &&
                    start+bit-fs->reserved[i].lo<fs->reserved[i].len) fixed=true;
                assert(fixed);
            }
        } else {
            assert(e4_bitmap(fs,g,false));
            for (uint32_t bit=0;bit<count;bit++) assert(e4_bit(fs->scratch,bit)==e4_bit(owned,start+bit));
        }
    }
    free(owned);
}
static void check_namespace(ext4_engine_t *e,const uint8_t *config,unsigned profile,bool changed,uint32_t created) {
    uint32_t source=e4_u32(config+4),dest=e4_u32(config+8),full=e4_u32(config+20),file=e4_u32(config+24);
    if (profile==0 || profile==1 || profile==6) {
        uint32_t ino=lookup_inode(e,profile==6 ? full : 2,profile==0 ? "newfile" : profile==1 ? "newdir" : long_name);
        assert(ino==(changed ? created : 0));
        if (ino) {
            e4_inode_t in;assert(!e4_inode(e->fs,ino,&in));
            assert(in.mode==(profile==1 ? 0x4000 : 0x8000) && in.size==(profile==1 ? e->fs->bs : 0));
            if (profile==1) assert(lookup_inode(e,ino,".")==ino && lookup_inode(e,ino,"..")==2);
        }
    } else if (profile==2 || profile==3 || profile==7) {
        assert(lookup_inode(e,source,"file.bin")==(!changed ? file : 0));
        assert(lookup_inode(e,profile==2 ? source : profile==3 ? dest : full,profile==7 ? long_name : "renamed.bin")==
            (changed ? file : 0));
        e4_node_t node={0};assert(!e4_inode(e->fs,file,&node.inode));node.node.fs_private=&node.inode;
        uint8_t data[4096];assert(node.inode.size==3u*e->fs->bs);
        for (unsigned k=0;k<3;k++) { assert(e4_read(&node.node,k*e->fs->bs,data,e->fs->bs)==e->fs->bs);
            for (unsigned i=0;i<e->fs->bs;i++) assert(data[i]=='O'); }
    } else {
        const char *name=profile==4 ? "victim.bin" : profile==5 ? "empty" : "tree.bin";
        unsigned offset=profile==4 ? 28 : profile==5 ? 12 : 32;
        assert(lookup_inode(e,2,name)==(changed ? 0 : e4_u32(config+offset)));
    }
    audit(e);
}
static bool recover_namespace(block_dev_t *dev,const uint8_t *initial,const e4_image_t *images,unsigned count,
                              const uint8_t *config,unsigned profile,uint32_t created,const char *pending) {
    ext4_fault_disk_t *d=dev->priv;jbd2_plan_t *plan=NULL;jbd2_report_t report;
    assert(!ext4_journal_analyze(dev,&plan,&report));bool logged=report.transactions!=0;
    if (logged && pending) save(pending,d->stable,d->bytes);
    assert(!jbd2_replay(plan,true));jbd2_release(plan);
    unsigned bs=e4_u32(config);bool changed=false,identified=false;
    for (unsigned i=0;i<count;i++) if (memcmp(initial+(uint64_t)images[i].block*bs,images[i].bytes,bs)) {
        const uint8_t *actual=d->stable+(uint64_t)images[i].block*bs;
        changed=!memcmp(actual,images[i].bytes,bs);
        assert(changed || !memcmp(actual,initial+(uint64_t)images[i].block*bs,bs));identified=true;break;
    }
    assert(identified && (!logged || changed));
    for (unsigned i=0;i<count;i++) assert(!memcmp(d->stable+(uint64_t)images[i].block*bs,
        changed ? images[i].bytes : initial+(uint64_t)images[i].block*bs,bs));
    ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_namespace(dev,true,&e));
    check_namespace(e,config,profile,changed,created);ext4_engine_close(e);
    assert(!ext4_journal_analyze(dev,&plan,&report) && !report.transactions);
    size_t events=d->events;assert(!jbd2_replay(plan,true) && d->events==events);jbd2_release(plan);assert(!live);
    return logged;
}
int main(int argc,char **argv) {
    assert(argc==5);memset(long_name,'n',63);long_name[63]=0;
    size_t n,cn;uint8_t *initial=load(argv[1],&n),*config=load(argv[2],&cn);assert(cn==40);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10),bs=e4_u32(config);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=namespace_sector,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_namespace(&dev,true,&e));audit(e);
    uint32_t out=UINT32_MAX;size_t events=d.events;
    assert(ext4_engine_namespace_create(e,2,"..",false,&out)==-VFS_EINVAL && out==UINT32_MAX);
    char overlong[65];memset(overlong,'a',64);overlong[64]=0;
    assert(ext4_engine_namespace_create(e,2,overlong,false,&out)==-VFS_EINVAL);
    assert(ext4_engine_namespace_create(e,2,"victim.bin",false,&out)==-VFS_EEXIST && out==UINT32_MAX);
    assert(ext4_engine_namespace_remove(e,2,"missing",false)==-VFS_ENOENT);
    assert(ext4_engine_namespace_remove(e,2,"nonempty",true)==-VFS_ENOTEMPTY);
    assert(ext4_engine_namespace_remove(e,2,"large.bin",false)==-VFS_EFBIG);
    assert(ext4_engine_namespace_remove(e,2,"empty",false)==-VFS_EOPNOTSUPP);
    assert(ext4_engine_namespace_rename(e,2,"empty",2,"moved")==-VFS_EOPNOTSUPP);
    assert(ext4_engine_namespace_rename(e,e4_u32(config+4),"file.bin",2,"victim.bin")==-VFS_EEXIST);
    assert(ext4_engine_namespace_rename(e,e4_u32(config+4),"file.bin",e4_u32(config+4),"file.bin")==0);
    assert(d.events==events && !e->tainted);ext4_engine_close(e);assert(!live);
    reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
    /* A future mounted adapter must reject active targets until 8.4. */
    e4_node_t *pin=kcalloc(1,sizeof(*pin));assert(pin);pin->inode.ino=e4_u32(config+28);pin->opens=1;
    e->fs->cached[e->fs->nodes++]=pin;
    assert(ext4_engine_namespace_remove(e,2,"victim.bin",false)==-VFS_EOPNOTSUPP && !d.events);
    ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
    uint32_t generation=0,identity=0;
    for (unsigned cycle=0;cycle<64;cycle++) {
        uint32_t created=0;assert(!ext4_engine_namespace_create(e,2,"cycle",false,&created));
        assert(!e->fs->maps && !e->fs->map_entries);
        if (cycle) assert(created==identity);
        identity=created;
        e4_inode_t in;assert(!e4_inode(e->fs,created,&in) && in.generation>generation);generation=in.generation;
        uint64_t off=0;assert(ext4_engine_file_write(e,created,&off,false,"persistent",10)==10 && off==10);
        assert(!ext4_engine_namespace_rename(e,2,"cycle",e4_u32(config+8),"cycle"));
        assert(!ext4_engine_namespace_remove(e,e4_u32(config+8),"cycle",false));
        assert(!e->fs->maps && !e->fs->map_entries);
        assert(!ext4_engine_namespace_create(e,2,"cycle-dir",true,&created));
        assert(!ext4_engine_namespace_remove(e,2,"cycle-dir",true));
        assert(!e->fs->maps && !e->fs->map_entries && !e->tainted);
    }
    audit(e);ext4_engine_close(e);assert(!live);
    /* Synthetic inode-exhaustion fixture, not an independent fsck claim. */
    reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
    for (uint32_t g=0;g<e->fs->groups;g++) {
        uint8_t *gd=d.volatile_bytes+(uint64_t)(e->fs->first+1)*bs+g*32;
        uint8_t *bitmap=d.volatile_bytes+(uint64_t)e4_u32(gd+4)*bs;
        memset(bitmap,255,bs);e4_p16(gd+14,0);e4_p16(gd+18,(uint16_t)(e4_u16(gd+18)&~1u));
        e4_p16(gd+26,(uint16_t)e4_crc(e->fs->seed,bitmap,e->fs->ipg/8));
        uint8_t number[4];e4_p32(number,g);e4_p16(gd+30,0);
        e4_p16(gd+30,(uint16_t)e4_crc(e4_crc(e->fs->seed,number,4),gd,32));
    }
    uint8_t *sb=d.volatile_bytes+1024;e4_p32(sb+16,0);e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));
    memcpy(d.stable,d.volatile_bytes,n);ext4_engine_close(e);
    assert(!ext4_engine_open_journal_namespace(&dev,true,&e));out=UINT32_MAX;
    assert(ext4_engine_namespace_create(e,2,"exhausted",false,&out)==-VFS_ENOSPC && out==UINT32_MAX && !d.events && !e->tainted);
    ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
    e4_inode_t root_inode;assert(!e4_inode(e->fs,2,&root_inode) && !e4_mapping(&root_inode));
    uint8_t *root_bytes=d.volatile_bytes+(uint64_t)e4_find(&root_inode,0)->physical*bs;
    root_bytes[7]=1;e4_dir_sum(&root_inode,root_bytes);
    memcpy(d.stable,d.volatile_bytes,n);ext4_engine_close(e);
    assert(!ext4_engine_open_journal_namespace(&dev,true,&e));out=UINT32_MAX;
    assert(ext4_engine_namespace_create(e,2,"bad-parent",false,&out)==-VFS_EIO && out==UINT32_MAX && !d.events && !e->tainted);
    ext4_engine_close(e);
    reset(&d,initial);assert(!ext4_engine_open_journal_files(&dev,true,&e));
    assert(ext4_engine_namespace_create(e,2,"newfile",false,&out)==-VFS_EOPNOTSUPP && !d.events);
    ext4_engine_close(e);size_t cuts=0;
    for (unsigned profile=0;profile<9;profile++) {
        reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
        alloc_calls=reads=0;out=UINT32_MAX;assert(!operation(e,config,profile,&out));
        size_t count=e->images,events=d.events,write_allocs=alloc_calls,write_reads=reads;
        e4_image_t *images=malloc(count*sizeof(*images));assert(images);memcpy(images,e->image,count*sizeof(*images));
        if (profile==4 || profile==5 || profile==8) assert(e->revoke_count==(profile==4 ? 3u : profile==5 ? 1u : 6u));
        if (profile==6 || profile==7) { e4_inode_t dir;assert(!e4_inode(e->fs,e4_u32(config+20),&dir) && dir.size==2u*bs); }
        check_namespace(e,config,profile,true,out);
        char checkpointed[4096],pending[4096],recovered[4096];
        assert(snprintf(checkpointed,sizeof(checkpointed),"%s-%u-checkpointed.img",argv[4],profile)>0);
        assert(snprintf(pending,sizeof(pending),"%s-%u-committed.img",argv[4],profile)>0);
        assert(snprintf(recovered,sizeof(recovered),"%s-%u-recovered.img",argv[4],profile)>0);
        save(checkpointed,d.stable,n);ext4_engine_close(e);bool saved=false;
        for (size_t cut=0;cut<write_allocs;cut++) {
            reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
            uint32_t provisional=UINT32_MAX;fail_alloc=(long)cut;
            assert(operation(e,config,profile,&provisional)==-VFS_ENOMEM && provisional==UINT32_MAX && !e->tainted && !d.events);
            ext4_engine_close(e);assert(!live);
        }
        for (size_t cut=0;cut<write_reads;cut++) {
            reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
            uint32_t provisional=UINT32_MAX;fail_read=(long)cut;
            assert(operation(e,config,profile,&provisional)==-VFS_EIO && provisional==UINT32_MAX);
            assert(!d.events || e->tainted);ext4_engine_close(e);fail_read=-1;ext4_fault_restart(&d);
            recover_namespace(&dev,initial,images,(unsigned)count,config,profile,out,NULL);
        }
        for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
            reset(&d,initial);d.persistence=mode;assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
            d.cut=(long)cut;d.after=after;uint32_t provisional=UINT32_MAX;
            assert(operation(e,config,profile,&provisional)==-VFS_EIO && provisional==UINT32_MAX && e->tainted && !e->fs->metadata_enabled);
            size_t stopped=d.events;assert(operation(e,config,profile,&provisional)==-VFS_EIO && d.events==stopped);
            ext4_engine_close(e);ext4_fault_restart(&d);
            if (recover_namespace(&dev,initial,images,(unsigned)count,config,profile,out,saved ? NULL : pending)) saved=true;
            cuts++;
        }
        assert(saved);reset(&d,initial);assert(!ext4_engine_open_journal_namespace(&dev,true,&e));
        uint32_t created=UINT32_MAX;assert(!operation(e,config,profile,&created));ext4_engine_close(e);ext4_fault_restart(&d);
        recover_namespace(&dev,initial,images,(unsigned)count,config,profile,created,NULL);save(recovered,d.stable,n);
        printf("PASS namespace profile=%u bs=%u sector=%u: %zu images, %zu events, %zu crash cuts, %zu read/%zu allocation failures\n",
            profile,bs,ss,count,events,events*8,write_reads,write_allocs);fflush(stdout);free(images);
    }
    printf("PASS EXT4 8.3: %zu crash cuts; names, directory checksums/links, exact ownership, bounded revokes, taint and idempotence\n",cuts);
    free(d.stable);free(d.volatile_bytes);free(initial);free(config);assert(!live);return 0;
}
