#define EXT4_ORPHAN_LIBRARY
#include "ext4_orphan_host.c"
static void deep_recover(block_dev_t *dev,const uint8_t *cfg,bool deleted) {
    jbd2_plan_t *plan=NULL;assert(!ext4_journal_analyze(dev,&plan,NULL));
    assert(!jbd2_replay(plan,true));jbd2_release(plan);ext4_engine_t *e=NULL;
    assert(!ext4_engine_open_journal_orphans(dev,true,&e) && !ext4_engine_orphan_recover(e));
    uint32_t ino=lookup(e,"deep.bin");assert(ino==0 || ino==e4_u32(cfg+20));if (deleted) assert(!ino);
    ownership(e);assert(!e->orphan_count);ext4_engine_close(e);assert(!live);
}
int main(int argc,char **argv) {
    assert(argc==5 || (argc==6 && !strcmp(argv[5],"--admission-check")));
    size_t n,cn;uint8_t *initial=load(argv[1],&n),*cfg=load(argv[2],&cn);assert(cn==28);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=orphan_sector,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
    if (argc==6) {
        assert(!d.events);ext4_engine_close(e);fixture_t f=fixture(&dev,initial,cfg);
        uint8_t *small=d.volatile_bytes+(uint64_t)f.map[0]*f.bs;
        j_put(small+20,f.source.journal_blocks-3);j_put(small+88,f.source.journal_blocks-3);
        jw_seal(small,1024,252,UINT32_MAX);free(f.map);
        assert(ext4_engine_open_journal_orphans(&dev,true,&e)==-VFS_ENOSPC && !e && !d.events && !live);
        printf("EXT4 orphan admission PASS block=%u sector=%u, insufficient cleanup credits reject with zero writes\n",e4_u32(cfg),ss);
        goto done;
    }
    e4_inode_t in;assert(!e4_inode(e->fs,e4_u32(cfg+20),&in) && e4_u16(in.extent+6)==2);
    observed=e;assert(!ext4_engine_namespace_remove(e,2,"deep.bin",false));observed=NULL;
    assert(!e->orphan_count && !lookup(e,"deep.bin"));ownership(e);size_t events=d.events;ext4_engine_close(e);
    char path[1024];snprintf(path,sizeof(path),"%s-complete.img",argv[4]);save(path,d.stable,n);
    size_t cuts[]={0,events/2,events-1};bool saved=false;
    for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (unsigned k=0;k<3;k++) {
        reset(&d,initial);assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
        d.cut=(long)cuts[k];d.persistence=mode;d.after=after;observed=e;
        assert(ext4_engine_namespace_remove(e,2,"deep.bin",false)==-VFS_EIO && e->tainted);observed=NULL;
        ext4_engine_close(e);ext4_fault_restart(&d);
        if (k==1 && !saved) { snprintf(path,sizeof(path),"%s-pending.img",argv[4]);save(path,d.stable,n);saved=true; }
        deep_recover(&dev,cfg,k!=0);
    }
    assert(saved);snprintf(path,sizeof(path),"%s-recovered.img",argv[4]);save(path,d.stable,n);
    printf("EXT4 full depth-2 reclaim PASS: %u sparse extents, %zu events, 24 selected cuts/restarts, zero leaks/double frees\n",
        e4_u32(cfg+24)/2+1,events);
done:
    free(d.stable);free(d.volatile_bytes);free(initial);free(cfg);assert(!live);return 0;
}
