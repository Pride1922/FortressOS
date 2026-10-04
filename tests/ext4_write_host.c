/* Actual ext4 and VFS, hosted memory disk and single-thread lock adapter. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "types.h"
#include "ext4_host/spinlock.h"
#define HOST_SPINLOCK_H
static size_t live, reads, writes, flushes;
static size_t read_runs,write_runs;
static uint64_t watched_lo,watched_hi;
static size_t watched_reads;
static long fail_alloc=-1, fail_read=-1, fail_write=-1, fail_flush=-1;
static bool uncertain;
static uint8_t *disk;
static size_t disk_len;
void *kmalloc(size_t n) { if (!fail_alloc) return NULL; if (fail_alloc>0) fail_alloc--; void *p=malloc(n); if (p) live++; return p; }
void *kcalloc(size_t a,size_t b) { if (a && b>SIZE_MAX/a) return NULL; void *p=kmalloc(a*b); if (p) memset(p,0,a*b); return p; }
void kfree(void *p) { if (p) { assert(live);live--;free(p); } }
void serial_puts(const char *s) { (void)s; }
void serial_raw_putc(char c) { (void)c; }
void console_terminal_write(const char *s,size_t n) { (void)s;(void)n; }
void console_inc_generation(void) {}
bool console_is_quiet(void) { return false; }
int64_t input_read(void *p,size_t n) { (void)p;(void)n;return 0; }
#include "../src/fs/vfs.c"
#include "../src/fs/ext4.c"
#include "../src/fs/jbd2.c"
bool block_read_sector(block_dev_t *d,uint64_t lba,void *p) {
    assert(lba<d->sector_count);reads++;
    if (lba>=watched_lo && lba<watched_hi) watched_reads++;
    if (!fail_read) return false;
    if (fail_read>0) fail_read--;
    memcpy(p,disk+lba*d->sector_size,d->sector_size);return true;
}
static bool store(block_dev_t *d,uint64_t lba,const void *p) {
    assert(lba<d->sector_count);writes++;
    if (!fail_write) { if (uncertain) memcpy(disk+lba*d->sector_size,p,d->sector_size);return false; }
    if (fail_write>0) fail_write--;
    memcpy(disk+lba*d->sector_size,p,d->sector_size);return true;
}
static bool barrier(block_dev_t *d) { (void)d;flushes++;if (!fail_flush) return false;if (fail_flush>0) fail_flush--;return true; }
bool block_write_sector(block_dev_t *d,uint64_t l,const void *p) { return d->write_sector(d,l,p); }
bool block_flush(block_dev_t *d) { return d->flush(d); }
static bool read_run(block_dev_t *d,uint64_t l,uint32_t n,void *p) {
    assert(n && n<=block_get_max_run_bytes(d)/d->sector_size && l<d->sector_count && n<=d->sector_count-l);read_runs++;
    for (uint32_t i=0;i<n;i++) if (!block_read_sector(d,l+i,(uint8_t *)p+i*d->sector_size)) return false;
    return true;
}
static bool write_run(block_dev_t *d,uint64_t l,uint32_t n,const void *p) {
    assert(n && n<=block_get_max_run_bytes(d)/d->sector_size && l<d->sector_count && n<=d->sector_count-l);write_runs++;
    for (uint32_t i=0;i<n;i++) if (!store(d,l+i,(const uint8_t *)p+i*d->sector_size)) return false;
    return true;
}
static void reset(void) {
    if (e4_active) { e4_discard(e4_active);e4_active=NULL; }
    kfree(g_vfs_root);g_vfs_root=NULL;assert(!live);
    fail_alloc=fail_read=fail_write=fail_flush=-1;reads=writes=flushes=0;uncertain=false;vfs_init();
}
static void save(const char *path) { FILE *f=fopen(path,"wbx");assert(f && fwrite(disk,1,disk_len,f)==disk_len);fclose(f); }
static void check_file(const char *path,const uint8_t *expected,size_t n) {
    file_t *f=vfs_open(path,VFS_O_RDONLY);assert(f);uint8_t b[4096];size_t at=0;
    while (at<n) { size_t take=n-at;if (take>sizeof(b)) take=sizeof(b);int64_t k=vfs_read(f,b,take);assert(k>0 && (size_t)k<=take && !memcmp(b,expected+at,(size_t)k));at+=(size_t)k; }
    assert(vfs_read(f,b,1)==0);vfs_close(f);
}
static void write_bytes(file_t *f,const uint8_t *b,size_t n) {
    while (n) { int64_t k=vfs_write(f,b,n);assert(k>0 && (size_t)k<=n);b+=k;n-=(size_t)k; }
}
static int operation(unsigned kind) {
    int error=0;
    if (kind==0) return vfs_create_ext("/mnt/new.bin",VFS_FILE,&error) ? 0 : error;
    if (kind==1) return vfs_mkdir("/mnt/newdir",0);
    if (kind==2) return vfs_unlink("/mnt/seed.bin");
    if (kind==3) return vfs_rename("/mnt/seed.bin","/mnt/renamed.bin");
    vfs_node_t *n=vfs_lookup("/mnt/seed.bin");if (!n) return -VFS_EIO;
    if (kind==4) return vfs_truncate(n,0);
    uint64_t off=kind==5 ? n->size+37 : 2;
    uint8_t bytes[89];memset(bytes,0x73,sizeof(bytes));
    int64_t k=n->write(n,&off,false,bytes,sizeof(bytes));return k<0 ? (int)k : 0;
}
static void fault_matrix(block_dev_t *dev,const uint8_t *original) {
    for (unsigned kind=0;kind<7;kind++) {
        reset();memcpy(disk,original,disk_len);ext4_mount_t *m;assert(!ext4_mount_rw(dev,"/mnt",&m));
        reads=writes=flushes=0;assert(!operation(kind));size_t rd=reads,wr=writes,fl=flushes;
        /* Exhaust all write/flush events; read failures before mutation must
         * leave bytes intact, later failures must taint. */
        for (unsigned which=0;which<3;which++) {
            size_t total=which==0 ? rd : which==1 ? wr : fl;
            for (size_t cut=0;cut<total;cut++) {
                reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(dev,"/mnt",&m));
                reads=writes=flushes=0;
                if (which==0) fail_read=(long)cut;else if (which==1) fail_write=(long)cut;else fail_flush=(long)cut;
                int r=operation(kind);assert(r<0);
                if (!writes) assert(!memcmp(disk,original,disk_len));
                else assert(m->engine->tainted);
                fail_read=fail_write=fail_flush=-1;
                if (m->engine->tainted) { size_t before=writes;assert(ext4_sync(m)==-VFS_EIO && ext4_freeze_and_sync(m)==-VFS_EIO && writes==before); }
            }
        }
        reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(dev,"/mnt",&m));
        fail_write=0;uncertain=true;assert(operation(kind)<0 && m->engine->tainted);
        printf("PASS fault events operation=%u reads=%zu writes=%zu barriers=%zu\n",kind,rd,wr,fl);
    }
}
typedef struct { file_t *file; unsigned id; } append_arg_t;
static void *append_thread(void *opaque) {
    append_arg_t *arg=opaque;uint8_t record[32];
    for (unsigned i=0;i<100;i++) { memset(record,0x91,sizeof(record));record[0]=(uint8_t)arg->id;record[1]=(uint8_t)i;assert(vfs_write(arg->file,record,sizeof(record))==sizeof(record)); }
    return NULL;
}
static void parallel_append(void) {
    pthread_t threads[4];append_arg_t args[4];
    for (unsigned i=0;i<4;i++) { args[i]=(append_arg_t){vfs_open("/mnt/concurrent.bin",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_APPEND),i};assert(args[i].file); }
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,append_thread,&args[i]));
    for (unsigned i=0;i<4;i++) { assert(!pthread_join(threads[i],NULL));vfs_close(args[i].file); }
    bool seen[4][100]={{false}};file_t *f=vfs_open("/mnt/concurrent.bin",VFS_O_RDONLY);assert(f && f->node->size==12800);
    uint8_t record[32];
    for (unsigned i=0;i<400;i++) { assert(vfs_read(f,record,sizeof(record))==sizeof(record));unsigned id=record[0],seq=record[1];assert(id<4 && seq<100 && !seen[id][seq]);seen[id][seq]=true;for (unsigned k=2;k<32;k++) assert(record[k]==0x91); }
    vfs_close(f);assert(!vfs_unlink("/mnt/concurrent.bin"));
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);assert(argc==4);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);disk_len=(size_t)ftell(f);rewind(f);
    disk=malloc(disk_len);uint8_t *original=malloc(disk_len);assert(disk && original && fread(disk,1,disk_len,f)==disk_len);fclose(f);memcpy(original,disk,disk_len);
    block_dev_t dev={.sector_size=(uint32_t)atoi(argv[3]),.sector_count=disk_len/(unsigned)atoi(argv[3]),.read_sector=block_read_sector,.write_sector=store,.flush=barrier};
    reset();ext4_mount_t *m;
    assert(!ext4_mount_rw(&dev,"/mnt",&m) && !writes && !flushes);
    size_t mount_allocs=live;
    reset();memcpy(disk,original,disk_len);
    for (size_t cut=1;cut<mount_allocs;cut++) { fail_alloc=(long)cut-1;int r=ext4_mount_rw(&dev,"/mnt",&m);assert(r==-VFS_ENOMEM && !m && !writes && !flushes && live==1);fail_alloc=-1; }
    reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));
    assert(vfs_create("/mnt/dirty.bin",VFS_FILE));reset();
    assert(ext4_mount_rw(&dev,"/mnt",&m)==-VFS_EIO && !m && !writes && !flushes);
    /* Every final clean-close event and ordinary sync failure taints. */
    for (unsigned which=0;which<3;which++) {
        reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));assert(!operation(0));
        reads=writes=flushes=0;assert(!ext4_freeze_and_sync(m));size_t total=which==0 ? reads : which==1 ? writes : flushes;
        for (size_t cut=0;cut<total;cut++) {
            reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));assert(!operation(0));
            if (which==0) fail_read=(long)cut;else if (which==1) fail_write=(long)cut;else fail_flush=(long)cut;
            assert(ext4_freeze_and_sync(m)==-VFS_EIO && m->engine->tainted);
            fail_read=fail_write=fail_flush=-1;size_t before=writes;assert(ext4_freeze_and_sync(m)==-VFS_EIO && writes==before);
        }
    }
    reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));fail_flush=0;
    assert(ext4_sync(m)==-VFS_EIO && m->engine->tainted);fail_flush=-1;
    reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));
    vfs_node_t *seed=vfs_lookup("/mnt/seed.bin");assert(seed);fail_flush=0;
    assert(!vfs_open("/mnt/seed.bin",VFS_O_RDWR|VFS_O_TRUNC) && ((e4_node_t *)seed)->opens==0);fail_flush=-1;
    reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));size_t mount_reads=reads;
    for (size_t cut=0;cut<mount_reads;cut++) {
        reset();memcpy(disk,original,disk_len);fail_read=(long)cut;
        assert(ext4_mount_rw(&dev,"/mnt",&m)==-VFS_EIO && !m && live==1 && !writes && !flushes);
    }
    fault_matrix(&dev,original);
    dev.read_sectors=read_run;dev.write_sectors=write_run;dev.max_run_bytes=16384;
    /* Accepted-prefix failure must dispatch once, never sector fallback. */
    uint8_t run_probe[16384]={0};
    reads=writes=0;size_t calls=write_runs;fail_write=1;
    assert(!block_write_sectors(&dev,0,16384/dev.sector_size,run_probe));
    assert(write_runs==calls+1 && writes==2);fail_write=-1;
    calls=write_runs;
    assert(!block_write_sectors(&dev,0,16384/dev.sector_size+1,run_probe));
    assert(!block_write_sectors(&dev,dev.sector_count-1,2,run_probe));
    assert(write_runs==calls);

    fault_matrix(&dev,original); /* same failure/taint gates with accepted prefixes */
    reset();memcpy(disk,original,disk_len);assert(!ext4_mount_rw(&dev,"/mnt",&m));
    uint32_t bs=m->bs;
    /* Prefill only free data ranges to make stale exposure observable. */
    for (uint32_t g=0;g<m->groups;g++) if (!(e4_u16(m->gd[g].raw+18)&2)) {
        const uint8_t *bitmap=disk+(uint64_t)e4_u32(m->gd[g].raw)*bs;
        for (uint32_t bit=0;bit<m->bpg && m->first+g*m->bpg+bit<m->blocks;bit++) {
            uint32_t b=m->first+g*m->bpg+bit;
            if (!e4_bit(bitmap,bit) && e4_data_range(m,b,1)) memset(disk+(uint64_t)b*bs,0xa5,bs);
        }
    }
    /* Mutation callbacks use the mount workspace even with heap unavailable. */
    file_t *frag=vfs_open("/mnt/fragmented.bin",VFS_O_CREAT|VFS_O_RDWR);
    file_t *blocker=vfs_open("/mnt/blocker.bin",VFS_O_CREAT|VFS_O_RDWR);assert(frag && blocker);
    uint8_t *fragment=calloc(800,bs);assert(fragment);
    for (unsigned i=0;i<400;i++) {
        memset(fragment+(size_t)i*2*bs,(uint8_t)(i%251+1),bs);
        frag->offset=(uint64_t)i*2*bs;fail_alloc=0;
        assert(vfs_write(frag,fragment+(size_t)i*2*bs,bs)==bs);
        assert(vfs_write(blocker,fragment+(size_t)i*2*bs,bs)==bs);fail_alloc=-1;
    }
    vfs_close(frag);vfs_close(blocker);check_file("/mnt/fragmented.bin",fragment,799*bs);free(fragment);
    assert(e4_u16(((e4_node_t *)vfs_lookup("/mnt/fragmented.bin"))->inode.extent+6)==(bs==1024 ? 2 : 1));
    assert(!vfs_truncate(vfs_lookup("/mnt/fragmented.bin"),0));
    assert(!vfs_unlink("/mnt/fragmented.bin") && !vfs_unlink("/mnt/blocker.bin"));
    size_t n=16*1024*1024;uint8_t *expected=malloc(n+bs*3);assert(expected);
    for (size_t i=0;i<n;i++) expected[i]=(uint8_t)(i*17+3);
    file_t *large=vfs_open("/mnt/large.bin",VFS_O_CREAT|VFS_O_RDWR);assert(large);
    write_bytes(large,expected,n);vfs_close(large);check_file("/mnt/large.bin",expected,n);
    file_t *one=vfs_open("/mnt/one.bin",VFS_O_CREAT|VFS_O_RDWR);assert(one);
    size_t before_flush=flushes,before_write=writes,before_runs=write_runs,before_reads=read_runs;
    /* Match wget's syscall-sized batches, independent of VFS callback cap. */
    for (size_t at=0;at<1048576;at+=16384) write_bytes(one,expected+at,16384);
    printf("EXT4 1MiB batched I/O: sectors=%zu barriers=%zu\n",writes-before_write,flushes-before_flush);
    printf("EXT4 1MiB transport: read runs=%zu write runs=%zu (max 16384 bytes)\n",read_runs-before_reads,write_runs-before_runs);
    if (bs==4096 && dev.sector_size==512) assert(write_runs-before_runs < (writes-before_write)/8);
    assert(flushes-before_flush<=64*3);
    if (bs<4096 && dev.sector_size==512) assert(write_runs-before_runs<=514);
    vfs_close(one);check_file("/mnt/one.bin",expected,1048576);
    /* Snapshot must perform no I/O, fit its public bound, and classify data. */
    char profile[2049],tiny[2]={0,0x55};size_t saved_reads=reads,saved_writes=writes;
    size_t profile_len=ext4_io_profile_format(profile,sizeof(profile)-1);
    assert(profile_len<sizeof(profile)-1);profile[profile_len]=0;
    assert(strstr(profile,"[EXT4 PERF] read inode calls=") && strstr(profile,"[EXT4 PERF] write calls="));
    assert(e4_read_profile[E4_PR_DATA].calls && e4_read_profile[E4_PR_INODE].calls && e4_write_bytes_total>=1048576);
    assert(ext4_io_profile_format(tiny,1)==1 && tiny[1]==0x55);
    assert(ext4_io_profile_format(NULL,1)==0 && ext4_io_profile_format(tiny,0)==0);
    assert(reads==saved_reads && writes==saved_writes);

    /* Existing initialized data: a complete aligned overwrite does not read
     * its old contents; partial overwrite must preserve its neighbours. */
    e4_inode_t *one_inode=&((e4_node_t *)vfs_lookup("/mnt/one.bin"))->inode;
    assert(!e4_u16(one_inode->extent+6));
    uint32_t physical=e4_u32(one_inode->extent+20);
    watched_lo=(uint64_t)physical*bs/dev.sector_size;
    watched_hi=((uint64_t)physical*bs+bs+dev.sector_size-1)/dev.sector_size;
    one=vfs_open("/mnt/one.bin",VFS_O_RDWR);assert(one);
    watched_reads=0;assert(vfs_write(one,expected,bs)==bs);
    if (bs>=dev.sector_size) assert(!watched_reads);
    one->offset=1;watched_reads=0;assert(vfs_write(one,expected+1,bs-2)==bs-2);
    assert(watched_reads);
    watched_lo=watched_hi=0;vfs_close(one);
    check_file("/mnt/one.bin",expected,1048576);
    file_t *a=vfs_open("/mnt/append.bin",VFS_O_CREAT|VFS_O_RDWR|VFS_O_APPEND),*b=vfs_open("/mnt/append.bin",VFS_O_WRONLY|VFS_O_APPEND);assert(a && b);
    assert(vfs_write(a,"abc",3)==3 && vfs_write(b,"def",3)==3 && a->offset==3 && b->offset==6);
    assert(vfs_unlink("/mnt/append.bin")==-VFS_EOPNOTSUPP);
    __atomic_add_fetch(&a->ref_count,1,__ATOMIC_RELAXED);vfs_close(a);assert(((e4_node_t *)b->node)->opens==2);vfs_close(a);vfs_close(b);
    check_file("/mnt/append.bin",(const uint8_t *)"abcdef",6);
    file_t *active_dest=vfs_open("/mnt/large.bin",VFS_O_RDONLY);assert(active_dest);
    assert(vfs_rename("/mnt/one.bin","/mnt/large.bin")==-VFS_EEXIST);vfs_close(active_dest);check_file("/mnt/large.bin",expected,n);
    assert(!vfs_mkdir("/mnt/sub",0));assert(vfs_rename("/mnt/sub","/mnt/sub2")==-VFS_EOPNOTSUPP);
    assert(!vfs_rename("/mnt/one.bin","/mnt/sub/moved.bin"));assert(!vfs_lookup("/mnt/one.bin"));check_file("/mnt/sub/moved.bin",expected,1048576);
    assert(vfs_unlink("/mnt/sub")==-VFS_ENOTEMPTY);
    assert(!vfs_unlink("/mnt/sub/moved.bin") && !vfs_unlink("/mnt/sub"));
    assert(!vfs_truncate(vfs_lookup("/mnt/append.bin"),0));check_file("/mnt/append.bin",expected,0);
    a=vfs_open("/mnt/gap.bin",VFS_O_CREAT|VFS_O_RDWR);assert(a);assert(vfs_write(a,"abc",3)==3);a->offset=bs*3+7;assert(vfs_write(a,"z",1)==1);vfs_close(a);
    memset(expected,0,bs*3+8);memcpy(expected,"abc",3);expected[bs*3+7]='z';check_file("/mnt/gap.bin",expected,bs*3+8);
    /* Directory record splitting/growth, deletion/reuse, inode-group changes. */
    for (unsigned i=0;i<130;i++) { char name[128];snprintf(name,sizeof(name),"/mnt/file-%03u-long-directory-record",i);assert(vfs_create(name,VFS_FILE)); }
    for (unsigned i=0;i<130;i++) { char name[128];snprintf(name,sizeof(name),"/mnt/file-%03u-long-directory-record",i);assert(!vfs_unlink(name)); }
    file_t *high=vfs_open("/mnt/high.bin",VFS_O_CREAT|VFS_O_RDWR);assert(high);
    uint64_t high_off=(1ULL<<32)+bs+13;high->offset=high_off;
    assert(vfs_write(high,"HIGHOFFSET",10)==10);vfs_close(high);
    uint8_t high_bytes[128];memset(high_bytes,0xaa,sizeof(high_bytes));
    vfs_node_t *high_node=vfs_lookup("/mnt/high.bin");assert(high_node && high_node->size==high_off+10);
    assert(high_node->read(high_node,high_off-17,high_bytes,sizeof(high_bytes))==27);
    for (unsigned i=0;i<17;i++) assert(high_bytes[i]==0);
    assert(!memcmp(high_bytes+17,"HIGHOFFSET",10));
    /* Make unwritten media visibly stale before partial conversion. */
    e4_inode_t *unwritten=vfs_lookup("/mnt/unwritten.bin")->fs_private;
    assert(!e4_mapping(unwritten));
    for (unsigned i=0;i<unwritten->map->count;i++) {
        e4_extent_t x=unwritten->map->entries[i];assert(x.unwritten);
        memset(disk+(uint64_t)x.physical*bs,0xa5,(size_t)x.len*bs);
    }
    /* Partial unwritten conversion must leave untouched bytes zero. */
    a=vfs_open("/mnt/unwritten.bin",VFS_O_RDWR);assert(a);a->offset=bs+13;assert(vfs_write(a,"UVW",3)==3);vfs_close(a);
    memset(expected,0,bs*8);memcpy(expected+bs+13,"UVW",3);check_file("/mnt/unwritten.bin",expected,bs*8);
    vfs_node_t *limit=vfs_lookup("/mnt/large.bin");size_t before=writes;uint64_t off=8ULL*1024*1024*1024;
    assert(limit->write(limit,&off,false,"x",1)==-VFS_EFBIG && writes==before && off==8ULL*1024*1024*1024);
    assert(vfs_truncate(limit,1)==-VFS_EINVAL && writes==before);
    assert(vfs_lookup("/mnt")->lookup(vfs_lookup("/mnt"),".")==vfs_lookup("/mnt"));
    parallel_append();
    assert(!ext4_sync(m) && e4_u16(disk+1024+58)==0);
    assert(!ext4_freeze_and_sync(m) && e4_u16(disk+1024+58)==1);
    assert(vfs_create("/mnt/frozen",VFS_FILE)==NULL && vfs_get_last_create_error()==-VFS_EROFS);
    save(argv[2]);reset();assert(!ext4_mount_ro(&dev,"/mnt",&m));
    for (size_t i=0;i<n;i++) expected[i]=(uint8_t)(i*17+3);
    check_file("/mnt/large.bin",expected,n);reset();kfree(g_vfs_root);g_vfs_root=NULL;assert(!live);
    free(expected);free(original);free(disk);
    printf("PASS VFS ext4 bs=%u sector=%u: 16MiB/1MiB, append, lifetime, gap, namespace, freeze/remount\n",bs,dev.sector_size);return 0;
}
