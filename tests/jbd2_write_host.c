#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ext4_fault_disk.h"
#include "../src/fs/ext4.c"
#include "../src/fs/jbd2.c"
static size_t live,reads,alloc_calls;
static long fail_alloc=-1,fail_read=-1;
static vfs_node_t root;
void *kmalloc(size_t n) { alloc_calls++;if (!fail_alloc) return NULL;if (fail_alloc>0) fail_alloc--;void *p=malloc(n);if (p) live++;return p; }
void *kcalloc(size_t n,size_t z) { if (n && z>SIZE_MAX/n) return NULL;void *p=kmalloc(n*z);if (p) memset(p,0,n*z);return p; }
void kfree(void *p) { if (p) { assert(live);live--;free(p); } }
bool block_read_sector(block_dev_t *d,uint64_t l,void *p) { reads++;if (!fail_read) return false;if (fail_read>0) fail_read--;return d->read_sector(d,l,p); }
bool block_write_sector(block_dev_t *d,uint64_t l,const void *p) { return d->write_sector(d,l,p); }
bool block_flush(block_dev_t *d) { return d->flush(d); }
void vfs_set_last_create_error(int e) { (void)e; }
vfs_node_t *vfs_lookup(const char *p) { return strcmp(p,"/") ? NULL : &root; }
static uint8_t *load(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");assert(f && !fseek(f,0,SEEK_END));*size=(size_t)ftell(f);rewind(f);
    uint8_t *b=malloc(*size);assert(b && fread(b,1,*size,f)==*size);fclose(f);return b;
}
static void save(const char *path,const uint8_t *bytes,size_t size) {
    FILE *f=fopen(path,"wbx");assert(f && fwrite(bytes,1,size,f)==size);fclose(f);
}
static void reset(ext4_fault_disk_t *d,const uint8_t *bytes) {
    assert(!live);memcpy(d->stable,bytes,d->bytes);ext4_fault_restart(d);
    fail_read=fail_alloc=-1;reads=alloc_calls=0;d->persistence=0;
}
typedef struct {
    jbd2_source_t source;
    uint32_t *map;
    unsigned bs, inodeblock, offset, fileino, targets[3];
    uint8_t inode[4096], escaped[4096], ordered[4096];
} fixture_t;
static fixture_t fixture(block_dev_t *dev,const uint8_t *initial,const uint8_t *config) {
    fixture_t f={0};f.bs=e4_u32(config);f.fileino=e4_u32(config+4);
    for (unsigned n=0;n<3;n++) f.targets[n]=e4_u32(config+8+n*4);
    ext4_mount_t *fs=kcalloc(1,sizeof(*fs));assert(fs);fs->dev=dev;
    fs->bs=dev->sector_size;fs->blocks=(uint32_t)dev->sector_count;fs->journal_bootstrap=true;
    assert(!e4_admit(fs));e4_inode_t in;assert(!e4_inode(fs,fs->journal_ino,&in) && !e4_mapping(&in));
    unsigned count=(unsigned)(in.size/fs->bs),at=0;f.map=malloc(count*4);assert(f.map);
    for (unsigned i=0;i<in.map->count;i++) {
        e4_extent_t x=in.map->entries[i];assert(x.logical==at && !x.unwritten);
        for (unsigned n=0;n<x.len;n++) f.map[at++]=x.physical+n;
    }
    assert(at==count);f.source=(jbd2_source_t){.block_size=f.bs,.filesystem_blocks=fs->blocks,.journal_blocks=count,.journal_map=f.map};
    memcpy(f.source.uuid,initial+1128,16);
    unsigned g=(f.fileino-1)/fs->ipg,index=(f.fileino-1)%fs->ipg;
    uint64_t where=(uint64_t)e4_u32(fs->gd[g].raw+8)*fs->bs+index*256;
    f.inodeblock=(unsigned)(where/fs->bs);f.offset=(unsigned)(where%fs->bs);
    memcpy(f.inode,initial+(uint64_t)f.inodeblock*f.bs,f.bs);
    e4_p16(f.inode+f.offset,0x8180); /* regular file, owner read/write */
    e4_inode_sum(fs,f.fileino,f.inode+f.offset);
    memset(f.escaped,'A',f.bs);j_put(f.escaped,J_MAGIC);memset(f.ordered,'C',f.bs);
    e4_discard(fs);assert(!live);return f;
}
static jbd2_writer_t *stage(block_dev_t *dev,fixture_t *f) {
    jbd2_writer_t *w=NULL;assert(!jbd2_writer_open(dev,&f->source,true,&w));
    assert(!jbd2_writer_begin(w,(jbd2_credits_t){2,1,1}));
    assert(!jbd2_writer_metadata(w,f->inodeblock,f->inode));
    assert(!jbd2_writer_metadata(w,f->targets[0],f->escaped));
    assert(!jbd2_writer_data(w,f->targets[1],f->ordered));
    assert(!jbd2_writer_revoke(w,f->targets[2]));return w;
}
static void compare(fixture_t *f,const uint8_t *actual,const uint8_t *initial,bool committed) {
    assert(!memcmp(actual+(uint64_t)f->inodeblock*f->bs,committed ? f->inode : initial+(uint64_t)f->inodeblock*f->bs,f->bs));
    assert(!memcmp(actual+(uint64_t)f->targets[0]*f->bs,committed ? f->escaped : initial+(uint64_t)f->targets[0]*f->bs,f->bs));
    if (committed) assert(!memcmp(actual+(uint64_t)f->targets[1]*f->bs,f->ordered,f->bs));
    assert(!memcmp(actual+(uint64_t)f->targets[2]*f->bs,initial+(uint64_t)f->targets[2]*f->bs,f->bs));
}
static void recover(block_dev_t *dev,fixture_t *f,const uint8_t *initial,bool must_commit) {
    ext4_fault_disk_t *d=dev->priv;jbd2_plan_t *p=NULL;jbd2_report_t report;
    /* Public filesystem bootstrap, not the writer's in-memory map/plan. */
    assert(!ext4_journal_analyze(dev,&p,&report));
    bool committed=report.transactions!=0;
    /* After tail retirement there is no log: derive the already-checkpointed
     * state from exact bytes, and require the entire same old/new image set. */
    if (!j_be(d->stable+(uint64_t)f->map[0]*f->bs+28))
        committed=!memcmp(d->stable+(uint64_t)f->targets[0]*f->bs,f->escaped,f->bs);
    if (must_commit) assert(committed);
    assert(!jbd2_replay(p,true));jbd2_release(p);compare(f,d->stable,initial,committed);
    assert(!ext4_journal_analyze(dev,&p,&report) && !report.transactions);
    size_t events=d->events;assert(!jbd2_replay(p,true) && events==d->events);jbd2_release(p);assert(!live);
}
int main(int argc,char **argv) {
    assert(argc==7);size_t n,cn;uint8_t *initial=load(argv[1],&n),*config=load(argv[2],&cn);assert(cn==20);
    unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);fixture_t f=fixture(&dev,initial,config);jbd2_writer_t *w=NULL;
    assert(jbd2_writer_open(&dev,&f.source,false,&w)==-VFS_EROFS && !w && !d.events);
    assert(!jbd2_writer_open(&dev,&f.source,true,&w));
    assert(jbd2_writer_begin(w,(jbd2_credits_t){65,0,0})==-VFS_EFBIG);
    assert(jbd2_writer_begin(w,(jbd2_credits_t){0,1,0})==-VFS_EINVAL);
    assert(!jbd2_writer_begin(w,(jbd2_credits_t){1,1,1}));
    assert(jbd2_writer_begin(w,(jbd2_credits_t){1,0,0})==-VFS_EAGAIN);
    assert(jbd2_writer_metadata(w,f.map[0],f.inode)==-VFS_EINVAL);
    assert(jbd2_writer_metadata(w,f.source.filesystem_blocks,f.inode)==-VFS_EINVAL);
    assert(!jbd2_writer_metadata(w,f.inodeblock,f.inode));
    assert(!jbd2_writer_metadata(w,f.inodeblock,f.inode));
    assert(jbd2_writer_metadata(w,f.targets[0],f.escaped)==-VFS_EFBIG);
    assert(jbd2_writer_data(w,f.inodeblock,f.ordered)==-VFS_EINVAL);
    assert(jbd2_writer_revoke(w,f.inodeblock)==-VFS_EINVAL);
    assert(!jbd2_writer_revoke(w,f.targets[2]) && !jbd2_writer_revoke(w,f.targets[2]));
    assert(!jbd2_writer_abort(w) && !d.events && !memcmp(initial,d.volatile_bytes,n));
    jbd2_writer_close(w);assert(!live);
    /* Journal-full credit admission with a validated four-block core map. */
    reset(&d,initial);jbd2_source_t small=f.source;small.journal_blocks=4;
    uint8_t *sb=d.volatile_bytes+(uint64_t)f.map[0]*f.bs;
    j_put(sb+16,4);j_put(sb+88,1);jw_seal(sb,1024,252,UINT32_MAX);
    assert(!jbd2_writer_open(&dev,&small,true,&w));
    assert(jbd2_writer_begin(w,(jbd2_credits_t){1,0,1})==-VFS_ENOSPC && !d.events);
    jbd2_writer_close(w);
    reset(&d,initial);w=stage(&dev,&f);size_t stage_allocs=alloc_calls,stage_reads=reads;
    size_t base_live=live;fail_alloc=0;
    assert(jbd2_writer_metadata(w,f.targets[0],f.escaped)==0 && live==base_live);fail_alloc=-1;
    d.trace=true;clock_t started=clock();assert(!jbd2_writer_commit(w));
    size_t commit_events=d.events;compare(&f,d.stable,initial,false);
    assert(jbd2_writer_state(w)==JBD2_WRITE_DURABLE);
    assert(jbd2_writer_begin(w,(jbd2_credits_t){1,0,0})==-VFS_EAGAIN);
    assert(jbd2_writer_abort(w)==-VFS_EINVAL);
    save(argv[4],d.stable,n);
    jbd2_writer_t *second=NULL;assert(jbd2_writer_open(&dev,&f.source,true,&second)==-VFS_ENOSPC && !second);
    assert(!jbd2_writer_checkpoint(w));double ms=(double)(clock()-started)*1000/CLOCKS_PER_SEC;
    d.trace=false;size_t events=d.events,flushes=d.flushes;
    compare(&f,d.stable,initial,true);save(argv[5],d.stable,n);
    assert(jbd2_writer_state(w)==JBD2_WRITE_IDLE);jbd2_writer_close(w);assert(!live);
    for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
        reset(&d,initial);d.persistence=mode;w=stage(&dev,&f);d.cut=(long)cut;d.after=after;
        int r=jbd2_writer_commit(w);bool durable=!r;
        if (!r) r=jbd2_writer_checkpoint(w);
        assert(r==-VFS_EIO && jbd2_writer_state(w)==JBD2_WRITE_FAILED);
        size_t stopped=d.events;assert(jbd2_writer_commit(w)==-VFS_EIO && jbd2_writer_checkpoint(w)==-VFS_EIO && d.events==stopped);
        jbd2_writer_close(w);ext4_fault_restart(&d);recover(&dev,&f,initial,durable);
    }
    /* Every staging allocation/read fails without mutation, with clean close. */
    for (size_t cut=0;cut<stage_allocs;cut++) {
        reset(&d,initial);fail_alloc=(long)cut;int r=jbd2_writer_open(&dev,&f.source,true,&w);
        if (!r) { assert(!jbd2_writer_begin(w,(jbd2_credits_t){2,1,1}));
            r=jbd2_writer_metadata(w,f.inodeblock,f.inode);
            if (!r) r=jbd2_writer_metadata(w,f.targets[0],f.escaped);
            if (!r) r=jbd2_writer_data(w,f.targets[1],f.ordered);
        }
        assert(r==-VFS_ENOMEM && !d.events);jbd2_writer_close(w);assert(!live);
    }
    for (size_t cut=0;cut<stage_reads;cut++) {
        reset(&d,initial);fail_read=(long)cut;
        assert(jbd2_writer_open(&dev,&f.source,true,&w)==-VFS_EIO && !w && !d.events && !live);
    }
    /* Read failures in 4096-sector read/modify/write are uncertain I/O too. */
    reset(&d,initial);w=stage(&dev,&f);reads=0;assert(!jbd2_writer_commit(w) && !jbd2_writer_checkpoint(w));size_t write_reads=reads;jbd2_writer_close(w);
    for (size_t cut=0;cut<write_reads;cut++) {
        reset(&d,initial);w=stage(&dev,&f);fail_read=(long)cut;
        int r=jbd2_writer_commit(w);if (!r) r=jbd2_writer_checkpoint(w);
        assert(r==-VFS_EIO);jbd2_writer_close(w);fail_read=-1;ext4_fault_restart(&d);recover(&dev,&f,initial,false);
    }
    for (size_t cut=0;cut<events;cut++) for (unsigned tear=1;tear<=ss/2;tear*=ss/2) {
        reset(&d,initial);w=stage(&dev,&f);d.cut=(long)cut;d.after=false;d.tear_bytes=tear;
        int r=jbd2_writer_commit(w);if (!r) r=jbd2_writer_checkpoint(w);
        assert(r==-VFS_EIO);jbd2_writer_close(w);ext4_fault_restart(&d);
        jbd2_plan_t *p=NULL;jbd2_report_t report;r=ext4_journal_analyze(&dev,&p,&report);
        if (!r) {
            bool committed=report.transactions!=0;
            if (!j_be(d.stable+(uint64_t)f.map[0]*f.bs+28))
                committed=!memcmp(d.stable+(uint64_t)f.targets[0]*f.bs,f.escaped,f.bs);
            assert(!jbd2_replay(p,true));jbd2_release(p);compare(&f,d.stable,initial,committed);
        } else assert((r==-VFS_EIO || r==-VFS_EOPNOTSUPP) && !p && !d.events && !live);
    }
    /* Maximum credit codec/replay gate: these generic home blocks are not
     * allocation tests or production filesystem metadata transactions. */
    reset(&d,initial);assert(!jbd2_writer_open(&dev,&f.source,true,&w));
    assert(!jbd2_writer_begin(w,(jbd2_credits_t){64,64,64}));
    unsigned homes[192],at=0;
    for (unsigned candidate=f.source.filesystem_blocks-256;at<192;candidate++) {
        assert(candidate<f.source.filesystem_blocks);
        if (j_target(w->io,candidate)) homes[at++]=candidate;
    }
    for (unsigned i=0;i<64;i++) {
        assert(!jbd2_writer_metadata(w,homes[i],f.escaped));
        assert(!jbd2_writer_data(w,homes[i+64],f.ordered));
        assert(!jbd2_writer_revoke(w,homes[i+128]));
    }
    assert(!jbd2_writer_commit(w));jbd2_writer_close(w);
    jbd2_plan_t *maximum=NULL;jbd2_report_t maximum_report;
    assert(!jbd2_analyze(&dev,&f.source,&maximum,&maximum_report));
    assert(maximum_report.images==64 && maximum_report.revokes==64 && maximum_report.transactions==1);
    assert(!jbd2_replay(maximum,true));jbd2_release(maximum);
    for (unsigned i=0;i<64;i++) {
        assert(!memcmp(d.stable+(uint64_t)homes[i]*f.bs,f.escaped,f.bs));
        assert(!memcmp(d.stable+(uint64_t)homes[i+64]*f.bs,f.ordered,f.bs));
        assert(!memcmp(d.stable+(uint64_t)homes[i+128]*f.bs,initial+(uint64_t)homes[i+128]*f.bs,f.bs));
    }
    /* Revoke-only and consecutive reuse advance head/sequence only after
     * checkpoint. Initial wrap fixtures exercise UINT32_MAX -> 0 -> 1. */
    reset(&d,initial);assert(!jbd2_writer_open(&dev,&f.source,true,&w));
    uint32_t first_sequence=w->sequence;
    for (unsigned i=0;i<3;i++) {
        assert(!jbd2_writer_begin(w,(jbd2_credits_t){0,0,1}));
        assert(!jbd2_writer_revoke(w,f.targets[2]));assert(!jbd2_writer_commit(w));
        assert(!jbd2_writer_checkpoint(w));assert(w->sequence==first_sequence+i+1);
    }
    jbd2_writer_close(w);
    /* A durable commit survives close with no implicit checkpoint. */
    reset(&d,initial);w=stage(&dev,&f);assert(!jbd2_writer_commit(w));jbd2_writer_close(w);
    ext4_fault_restart(&d);recover(&dev,&f,initial,true);save(argv[6],d.stable,n);
    printf("PASS writer bs=%u sector=%u: %zu events (%zu commit), %zu barriers, %.3f host CPU ms; %zu atomic crash cuts, tears, allocation/read unwind, max credits, revoke-only reuse, ordering/abort/idempotence\n",f.bs,ss,events,commit_events,flushes,ms,events*8);
    free(f.map);free(d.stable);free(d.volatile_bytes);free(initial);free(config);assert(!live);return 0;
}
