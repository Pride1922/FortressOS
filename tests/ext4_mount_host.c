/* Actual VFS/EXT4/JBD2, pthread exclusion and sector-atomic power-loss model.
 * No hardware IRQ, scheduler or physical-device acceptance claim. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#undef WNOHANG
#undef WUNTRACED
#undef WCONTINUED
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#undef WIFSTOPPED
#undef WSTOPSIG
#undef WIFCONTINUED
#include "ext4_host/spinlock.h"
#define HOST_SPINLOCK_H
#include "ext4_fault_disk.h"
static size_t live,reads,allocations;
static long fail_read=-1,fail_alloc=-1;
void *kmalloc(size_t n) { allocations++;if (!fail_alloc) return NULL;if (fail_alloc>0) fail_alloc--;void *p=malloc(n);if (p) live++;return p; }
void *kcalloc(size_t n,size_t z) { if (n && z>SIZE_MAX/n) return NULL;void *p=kmalloc(n*z);if (p) memset(p,0,n*z);return p; }
void kfree(void *p) { if (p) { assert(live);live--;free(p); } }
void serial_puts(const char *s) { (void)s; }
void serial_raw_putc(char c) { (void)c; }
void console_terminal_write(const char *s,size_t n) { (void)s;(void)n; }
void console_inc_generation(void) {}
bool console_is_quiet(void) { return false; }
int64_t input_read(void *p,size_t n) { (void)p;(void)n;return 0; }
bool block_read_sector(block_dev_t *dev,uint64_t lba,void *bytes) {
#ifdef TEST_PERM_DEVICE_IO
    extern void permissions_device_lock_observed(void);
    permissions_device_lock_observed();
#endif
    reads++;if (!fail_read) return false;if (fail_read>0) fail_read--;return dev->read_sector(dev,lba,bytes);
}
bool block_write_sector(block_dev_t *dev,uint64_t lba,const void *bytes) { return dev->write_sector(dev,lba,bytes); }
bool block_flush(block_dev_t *dev) { return dev->flush(dev); }
#include "../src/fs/gpt.h"
size_t gpt_get_partition_count(void) { return 0; }
gpt_partition_t *gpt_get_partition(size_t i) { (void)i;return NULL; }
#ifndef TEST_PERM_DEVICE_IO
bool ext2_device_read_sector(block_dev_t *d,uint64_t l,void *b) { (void)d;(void)l;(void)b;assert(0);return false; }
#endif
#include "../src/fs/runfs.c"
#include "../src/fs/devfs.c"
#include "../src/fs/vfs.c"
#include "../src/fs/ext4.c"
#include "../src/fs/jbd2.c"
static const ext4_journal_admission_t admitted={true,true,true};
static void teardown(void) {
    if (e4_active) {
        ext4_engine_t *e=e4_active->engine;e4_active->journal_mounted=false;e4_active=NULL;
        assert(g_vfs_root->children==&e->fs->cached[0]->node);
        g_vfs_root->children=g_vfs_root->children->next;ext4_engine_close(e);
    }
    /* VFS may install unrelated empty in-memory root directories. */
    /* Static boot mounts are reset by the next boot-only vfs_init. */
    if (g_vfs_root) g_vfs_root->children=NULL;
    kfree(g_vfs_root);g_vfs_root=NULL;assert(!live && !e4_engine_busy);
    fail_read=fail_alloc=-1;
}
static void reset(ext4_fault_disk_t *d,const uint8_t *initial) {
    teardown();memcpy(d->stable,initial,d->bytes);ext4_fault_restart(d);d->persistence=0;
    vfs_init();reads=allocations=0;
}
static uint8_t *load(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb");assert(f && !fseek(f,0,SEEK_END));*n=(size_t)ftell(f);rewind(f);
    uint8_t *p=malloc(*n);assert(p && fread(p,1,*n,f)==*n);fclose(f);return p;
}
static void save(const char *prefix,const char *suffix,const uint8_t *p,size_t n) {
    char path[1024];snprintf(path,sizeof(path),"%s-%s.img",prefix,suffix);
    FILE *f=fopen(path,"wbx");assert(f && fwrite(p,1,n,f)==n);fclose(f);
}
static void dirty_check(ext4_mount_t *fs) {
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,1024));
    assert(e4_u16(sb+58)==0 && e4_u32(sb+96)==0x46 && e4_u32(sb+1020)==e4_crc(UINT32_MAX,sb,1020));
}
static void clean_check(ext4_mount_t *fs) {
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,1024));
    assert(e4_u16(sb+58)==1 && e4_u32(sb+96)==0x42 && !e4_u32(sb+232));
    assert(e4_u32(sb+1020)==e4_crc(UINT32_MAX,sb,1020) && !fs->engine->dirty);
    assert(!j_be(fs->engine->writer->io->super+28) && jbd2_writer_state(fs->engine->writer)==JBD2_WRITE_IDLE);
}
static void bytes_check(file_t *file,uint8_t value,size_t n) {
    uint8_t b[4096];file->offset=0;
    while (n) { size_t take=n>sizeof(b) ? sizeof(b) : n;assert(vfs_read(file,b,take)==(int64_t)take);
        for (size_t k=0;k<take;k++) assert(b[k]==value);
        n-=take; }
    assert(!vfs_read(file,b,1));
}
static void lifecycle(block_dev_t *dev,const uint8_t *initial,unsigned bs,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));dirty_check(fs);
    size_t stop=d->events;assert(ext4_mount_rw(dev,"/mnt",&fs)==-VFS_EEXIST || !fs);assert(d->events==stop);
    fs=e4_active;assert(!ext4_sync_journal_fixture() && !fs->frozen);dirty_check(fs);
    file_t *a=vfs_open("/mnt/target.bin",VFS_O_RDWR|VFS_O_APPEND),*b=vfs_open("/mnt/target.bin",VFS_O_RDWR);assert(a && b);
    a->ref_count++;assert(((e4_node_t *)a->node)->opens==2);
    assert(!vfs_unlink("/mnt/target.bin") && !vfs_lookup("/mnt/target.bin"));bytes_check(b,'O',3*bs);
    uint8_t data[4096];memset(data,'N',bs);assert(vfs_write(a,data,bs)==(int64_t)bs);
    assert(!vfs_truncate(b->node,bs+17));assert(!ext4_sync(fs) && !fs->frozen && fs->engine->orphan_count==1);
    assert(vfs_close(a)==0);assert(((e4_node_t *)b->node)->opens==2);
    assert(!vfs_close(b));assert(!vfs_close(a));assert(!fs->engine->orphan_count);
    file_t *fresh=vfs_open("/mnt/persist.bin",VFS_O_CREAT|VFS_O_RDWR);assert(fresh);
    memset(data,'P',bs);assert(vfs_write(fresh,data,bs)==(int64_t)bs);assert(!vfs_close(fresh));
    assert(!vfs_mkdir("/mnt/sub",0));assert(!vfs_rename("/mnt/persist.bin","/mnt/sub/persist.bin"));
    assert(!ext4_sync(fs));dirty_check(fs);
    file_t *late=vfs_open("/mnt/sub/persist.bin",VFS_O_WRONLY|VFS_O_APPEND);assert(late);
    assert(vfs_write(late,data,17)==17);assert(!vfs_close(late));assert(!ext4_freeze_journal_fixture());clean_check(fs);
    stop=d->events;assert(vfs_open("/mnt/new.bin",VFS_O_CREAT|VFS_O_WRONLY)==NULL);assert(d->events==stop);
    fresh=vfs_open("/mnt/sub/persist.bin",VFS_O_RDONLY);assert(fresh);bytes_check(fresh,'P',bs+17);assert(!vfs_close(fresh));
    save(prefix,"lifecycle-clean",d->stable,d->bytes);teardown();ext4_fault_restart(d);vfs_init();
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));dirty_check(fs);
    fresh=vfs_open("/mnt/sub/persist.bin",VFS_O_RDONLY);assert(fresh);bytes_check(fresh,'P',bs+17);assert(!vfs_close(fresh));
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    /* Freeze cannot claim clean while an unlinked independent open survives.
     * Closing after freeze changes pins only; a later drain completes cleanup. */
    reset(d,initial);assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
    a=vfs_open("/mnt/target.bin",VFS_O_RDONLY);assert(a && !vfs_unlink("/mnt/target.bin"));
    assert(ext4_freeze_and_sync(fs)==-VFS_EAGAIN && fs->frozen && !fs->engine->tainted);dirty_check(fs);
    stop=d->events;assert(!vfs_close(a) && d->events==stop);assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    printf("lifecycle PASS: actual VFS dup/independent open-unlink, sync/later-write, frozen pins and clean reboot\n");fflush(stdout);
}
static uint8_t *pending_source(block_dev_t *dev,const uint8_t *initial,bool invalid,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);
    e4_p32(d->volatile_bytes+1120,0x46);e4_p32(d->volatile_bytes+2044,e4_crc(UINT32_MAX,d->volatile_bytes+1024,1020));
    memcpy(d->stable,d->volatile_bytes,d->bytes);ext4_engine_t *e=NULL;
    assert(!ext4_engine_open_journal_orphans(dev,true,&e));e4_inode_t root,in;uint32_t ino;
    assert(!e4_inode(e->fs,2,&root) && e4_scan(&root,"target.bin",0,NULL,&ino)==1);
    assert(!e4_mapping(&root));uint32_t root_block=e4_find(&root,0)->physical;
    uint8_t *raw;assert(!e4_orphan_begin(e) && !e4_load_plan(e,ino,&in,&raw));
    if (!invalid) {
        assert(!e4_orphan_add(e,ino,raw));e4_p16(raw+26,0);e4_inode_sum(e->fs,ino,raw);
        assert(!e4_dir_edit(e,&root,"target.bin",0,0) && !e4_namespace_end(e,0));e4_orphan_added(e,ino);
        assert(!e4_orphan_begin(e) && !e4_orphan_load(e,ino,&in,&raw));
    } else e4_p32(raw+60,root_block);
    e4_p16(raw,0x8180);e4_inode_sum(e->fs,ino,raw);
    uint32_t block=(uint32_t)(((uint64_t)e4_u32(e->fs->gd[(ino-1)/e->fs->ipg].raw+8)*e->fs->bs+
        ((ino-1)%e->fs->ipg)*256)/e->fs->bs);
    const uint8_t *image=NULL;for (unsigned k=0;k<e->images;k++) if (e->image[k].block==block) image=e->image[k].bytes;
    assert(image && !jbd2_writer_begin(e->writer,(jbd2_credits_t){1,0,0}));
    assert(!jbd2_writer_metadata(e->writer,block,image) && !jbd2_writer_commit(e->writer));
    /* Fixture producer deliberately leaves a durable, uncheckpointed log. */
    ext4_engine_close(e);uint8_t *source=malloc(d->bytes);assert(source);memcpy(source,d->stable,d->bytes);
    save(prefix,invalid ? "preview-rejected" : "pending-seed",source,d->bytes);return source;
}
static void admission(block_dev_t *dev,const uint8_t *initial,const uint8_t *bad) {
    ext4_fault_disk_t *d=dev->priv;ext4_mount_t *fs=NULL;const char *stage=NULL;
    reset(d,initial);
    assert(ext4_mount_journal_fixture_diagnose(dev,"/bad",admitted,&fs,&stage)==-VFS_EINVAL);
    assert(!strcmp(stage,"arguments") && !fs && !d->events && !reads);
    for (unsigned missing=0;missing<3;missing++) {
        reset(d,initial);ext4_journal_admission_t policy=admitted;
        if (!missing) policy.disposable_fixture=false;else if (missing==1) policy.writable=false;else policy.recovery=false;
        assert(ext4_mount_journal_fixture_diagnose(dev,"/mnt",policy,&fs,&stage)==-VFS_EROFS && !fs && !d->events && !reads);
        assert(!strcmp(stage,"writable-admission"));
    }
    reset(d,initial);assert(ext4_mount_rw(dev,"/mnt",&fs)==-VFS_EOPNOTSUPP && !fs && !d->events);
    assert(ext4_mount_ro(dev,"/mnt",&fs)==-VFS_EOPNOTSUPP && !fs && !d->events);
    reset(d,bad);jbd2_plan_t *plan=NULL;jbd2_report_t report;
    assert(!ext4_journal_analyze(dev,&plan,&report) && report.transactions==1);jbd2_release(plan);
    assert(ext4_mount_journal_fixture_diagnose(dev,"/mnt",admitted,&fs,&stage)<0 && !fs && !d->events && !vfs_lookup("/mnt"));
    assert(!strcmp(stage,"recovery-preview"));
    assert(!e4_engine_busy);teardown();
    reset(d,initial);fail_read=0;
    assert(ext4_mount_journal_fixture_diagnose(dev,"/mnt",admitted,&fs,&stage)==-VFS_EIO);
    assert(!strcmp(stage,"journal-source") && !fs && !d->events && !e4_engine_busy && !vfs_lookup("/mnt"));
    fail_read=-1;teardown();reset(d,initial);
    assert(!ext4_mount_journal_fixture_diagnose(dev,"/mnt",admitted,&fs,&stage));
    assert(!strcmp(stage,"mounted") && fs);
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);teardown();
    puts("admission PASS: explicit policy/production exclusion, valid journal with invalid replayed ownership rejects before writes");fflush(stdout);
}
static size_t mount_faults(block_dev_t *dev,const uint8_t *source,bool deleted,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;ext4_mount_t *fs=NULL;reset(d,source);
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));size_t events=d->events,rd=reads,al=allocations;
    assert((vfs_lookup("/mnt/target.bin")==NULL)==deleted && !fs->engine->orphan_count);dirty_check(fs);
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    save(prefix,deleted ? "recovery-clean" : "activation-clean",d->stable,d->bytes);
    for (unsigned persistence=0;persistence<4;persistence++) for (unsigned after=0;after<2;after++)
        for (size_t cut=0;cut<events;cut++) {
            reset(d,source);d->cut=(long)cut;d->after=after!=0;d->persistence=persistence;
            assert(ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs)==-VFS_EIO && !fs && !e4_active && !vfs_lookup("/mnt"));
            assert(!e4_engine_busy);teardown();ext4_fault_restart(d);vfs_init();
            assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
            assert(!fs->engine->orphan_count && (vfs_lookup("/mnt/target.bin")==NULL)==deleted);
            assert(!ext4_freeze_and_sync(fs));clean_check(fs);
        }
    for (unsigned kind=0;kind<2;kind++) for (size_t at=0;at<(kind ? rd : al);at++) {
        reset(d,source);if (kind) fail_read=(long)at;else fail_alloc=(long)at;
        assert(ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs)<0 && !fs && !e4_active && !vfs_lookup("/mnt") && !e4_engine_busy);
        fail_read=fail_alloc=-1;teardown();ext4_fault_restart(d);vfs_init();
        assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    }
    printf("mount profile=%u PASS events=%zu cuts=%zu read=%zu allocation=%zu; no failed publication\n",deleted,events,events*8,rd,al);fflush(stdout);
    return events*8;
}
static size_t freeze_faults(block_dev_t *dev,const uint8_t *initial,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
    file_t *file=vfs_open("/mnt/target.bin",VFS_O_WRONLY|VFS_O_APPEND);assert(file);
    uint8_t data[4096];memset(data,'F',fs->bs);assert(vfs_write(file,data,fs->bs)==(int64_t)fs->bs);assert(!vfs_close(file));
    assert(!ext4_sync(fs));uint8_t *dirty=malloc(d->bytes);assert(dirty);memcpy(dirty,d->stable,d->bytes);
    d->events=d->writes=d->flushes=0;e4_cache_drop(fs,false);reads=allocations=0;
    assert(!ext4_freeze_and_sync(fs));size_t events=d->events,rd=reads,al=allocations;clean_check(fs);
    for (unsigned persistence=0;persistence<4;persistence++) for (unsigned after=0;after<2;after++)
        for (size_t cut=0;cut<events;cut++) {
            reset(d,dirty);assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
            d->events=d->writes=d->flushes=0;d->cut=(long)cut;d->after=after!=0;d->persistence=persistence;
            assert(ext4_freeze_and_sync(fs)==-VFS_EIO && fs->frozen && fs->engine->tainted);
            size_t stop=d->events;assert(ext4_sync(fs)==-VFS_EIO && ext4_freeze_and_sync(fs)==-VFS_EIO && d->events==stop);
            if (persistence==1 && !after && cut==events/2) save(prefix,"freeze-pending",d->stable,d->bytes);
            teardown();ext4_fault_restart(d);vfs_init();assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
            assert(vfs_lookup("/mnt/target.bin")->size==4*fs->bs);
            assert(!ext4_freeze_and_sync(fs));clean_check(fs);
        }
    save(prefix,"freeze-recovered",d->stable,d->bytes);
    for (unsigned kind=0;kind<2;kind++) for (size_t at=0;at<(kind ? rd : al);at++) {
        reset(d,dirty);assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));e4_cache_drop(fs,false);
        d->events=d->writes=d->flushes=0;if (kind) fail_read=(long)at;else fail_alloc=(long)at;
        assert(ext4_freeze_and_sync(fs)<0 && fs->frozen);
        fail_read=fail_alloc=-1;
        if (fs->engine->tainted) {
            /* Sub-sector metadata publication needs device RMW reads too.
             * Their failure is an uncertain I/O boundary, not staging OOM. */
            assert(kind);size_t stop=d->events;
            assert(ext4_freeze_and_sync(fs)==-VFS_EIO && d->events==stop);
            teardown();ext4_fault_restart(d);vfs_init();
            assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
        } else { assert(!d->writes);dirty_check(fs); }
        assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    }
    reset(d,dirty);assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));d->events=0;d->cut=0;
    assert(ext4_sync(fs)==-VFS_EIO && fs->engine->tainted && !fs->frozen);
    size_t stop=d->events;assert(ext4_sync(fs)==-VFS_EIO && ext4_freeze_and_sync(fs)==-VFS_EIO && d->events==stop);
    free(dirty);printf("freeze PASS events=%zu cuts=%zu staging reads=%zu allocations=%zu; permanent flush taint\n",events,events*8,rd,al);fflush(stdout);
    return events*8;
}
typedef struct { pthread_barrier_t *barrier;ext4_mount_t *fs;file_t *file;int64_t result; } race_arg_t;
static void cache_check(vfs_node_t *node) {
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t current;
    e4_inode_t *cached=node->fs_private;assert(!e4_inode(cached->fs,cached->ino,&current));
    assert(node->size==current.size && cached->size==current.size &&
        cached->generation==current.generation && !memcmp(cached->extent,current.extent,60));
    spin_unlock_irqrestore(&e4_lock,flags);
}
static void directory_cache(block_dev_t *dev,const uint8_t *initial,const char *prefix) {
    ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_mount_t *fs=NULL;
    assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
    assert(!vfs_mkdir("/mnt/growth",0));vfs_node_t *directory=vfs_lookup("/mnt/growth");assert(directory);
    cache_check(vfs_lookup("/mnt"));
    char name[61],path[96];memset(name,'x',60);name[60]=0;
    unsigned count=(fs->bs-36)/68; /* dot entries + checksum tail; 60-byte names. */
    for (unsigned k=0;k<count;k++) {
        name[0]='f';name[1]=(char)('0'+k/100);name[2]=(char)('0'+k/10%10);name[3]=(char)('0'+k%10);
        snprintf(path,sizeof(path),"/mnt/growth/%s",name);assert(vfs_create(path,VFS_FILE));cache_check(directory);
    }
    assert(directory->size==fs->bs && vfs_create("/mnt/move.bin",VFS_FILE));
    name[0]='r';snprintf(path,sizeof(path),"/mnt/growth/%s",name);
    assert(!vfs_rename("/mnt/move.bin",path));
    assert(directory->size==2*fs->bs);cache_check(directory);cache_check(vfs_lookup("/mnt"));
    assert(!vfs_mkdir("/mnt/empty",0));cache_check(vfs_lookup("/mnt"));
    assert(!vfs_unlink("/mnt/empty"));cache_check(vfs_lookup("/mnt"));
    assert(!ext4_freeze_and_sync(fs));clean_check(fs);save(prefix,"directory-cache-clean",d->stable,d->bytes);
    puts("directory cache PASS: create/parent mappings and rename growth immediately match committed inode");fflush(stdout);
}
static void *race_write(void *opaque) {
    race_arg_t *arg=opaque;uint8_t bytes[4096];memset(bytes,'R',arg->fs->bs);
    int r=pthread_barrier_wait(arg->barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    arg->result=vfs_write(arg->file,bytes,arg->fs->bs);return NULL;
}
static void *race_freeze(void *opaque) {
    race_arg_t *arg=opaque;int r=pthread_barrier_wait(arg->barrier);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
    arg->result=ext4_freeze_and_sync(arg->fs);return NULL;
}
static void concurrency(block_dev_t *dev,const uint8_t *initial) {
    for (unsigned round=0;round<8;round++) {
        ext4_fault_disk_t *d=dev->priv;reset(d,initial);ext4_mount_t *fs=NULL;
        assert(!ext4_mount_journal_fixture(dev,"/mnt",admitted,&fs));
        file_t *file=vfs_open("/mnt/target.bin",VFS_O_RDWR|VFS_O_APPEND);assert(file);
        pthread_barrier_t barrier;assert(!pthread_barrier_init(&barrier,NULL,2));
        race_arg_t writer={&barrier,fs,file,0},freezer={&barrier,fs,NULL,0};pthread_t a,b;
        assert(!pthread_create(&a,NULL,race_write,&writer) && !pthread_create(&b,NULL,race_freeze,&freezer));
        assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_barrier_destroy(&barrier));
        assert(!freezer.result && (writer.result==(int64_t)fs->bs || writer.result==-VFS_EROFS));clean_check(fs);
        size_t stop=d->events;uint8_t byte=0;assert(vfs_write(file,&byte,1)==-VFS_EROFS && d->events==stop);
        assert(file->node->size==(writer.result>0 ? 4u : 3u)*fs->bs);assert(!vfs_close(file));
    }
    puts("concurrency PASS 8 pthread write/freeze races: ranked exclusion, complete durability or zero publication after freeze");fflush(stdout);
}
int main(int argc,char **argv) {
    assert(argc==4 || (argc==5 && !strcmp(argv[4],"--smoke")));
    size_t n;uint8_t *initial=load(argv[1],&n);unsigned ss=(unsigned)strtoul(argv[2],NULL,10);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
    block_dev_t dev={.sector_size=ss,.sector_count=n/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    unsigned bs=1024u<<e4_u32(initial+1048);lifecycle(&dev,initial,bs,argv[3]);
    uint8_t *pending=pending_source(&dev,initial,false,argv[3]),*bad=pending_source(&dev,initial,true,argv[3]);
    admission(&dev,initial,bad);size_t cuts=0;
    if (argc==4) {
        cuts=mount_faults(&dev,initial,false,argv[3]);
        cuts+=mount_faults(&dev,pending,true,argv[3]);cuts+=freeze_faults(&dev,initial,argv[3]);
    } else {
        reset(&d,pending);ext4_mount_t *fs=NULL;
        assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs) && !vfs_lookup("/mnt/target.bin"));
        assert(!ext4_freeze_and_sync(fs));clean_check(fs);
    }
    if (argc==5) directory_cache(&dev,initial,argv[3]);
    concurrency(&dev,initial);
    teardown();free(pending);free(bad);free(initial);free(d.stable);free(d.volatile_bytes);
    printf("EXT4 mount host PASS block=%u sector=%u cuts=%zu\n",bs,ss,cuts);return 0;
}
