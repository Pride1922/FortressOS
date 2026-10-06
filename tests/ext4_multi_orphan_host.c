/* Three simultaneous intents: two deleted files and one linked shrink. */
#define EXT4_ORPHAN_LIBRARY
#include "ext4_orphan_host.c"
static void multi_check(ext4_engine_t *e,unsigned bs) {
    assert(!e->orphan_count && !lookup(e,"victim.bin") && !lookup(e,"large.bin"));
    uint32_t ino=lookup(e,"tree.bin");assert(ino);e4_inode_t in;
    assert(!e4_inode(e->fs,ino,&in) && in.size==bs && !e4_mapping(&in));
    ownership(e);
}
static int drain(block_dev_t *dev,ext4_engine_t **out) {
    jbd2_plan_t *plan=NULL;jbd2_report_t report;int r=ext4_journal_analyze(dev,&plan,&report);
    if (!r) r=jbd2_replay(plan,true);
    jbd2_release(plan);
    if (!r) r=ext4_engine_open_journal_orphans(dev,true,out);
    if (!r) r=ext4_engine_orphan_recover(*out);
    return r;
}
int main(int argc,char **argv) {
    assert(argc==5);size_t n,cn;uint8_t *initial=load(argv[1],&n),*cfg=load(argv[2],&cn);assert(cn==28);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10),bs=e4_u32(cfg);assert(ss==512 || ss==4096);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=orphan_sector,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);ext4_engine_t *e=NULL;assert(!ext4_engine_open_journal_orphans(&dev,true,&e));
    intent_only(e,"victim.bin",e4_u32(cfg+4));intent_only(e,"large.bin",e4_u32(cfg+8));
    uint32_t tree=e4_u32(cfg+12);assert(!e4_orphan_begin(e));e4_inode_t in;uint8_t *raw;
    assert(!e4_load_plan(e,tree,&in,&raw) && !e4_orphan_add(e,tree,raw));
    e4_p32(raw+4,bs);e4_p32(raw+108,0);e4_inode_sum(e->fs,tree,raw);
    assert(!e4_namespace_end(e,0));e4_orphan_added(e,tree);assert(e->orphan_count==3);ownership(e);
    uint8_t *pending=malloc(n);assert(pending);memcpy(pending,d.stable,n);ext4_engine_close(e);assert(!live);
    char path[1024];snprintf(path,sizeof(path),"%s-pending.img",argv[4]);save(path,pending,n);
    reset(&d,pending);assert(!drain(&dev,&e));multi_check(e,bs);size_t events=d.events;
    size_t stopped=d.events;assert(!ext4_engine_orphan_recover(e) && d.events==stopped);
    snprintf(path,sizeof(path),"%s-clean.img",argv[4]);save(path,d.stable,n);ext4_engine_close(e);
    size_t cuts=0,repeated=0;
    for (unsigned persistence=0;persistence<4;persistence++) for (size_t cut=0;cut<events;cut++) for (unsigned after=0;after<2;after++) {
        reset(&d,pending);d.persistence=persistence;d.cut=(long)cut;d.after=after!=0;e=NULL;
        int r=drain(&dev,&e);assert(r==-VFS_EIO && d.offline);if (e) {assert(e->tainted);ext4_engine_close(e);}assert(!live);
        ext4_fault_restart(&d);d.persistence=persistence;d.cut=0;d.after=after!=0;e=NULL;
        r=drain(&dev,&e);
        if (d.offline) {assert(r==-VFS_EIO);if (e) assert(e->tainted);repeated++;} else assert(!r);
        if (e) ext4_engine_close(e);
        assert(!live);
        ext4_fault_restart(&d);d.persistence=0;e=NULL;assert(!drain(&dev,&e));multi_check(e,bs);
        stopped=d.events;assert(!ext4_engine_orphan_recover(e) && stopped==d.events);ext4_engine_close(e);assert(!live);cuts++;
        if (!(cuts%100)) {printf("progress multi-orphan cuts=%zu/%zu\n",cuts,events*8);fflush(stdout);}
    }
    printf("MULTI ORPHAN PASS block=%u sector=%u events=%zu cuts=%zu repeated=%zu\n",bs,ss,events,cuts,repeated);
    free(initial);free(cfg);free(pending);free(d.stable);free(d.volatile_bytes);return 0;
}
