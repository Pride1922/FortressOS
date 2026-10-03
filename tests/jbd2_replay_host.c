#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void reset(ext4_fault_disk_t *d,const uint8_t *initial) {
    memcpy(d->stable,initial,d->bytes);ext4_fault_restart(d);fail_alloc=fail_read=-1;reads=alloc_calls=0;d->persistence=0;assert(!live);
}
static void check(jbd2_plan_t *p,const uint8_t *disk,const uint8_t *oracle) {
    for (unsigned i=0;i<p->images;i++) {
        unsigned b=p->image[i].block;
        assert(!memcmp(disk+(uint64_t)b*p->bs,oracle+(uint64_t)b*p->bs,p->bs));
    }
}
int main(int argc,char **argv) {
    assert(argc==5);size_t n,on;uint8_t *initial=load(argv[1],&n),*oracle=NULL;
    bool reject=!strcmp(argv[2],"reject");
    if (!reject) { oracle=load(argv[2],&on);assert(n==on); }
    unsigned ss=(unsigned)strtoul(argv[4],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,
        .write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    reset(&d,initial);jbd2_plan_t *p=NULL;jbd2_report_t report;
    ext4_mount_t *ordinary=NULL;
    assert(ext4_mount_ro(&dev,"/journal-test",&ordinary)<0 && !ordinary && !live && !d.events);
    reset(&d,initial);
    int r=ext4_journal_analyze(&dev,&p,&report);
    if (reject) {
        assert(r<0 && !p && !d.events && !live && !memcmp(initial,d.volatile_bytes,n));
        printf("PASS zero-write rejection %s error=%d\n",argv[1],r);
        free(d.stable);free(d.volatile_bytes);free(initial);return 0;
    }
    if (r) fprintf(stderr,"analysis %s sector=%u error=%d\n",argv[1],ss,r);
    assert(!r && p && !d.events && !memcmp(initial,d.volatile_bytes,n));
    printf("JBD2 analyzed: tx=%u images=%u revokes=%u skipped=%u tail=%u next=%u\n",report.transactions,report.images,report.revokes,report.revoked,report.incomplete_tail,report.next_sequence);
    size_t analyze_reads=reads,allocations=alloc_calls;
    assert(jbd2_replay(p,false)==-VFS_EROFS && !d.events);
    d.trace=true;assert(!jbd2_replay(p,true));d.trace=false;size_t events=d.events;check(p,d.stable,oracle);
    size_t before=d.events;assert(!jbd2_replay(p,true) && before==d.events);
    jbd2_release(p);assert(!live);
    /* Fresh runtime state and only stable bytes after each cut. */
    for (unsigned mode=0;mode<4;mode++) for (unsigned after=0;after<2;after++) for (size_t cut=0;cut<events;cut++) {
        reset(&d,initial);d.persistence=mode;assert(!ext4_journal_analyze(&dev,&p,NULL));
        d.cut=(long)cut;d.after=after;assert(jbd2_replay(p,true)==-VFS_EIO);
        before=d.events;assert(jbd2_replay(p,true)==-VFS_EIO && before==d.events);jbd2_release(p);
        ext4_fault_restart(&d);assert(!ext4_journal_analyze(&dev,&p,NULL));assert(!jbd2_replay(p,true));
        /* Compare all replayed home blocks from the original plan's oracle. */
        jbd2_release(p);ext4_fault_disk_t saved=d;
        d.volatile_bytes=(uint8_t *)initial;d.offline=false;
        assert(!ext4_journal_analyze(&dev,&p,NULL));check(p,saved.stable,oracle);jbd2_release(p);d=saved;
    }
    /* Every analysis read failure rejects without a write; allocation unwind. */
    for (size_t cut=0;cut<analyze_reads;cut++) {
        reset(&d,initial);fail_read=(long)cut;assert(ext4_journal_analyze(&dev,&p,NULL)<0 && !p && !d.events && !live);
    }
    for (size_t cut=0;cut<allocations;cut++) {
        reset(&d,initial);fail_alloc=(long)cut;
        assert(ext4_journal_analyze(&dev,&p,NULL)==-VFS_ENOMEM && !p && !d.events && !live);
    }
    for (size_t cut=0;cut<events;cut++) for (unsigned tear=1;tear<=ss/2;tear*=ss/2) {
        reset(&d,initial);assert(!ext4_journal_analyze(&dev,&p,NULL));d.cut=(long)cut;d.tear_bytes=tear;
        assert(jbd2_replay(p,true)==-VFS_EIO);jbd2_release(p);ext4_fault_restart(&d);
        r=ext4_journal_analyze(&dev,&p,NULL);
        if (!r) { assert(!jbd2_replay(p,true));jbd2_release(p); }
        else assert(r==-VFS_EIO && !p && !d.events && !live);
    }
    reset(&d,initial);assert(!ext4_journal_analyze(&dev,&p,NULL));assert(!jbd2_replay(p,true));check(p,d.stable,oracle);jbd2_release(p);
    FILE *f=fopen(argv[3],"wbx");assert(f && fwrite(d.stable,1,n,f)==n);fclose(f);
    printf("PASS Linux journal recovery, RO/no-write, %zu replay events x 8 crash patterns, %zu read failures, allocation cleanup, idempotence\n",events,analyze_reads);
    free(d.stable);free(d.volatile_bytes);free(initial);free(oracle);assert(!live);return 0;
}
