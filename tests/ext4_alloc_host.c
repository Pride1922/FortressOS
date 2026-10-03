/* Actual mutation engine + memory disk. Reuse admission adapters/oracles. */
#define main ext4_format_main
#include "ext4_format_host.c"
#undef main
#include <time.h>
static size_t stores, barriers, allocations;
static long fail_store=-1, fail_barrier=-1;
static bool uncertain;
static ext4_engine_t *oracle_engine;
static uint32_t oracle_ino;
static uint64_t peak_ns;
static size_t peak_reads,peak_stores,peak_images;
static void live_references(const uint8_t *h,unsigned depth,unsigned *visits) {
    ext4_mount_t *fs=oracle_engine->fs;
    assert(depth<=2 && ++*visits<=4096 && e4_u16(h)==0xf30a && e4_u16(h+6)==depth);
    unsigned count=e4_u16(h+2);assert(count<=e4_u16(h+4));
    for (unsigned i=0;i<count;i++) {
        const uint8_t *x=h+12+i*12;
        uint32_t block=e4_u32(x+(depth ? 4 : 8));
        unsigned raw=e4_u16(x+4),len=depth ? 1 : raw>32768 ? raw-32768 : raw;
        assert(e4_data_range(fs,block,len));
        for (unsigned k=0;k<len;k++) {
            uint32_t group=(block+k-fs->first)/fs->bpg,bit=(block+k-fs->first)%fs->bpg;
            const uint8_t *gd=disk+(uint64_t)(fs->first+1)*fs->bs+group*32;
            assert(!(e4_u16(gd+18)&2));
            assert(e4_bit(disk+(uint64_t)e4_u32(gd)*fs->bs,bit));
        }
        if (depth) live_references(disk+(uint64_t)block*fs->bs,depth-1,visits);
    }
}
static void barrier_oracle(void) {
    if (!oracle_engine) return;
    ext4_mount_t *fs=oracle_engine->fs;
    uint32_t g=(oracle_ino-1)/fs->ipg,index=(oracle_ino-1)%fs->ipg;
    const uint8_t *gd=disk+(uint64_t)(fs->first+1)*fs->bs+g*32;
    const uint8_t *raw=disk+(uint64_t)e4_u32(gd+8)*fs->bs+index*256;
    unsigned visits=0;live_references(raw+40,e4_u16(raw+46),&visits);
}
static int checked_commit(ext4_engine_t *e) {
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    size_t rd=reads,wr=stores;oracle_engine=e;int r=ext4_engine_commit(e);oracle_engine=NULL;
    clock_gettime(CLOCK_MONOTONIC,&end);
    uint64_t elapsed=(uint64_t)(end.tv_sec-start.tv_sec)*1000000000ULL;
    if (end.tv_nsec>=start.tv_nsec) elapsed+=(uint64_t)(end.tv_nsec-start.tv_nsec);
    else elapsed-=(uint64_t)(start.tv_nsec-end.tv_nsec);
    if (!r) {
        if (elapsed>peak_ns) peak_ns=elapsed;
        if (reads-rd>peak_reads) peak_reads=reads-rd;
        if (stores-wr>peak_stores) peak_stores=stores-wr;
        if (e->images>peak_images) peak_images=e->images;
    }
    return r;
}
#define ext4_engine_commit checked_commit
static bool store(block_dev_t *d,uint64_t lba,const void *buf) {
    assert(pipe_host_lock_depth==1 && lba<d->sector_count);
    stores++;
    if (fail_store==0) {
        if (uncertain) memcpy(disk+lba*d->sector_size,buf,d->sector_size);
        return false;
    }
    if (fail_store>0) fail_store--;
    memcpy(disk+lba*d->sector_size,buf,d->sector_size);return true;
}
static bool barrier(block_dev_t *d) {
    (void)d; assert(pipe_host_lock_depth==1);barriers++;
    if (fail_barrier==0) return false;
    if (fail_barrier>0) fail_barrier--;
    barrier_oracle(); return true;
}
static void load(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);disk_len=ftell(f);rewind(f);
    disk=malloc(disk_len);assert(disk && fread(disk,1,disk_len,f)==disk_len);fclose(f);
}
static void audit(ext4_engine_t *e,uint32_t ino,unsigned expected_depth) {
    e4_inode_t in;assert(!e4_inode(e->fs,ino,&in));
    assert(e4_u16(in.extent+6)==expected_depth);
    e->fs->visits=e->fs->pending_count=e->fs->physical_count=0;
    assert(!e4_tree(&in,in.extent,expected_depth,true,0,1ULL<<32));
    allocations=e->fs->pending_count;
    uint64_t count=0;
    for (unsigned i=0;i<e->fs->pending_count;i++) {
        e4_extent_t x=e->fs->pending[i]; count+=x.len;
        for (uint32_t k=0;k<x.len;k++) {
            assert(e4_bytes(e->fs,(uint64_t)(x.physical+k)*e->fs->bs,e->fs->scratch,e->fs->bs));
            for (unsigned j=0;j<e->fs->bs;j++) assert(!e->fs->scratch[j]);
        }
    }
    unsigned external=0;
    for (unsigned i=0;i<e->fs->physical_count;i++) {
        e4_range_t x=e->fs->physical[i]; bool data=false;
        for (unsigned j=0;j<e->fs->pending_count;j++) if (x.lo==e->fs->pending[j].physical && x.len==e->fs->pending[j].len) data=true;
        if (!data) external++;
    }
    uint8_t raw[256];uint32_t g=(ino-1)/e->fs->ipg,index=(ino-1)%e->fs->ipg;
    assert(e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+index*256,raw,256));
    assert(e4_u32(raw+28)==(count+external)*(e->fs->bs/512));
    for (uint32_t group=0;group<e->fs->groups;group++) if (!(e4_u16(e->fs->gd[group].raw+18)&2)) {
        assert(e4_bitmap(e->fs,group,false));
        uint32_t start=e->fs->first+group*e->fs->bpg;
        for (unsigned i=0;i<e->fs->ranges;i++) {
            e4_range_t x=e->fs->reserved[i];if (x.lo>=start && x.lo<start+e->fs->bpg)
                for (unsigned k=0;k<x.len;k++) assert(e4_bit(e->fs->scratch,x.lo-start+k));
        }
    }
}
static void snapshot(ext4_engine_t *e,const char *base,const char *suffix) {
    assert(!ext4_engine_finish(e));
    char path[1024];assert(snprintf(path,sizeof(path),"%s.%s.img",base,suffix)<(int)sizeof(path));
    FILE *f=fopen(path,"wb");assert(f && fwrite(disk,1,disk_len,f)==disk_len);fclose(f);
}
static void bitmap_oracle(ext4_engine_t *e,const uint8_t *baseline,bool exact) {
    ext4_mount_t *fs=e->fs;uint64_t free_count=0;
    for (uint32_t g=0;g<fs->groups;g++) {
        const uint8_t *old=baseline ? baseline+(uint64_t)(fs->first+1)*fs->bs+g*32 : NULL;
        uint32_t start=fs->first+g*fs->bpg,n=fs->blocks-start;if (n>fs->bpg) n=fs->bpg;
        unsigned free=0;
        if (e4_u16(fs->gd[g].raw+18)&2) {
            free=e4_u16(fs->gd[g].raw+12);
        } else {
            assert(e4_bitmap(fs,g,false));
            for (uint32_t k=0;k<n;k++) {
                bool bit=e4_bit(fs->scratch,k);if (!bit) free++;
                if (exact) {
                    bool expected=e4_u16(old+18)&2 ? !e4_data_range(fs,start+k,1) :
                        e4_bit(baseline+(uint64_t)e4_u32(old)*fs->bs,k);
                    assert(bit==expected);
                }
            }
        }
        assert(free==e4_u16(fs->gd[g].raw+12));free_count+=free;
    }
    uint8_t sb[1024];assert(e4_bytes(fs,1024,sb,1024));assert(free_count==e4_u32(sb+12));
}
static void stale_free_blocks(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;
    for (uint32_t g=0;g<fs->groups;g++) {
        uint32_t start=fs->first+g*fs->bpg,n=fs->blocks-start;if (n>fs->bpg) n=fs->bpg;
        bool uninit=e4_u16(fs->gd[g].raw+18)&2;
        if (!uninit) assert(e4_bitmap(fs,g,false));
        for (uint32_t k=0;k<n;k++) if (uninit ? e4_data_range(fs,start+k,1) : !e4_bit(fs->scratch,k))
            memset(disk+(uint64_t)(start+k)*fs->bs,0xa5,fs->bs);
    }
}
static int fault_plan(ext4_engine_t *e,uint32_t ino,uint32_t logical,unsigned kind) {
    uint32_t reserved;
    return kind==0 ? ext4_engine_grow(e,ino,logical,2) : kind==1 ? ext4_engine_trim(e,ino,logical) :
        ext4_engine_reserve_inode_kind(e,kind==3,&reserved);
}
static void ownership_and_dirty_mount(block_dev_t *dev,uint32_t ino,const uint8_t *baseline) {
    ext4_engine_t *e=NULL,*other=(void *)1;
    assert(!ext4_engine_open(dev,&e));
    assert(ext4_engine_open(dev,&other)==-VFS_EEXIST && !other);
    ext4_mount_t *mount=(void *)1;
    assert(ext4_mount_ro(dev,"/mnt",&mount)==-VFS_EEXIST && !mount);
    assert(!ext4_engine_grow(e,ino,0,1));ext4_engine_abort(e);ext4_engine_close(e);
    assert(!live && !memcmp(disk,baseline,disk_len));
    assert(!ext4_engine_open(dev,&e));assert(!ext4_engine_grow(e,ino,0,1));assert(!ext4_engine_commit(e));
    size_t st=stores,ba=barriers;ext4_engine_close(e);assert(st==stores && ba==barriers && !live);
    assert(e4_u16(disk+1024+58)==0);
    assert(ext4_engine_open(dev,&e)==-VFS_EIO && !e && !live);
    assert(ext4_mount_ro(dev,"/mnt",&mount)==-VFS_EIO && !mount && !live && !root.children);
    assert(st==stores && ba==barriers);memcpy(disk,baseline,disk_len);
    puts("PASS exclusive engine/RO ownership, abort is write-free, close leaves dirty, dirty remount rejected");
}
static void capacity_limits(block_dev_t *dev,uint32_t ino,const uint8_t *baseline) {
    ext4_engine_t *e=NULL;
    for (unsigned kind=0;kind<2;kind++) {
        memcpy(disk,baseline,disk_len);assert(!ext4_engine_open(dev,&e));
        ext4_mount_t *fs=e->fs;
        for (uint32_t g=0;g<fs->groups;g++) {
            uint8_t *d=disk+(uint64_t)(fs->first+1)*fs->bs+g*32;
            uint8_t *map=disk+(uint64_t)e4_u32(d+kind*4)*fs->bs;
            memset(map,0xff,fs->bs);
            e4_p16(d+(kind ? 14 : 12),0);
            e4_p16(d+18,(uint16_t)(e4_u16(d+18)&~(kind ? 1 : 2)));
            if (kind) e4_p16(d+28,0);
            e4_p16(d+24+kind*2,(uint16_t)e4_crc(fs->seed,map,(kind ? fs->ipg : fs->bpg)/8));
            uint8_t group[4];e4_p32(group,g);e4_p16(d+30,0);
            e4_p16(d+30,(uint16_t)e4_crc(e4_crc(fs->seed,group,4),d,32));
        }
        e4_p32(disk+1024+(kind ? 16 : 12),0);sb_checksum();ext4_engine_close(e);assert(!live);
        assert(!ext4_engine_open(dev,&e));stores=barriers=0;uint32_t reserved;
        assert((kind ? ext4_engine_reserve_inode(e,&reserved) : ext4_engine_grow(e,ino,0,1))==-VFS_ENOSPC);
        assert(!e->ready && !stores && !barriers);ext4_engine_close(e);assert(!live);
    }
    memcpy(disk,baseline,disk_len);assert(!ext4_engine_open(dev,&e));stores=barriers=0;
    assert(!e4_plan_begin(e));
    for (unsigned i=0;i<E4_IMAGES;i++) assert(e4_stage(e,i,E4_ALLOC));
    assert(!e4_stage(e,E4_IMAGES,E4_ALLOC) && e4_plan_end(e)==-VFS_EFBIG);
    assert(!e->ready && e->images==E4_IMAGES && !stores && !barriers);
    ext4_engine_close(e);assert(!live);memcpy(disk,baseline,disk_len);
    puts("PASS ENOSPC block/inode exhaustion and 64-image EFBIG preflight: zero writes");
}
static void finish_faults(block_dev_t *dev,uint32_t ino,const uint8_t *baseline) {
    ext4_engine_t *e=NULL;
    assert(!ext4_engine_open(dev,&e));assert(!ext4_engine_grow(e,ino,0,2));assert(!ext4_engine_commit(e));
    reads=stores=barriers=0;assert(!ext4_engine_finish(e));
    size_t counts[]={reads,stores,barriers};ext4_engine_close(e);assert(!live);
    for (unsigned kind=0;kind<3;kind++) for (size_t k=0;k<counts[kind];k++) {
        memcpy(disk,baseline,disk_len);assert(!ext4_engine_open(dev,&e));
        assert(!ext4_engine_grow(e,ino,0,2));assert(!ext4_engine_commit(e));
        fail_read=kind==0 ? (long)k : -1;fail_store=kind==1 ? (long)k : -1;fail_barrier=kind==2 ? (long)k : -1;
        assert(ext4_engine_finish(e)==-VFS_EIO && e->tainted);
        size_t st=stores,ba=barriers;assert(ext4_engine_finish(e)==-VFS_EIO);assert(st==stores && ba==barriers);
        fail_read=fail_store=fail_barrier=-1;ext4_engine_close(e);assert(!live);
    }
    memcpy(disk,baseline,disk_len);puts("PASS every finish read/write/barrier failure: taint, no clean-success claim");
}
static void faults(block_dev_t *dev,uint32_t ino,const uint8_t *baseline,uint32_t logical,unsigned kind,bool staging) {
    ext4_engine_t *e=NULL;
    assert(!ext4_engine_open(dev,&e));reads=stores=barriers=0;
    assert(!fault_plan(e,ino,logical,kind));size_t planning_reads=reads;
    assert(!stores && !barriers);
    reads=0;assert(!ext4_engine_commit(e));size_t commit_reads=reads, count=stores, syncs=barriers;
    ext4_engine_close(e);assert(!live);memcpy(disk,baseline,disk_len);
    for (size_t i=0;i<planning_reads;i++) {
        if (!staging && i!=0 && i!=planning_reads/2 && i!=planning_reads-1) continue;
        assert(!ext4_engine_open(dev,&e));size_t before=live;stores=barriers=0;fail_read=i;
        assert(fault_plan(e,ino,logical,kind)==-VFS_EIO && !e->ready && live==before);
        assert(!stores && !barriers && !memcmp(disk,baseline,disk_len));
        fail_read=-1;ext4_engine_close(e);assert(!live);
    }
    for (unsigned mode=0;mode<4;mode++) {
        size_t n=mode==0 || mode==3 ? count : mode==1 ? syncs : commit_reads;
        for (size_t i=0;i<n;i++) {
            memcpy(disk,baseline,disk_len);assert(!ext4_engine_open(dev,&e));
            assert(!fault_plan(e,ino,logical,kind));stores=barriers=0;
            fail_store=(mode==0 || mode==3) ? (long)i : -1;
            fail_barrier=mode==1 ? (long)i : -1;
            fail_read=mode==2 ? (long)i : -1;uncertain=mode==3;
            assert(ext4_engine_commit(e)==-VFS_EIO && e->tainted);
            size_t st=stores,ba=barriers;
            assert(ext4_engine_commit(e)==-VFS_EIO && ext4_engine_grow(e,ino,0,1)==-VFS_EIO && ext4_engine_finish(e)==-VFS_EIO);
            assert(st==stores && ba==barriers);
            fail_store=fail_barrier=fail_read=-1;uncertain=false;
            ext4_engine_close(e);assert(!live);
        }
    }
    memcpy(disk,baseline,disk_len);
    printf("PASS fault cuts: %zu staging reads, %zu commit reads, %zu sector writes, %zu barriers, uncertain accepted-write failures\n",planning_reads,commit_reads,count,syncs);
}
static void maximum_map(block_dev_t *dev,uint32_t ino,const char *output) {
    oracle_ino=ino;ext4_engine_t *e=NULL;assert(!ext4_engine_open(dev,&e));
    stores=barriers=0;
    assert(ext4_engine_grow(e,ino,8190,2)==-VFS_EFBIG && !e->ready && !stores && !barriers);
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    assert(!ext4_engine_grow(e,ino,8190,1));
    clock_gettime(CLOCK_MONOTONIC,&end);
    double stage_ms=(end.tv_sec-start.tv_sec)*1000.0+(end.tv_nsec-start.tv_nsec)/1000000.0;
    assert(e->images<=64);assert(!ext4_engine_commit(e));
    e4_inode_t in;assert(!e4_inode(e->fs,ino,&in));
    e->fs->visits=e->fs->pending_count=e->fs->physical_count=0;
    assert(!e4_tree(&in,in.extent,2,true,0,1ULL<<32) && e->fs->pending_count==4096);
    bitmap_oracle(e,NULL,false);assert(!ext4_engine_finish(e));ext4_engine_close(e);assert(!live);
    FILE *f=fopen(output,"wb");assert(f && fwrite(disk,1,disk_len,f)==disk_len);fclose(f);
    printf("PASS maximum 4096 extents: %zu staged images, %zu sector reads, %zu sector writes, peak mock commit %.3f ms, maximum-map staging %.3f ms\n",
           peak_images,peak_reads,peak_stores,peak_ns/1000000.0,stage_ms);
}
int main(int argc,char **argv) {
    assert(argc==6 || argc==7); reset();load(argv[1]);unsigned ss=(unsigned)strtoul(argv[3],NULL,10),ino=(unsigned)strtoul(argv[4],NULL,10),blocker=(unsigned)strtoul(argv[5],NULL,10);
    block_dev_t dev={.sector_size=ss,.sector_count=disk_len/ss,.read_sector=block_read_sector,.write_sector=store,.flush=barrier};
    if (argc==7) { maximum_map(&dev,ino,argv[2]);free(disk);disk=NULL;return 0; }
    ext4_engine_t *e=NULL;
    assert(!ext4_engine_open(&dev,&e));stale_free_blocks(e);ext4_engine_close(e);assert(!live);
    uint8_t *baseline=malloc(disk_len);assert(baseline);memcpy(baseline,disk,disk_len);
    for (long i=0;i<4;i++) {
        fail_alloc=i;assert(ext4_engine_open(&dev,&e)==-VFS_ENOMEM && !e && !live);fail_alloc=-1;
    }
    oracle_ino=ino;
    ownership_and_dirty_mount(&dev,ino,baseline);
    capacity_limits(&dev,ino,baseline);
    finish_faults(&dev,ino,baseline);
    faults(&dev,ino,baseline,0,0,true);
    faults(&dev,ino,baseline,0,2,true);
    faults(&dev,ino,baseline,0,3,true);
    assert(!ext4_engine_open(&dev,&e));
    size_t before=live;fail_alloc=0;
    assert(!ext4_engine_grow(e,ino,0,16));assert(live==before);
    assert(!ext4_engine_commit(e));assert(live==before);fail_alloc=-1;audit(e,ino,0);assert(allocations==1);
    assert(ext4_engine_grow(e,ino,0,1)==-VFS_EEXIST && !e->ready);
    assert(ext4_engine_grow(e,ino,UINT32_MAX,1)==-VFS_EFBIG && !e->ready);
    assert(!ext4_engine_grow(e,ino,16,1));assert(!ext4_engine_commit(e));audit(e,ino,0);assert(allocations==1);
    unsigned target=1048576/e->fs->bs;
    for (unsigned at=17;at<target;) {
        unsigned n=target-at;if (n>16) n=16;
        assert(!ext4_engine_grow(e,ino,at,n));assert(!ext4_engine_commit(e));at+=n;
    }
    audit(e,ino,0);assert(allocations==1);snapshot(e,argv[2],"contiguous");
    assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));audit(e,ino,0);
    bitmap_oracle(e,baseline,true);
    assert(!ext4_engine_grow(e,ino,0,1));assert(!ext4_engine_commit(e));
    assert(!ext4_engine_grow(e,blocker,0,1));assert(!ext4_engine_commit(e));
    assert(!ext4_engine_grow(e,ino,1,1));assert(!ext4_engine_commit(e));
    audit(e,ino,0);assert(allocations==2); /* Contiguous logical, separated physical. */
    assert(!ext4_engine_trim(e,blocker,0));assert(!ext4_engine_commit(e));
    assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));bitmap_oracle(e,baseline,true);
    unsigned cross=e4_u16(e->fs->gd[0].raw+12)+16;
    for (unsigned at=0;at<cross;) {
        unsigned n=cross-at;if (n>16) n=16;
        assert(!ext4_engine_grow(e,ino,at,n));assert(!ext4_engine_commit(e));at+=n;
    }
    audit(e,ino,0);assert(allocations>=2);bitmap_oracle(e,baseline,false);
    snapshot(e,argv[2],"crossgroup");
    assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));bitmap_oracle(e,baseline,true);
    unsigned leaf=(e->fs->bs-12)/12;
    for (unsigned k=0;k<4*leaf+1;k++) {
        assert(!ext4_engine_grow(e,ino,k*2,1));assert(e->images<=64);
        assert(!ext4_engine_commit(e));
        if (k==4) { audit(e,ino,1);snapshot(e,argv[2],"promotion"); }
        if (k==leaf) audit(e,ino,1);
    }
    audit(e,ino,2);assert(allocations==4*leaf+1);bitmap_oracle(e,baseline,false);snapshot(e,argv[2],"depth2");
    ext4_engine_close(e);assert(!live);
    uint8_t *deep=malloc(disk_len);assert(deep);memcpy(deep,disk,disk_len);
    faults(&dev,ino,deep,(4*leaf+1)*2+2,0,false);
    faults(&dev,ino,deep,0,1,false);
    free(deep);assert(!ext4_engine_open(&dev,&e));
    assert(!ext4_engine_trim(e,ino,6));assert(!ext4_engine_commit(e));audit(e,ino,0);assert(allocations==3);
    assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));audit(e,ino,0);assert(!allocations);
    uint32_t reserved;
    assert(!ext4_engine_reserve_inode(e,&reserved) && reserved>=11);
    assert(!ext4_engine_commit(e));assert(ext4_engine_finish(e)==-VFS_EINVAL);
    assert(!ext4_engine_release_inode(e,reserved));assert(!ext4_engine_commit(e));
    uint32_t many[64];unsigned inode_cross=e4_u16(e->fs->gd[0].raw+14)+1;
    assert(inode_cross<=64);
    for (unsigned k=0;k<inode_cross;k++) {
        assert(!ext4_engine_reserve_inode_kind(e,k%2==0,&many[k]));assert(!ext4_engine_commit(e));
    }
    assert((many[inode_cross-1]-1)/e->fs->ipg>=1);
    for (unsigned k=0;k<inode_cross;k++) {
        assert(!ext4_engine_release_inode(e,many[k]));assert(!ext4_engine_commit(e));
    }
    uint32_t directory;
    assert(!ext4_engine_reserve_inode_kind(e,true,&directory));assert(!ext4_engine_commit(e));
    assert(!ext4_engine_release_inode(e,directory));assert(!ext4_engine_commit(e));
    assert(!ext4_engine_grow(e,ino,0,4));ext4_engine_abort(e);
    assert(!ext4_engine_grow(e,ino,0,4));assert(!ext4_engine_commit(e));audit(e,ino,0);
    assert(!ext4_engine_finish(e));ext4_engine_close(e);assert(!live);
    /* Allocation set exactly restores original after a second truncation. */
    assert(!ext4_engine_open(&dev,&e));assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));
    bitmap_oracle(e,baseline,true);
    uint32_t high=(uint32_t)((1ULL<<32)/e->fs->bs)+7;
    assert(!ext4_engine_grow(e,ino,high,1));assert(!ext4_engine_commit(e));
    e4_inode_t large;assert(!e4_inode(e->fs,ino,&large) && large.size==(1ULL<<32)+8*e->fs->bs);
    snapshot(e,argv[2],"highoffset");ext4_engine_close(e);assert(!live);
    ext4_mount_t *ro=NULL;assert(!ext4_mount_ro(&dev,"/mnt",&ro));
    vfs_node_t *file=root.children->lookup(root.children,"engine.bin");assert(file && file->size>UINT32_MAX);
    uint8_t bytes[32];assert(file->read(file,file->size-17,bytes,sizeof(bytes))==17);
    for (unsigned k=0;k<17;k++) assert(!bytes[k]);
    reset();assert(!live);
    assert(!ext4_engine_open(&dev,&e));assert(!ext4_engine_trim(e,ino,0));assert(!ext4_engine_commit(e));
    bitmap_oracle(e,baseline,true);
    assert(!ext4_engine_finish(e));ext4_engine_close(e);assert(!live);
    FILE *f=fopen(argv[2],"wb");assert(f && fwrite(disk,1,disk_len,f)==disk_len);fclose(f);
    free(baseline);free(disk);disk=NULL;
    printf("Measured successful mock commits: peak %zu images, %zu reads, %zu writes, %.3f ms (includes host sanitizer/oracle; not hardware IRQ latency)\n",
           peak_images,peak_reads,peak_stores,peak_ns/1000000.0);
    puts("EXT4 Phase 3 ASan/UBSan PASS: growth/merge, root promotions, leaf splits, index propagation, shrink/reclaim, inode reserve/release, fault boundaries (mock disk only)");
    return 0;
}
