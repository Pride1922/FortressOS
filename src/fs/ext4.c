/* E4-A: bounded extent-backed reads and synchronous non-journaled writes. */
#include "ext4.h"
#include "vfs.h"
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
#include "permission_values.h"
#endif
#include "heap.h"
#include "string.h"
#include "spinlock.h"
#include "ext4_engine.h"

#define E4_GROUPS 1024u
#define E4_NODES 1024u
#define E4_DIR_MAX (1024u * 1024u)
#define E4_MAP_MAX 4096u
typedef struct { uint32_t logical, physical, len; bool unwritten; } e4_extent_t;
typedef struct e4_map { struct e4_map *next; unsigned count; e4_extent_t entries[]; } e4_map_t;
typedef struct { uint32_t lo, len; } e4_range_t;
typedef struct { uint8_t raw[32]; } e4_group_t;
typedef struct {
    ext4_mount_t *fs;
    uint32_t ino, generation;
    uint64_t size;
    uint16_t mode; /* type discriminator retained for bounded engine helpers */
    uint16_t full_mode;
    uint32_t uid,gid;
    uint8_t extent[60];
    e4_map_t *map;
} e4_inode_t;
typedef struct { vfs_node_t node; e4_inode_t inode; unsigned opens, refs; bool removed, legacy; } e4_node_t;
struct ext4_mount {
    block_dev_t *dev;
    ext4_engine_t *engine;
    e4_map_t *rw_map;
    uint32_t rw_ino, rw_generation;
    bool frozen;
    bool journal_bootstrap;
    bool journal_mounted, state_transition;
    uint32_t orphan_access; /* Exclusive 8.4 workbench, one validated orphan. */
    uint32_t journal_ino;
    e4_range_t *journal_reserved;
    unsigned journal_ranges;
    uint32_t bs, blocks, inodes, first, bpg, ipg, groups, seed, gdt_blocks;
    e4_group_t *gd;
    e4_range_t *reserved;
    unsigned ranges, nodes;
    e4_node_t *cached[E4_NODES];
    /* RW-only clean metadata cache. Sole writer is serialized by e4_lock. */
    struct { uint32_t block; bool valid; uint8_t bytes[4096]; } metadata[8];
    unsigned metadata_next;
    bool metadata_enabled;
    uint8_t sector[4096], scratch[4096];
    uint8_t tree[2][4096], validation_bitmap[4096];
    uint32_t validation_group;
    bool validation_valid, reserved_sorted;
    e4_extent_t pending[E4_MAP_MAX];
    e4_range_t physical[E4_MAP_MAX * 2];
    unsigned pending_count, physical_count, visits, map_entries;
    e4_map_t *maps;
};
static spinlock_t e4_lock = SPINLOCK_RANKED(1, "ext4");
#ifdef FORTRESS_EXT4_COMMIT_PAUSE_TEST
static ext4_mount_t *e4_pause_mount;
static void (*e4_pause_callback)(void);
int ext4_test_arm_commit_pause(ext4_mount_t *mount,void (*pause)(void)) {
    spin_debug_assert_unheld();
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    int r=0;
    if (!mount || !pause || !mount->journal_mounted || !mount->engine || e4_pause_callback) r=-VFS_EINVAL;
    else { e4_pause_mount=mount;e4_pause_callback=pause; }
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
#endif
static ext4_mount_t *e4_active;
/* Boot publishes at most one /mnt filesystem before user execution. Raw
 * read-only devfs I/O joins that same exclusion; never nests filesystem locks. */
bool ext4_device_read_sector(block_dev_t *dev,uint64_t lba,void *buf,bool *handled) {
    spin_debug_assert_unheld();
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    *handled=e4_active!=NULL;
    bool ok=*handled && block_read_sector(dev,lba,buf);
    spin_unlock_irqrestore(&e4_lock,flags);
    return ok;
}
static bool e4_engine_busy;
static void e4_engine_owner(ext4_engine_t *e);
static int e4_journal_drain(ext4_mount_t *fs,bool finish);
static bool e4_mounted_orphan(ext4_mount_t *fs,uint32_t ino);
static int e4_journal_refresh(vfs_node_t *node);
static uint16_t e4_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t e4_u32(const uint8_t *p) { return e4_u16(p) | (uint32_t)e4_u16(p+2) << 16; }
static void e4_p32(uint8_t *p, uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v >> (i*8));
}
static uint64_t e4_tsc(void);
static uint64_t e4_crc_calls,e4_crc_bytes,e4_crc_cycles,e4_crc_anomalies;
/* Reflected Castagnoli, no final complement: ext4 chains raw CRC states. */
static uint32_t e4_crc_impl(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p=data;
    while (len--) {
        crc ^= *p++;
        for (unsigned b=0;b<8;b++) crc=(crc >> 1) ^ (0x82f63b78u & (0u-(crc & 1u)));
    }
    return crc;
}
static uint32_t e4_crc(uint32_t crc,const void *data,size_t len) {
    uint64_t start=e4_tsc();uint32_t result=e4_crc_impl(crc,data,len);uint64_t end=e4_tsc();
    __atomic_fetch_add(&e4_crc_calls,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&e4_crc_bytes,len,__ATOMIC_RELAXED);
    if (end>=start) __atomic_fetch_add(&e4_crc_cycles,end-start,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&e4_crc_anomalies,1,__ATOMIC_RELAXED);
    return result;
}
/* Diagnostic counters only; no caching, I/O ordering or scheduling changes. */
enum { E4_PR_SUPER,E4_PR_DESCRIPTOR,E4_PR_BITMAP,E4_PR_INODE,
       E4_PR_EXTENT,E4_PR_DIRECTORY,E4_PR_DATA,E4_PR_OTHER,E4_PR_COUNT };
typedef struct { uint64_t calls,requests,bytes,cycles,failures,anomalies; } e4_read_sample_t;
static e4_read_sample_t e4_read_profile[E4_PR_COUNT];
static uint64_t e4_write_calls,e4_write_bytes_total,e4_write_cycles,e4_write_failures;
static uint64_t e4_plan_cycles,e4_commit_cycles,e4_refresh_cycles,e4_clock_anomalies;
static uint64_t e4_write_max_cycles,e4_write_lock_wait_cycles;
static uint64_t e4_tsc(void) {
    uint32_t lo,hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo),"=d"(hi) :: "memory");
    return ((uint64_t)hi<<32)|lo;
}
static void e4_elapsed(uint64_t *total,uint64_t start,uint64_t end) {
    if (end>=start) *total+=end-start; else e4_clock_anomalies++;
}
static void e4_write_elapsed(uint64_t start,uint64_t end) {
    e4_elapsed(&e4_write_cycles,start,end);
    if (end>=start && end-start>e4_write_max_cycles) e4_write_max_cycles=end-start;
}
static unsigned e4_read_kind(ext4_mount_t *fs,uint64_t off) {
    if (!fs->bs) return E4_PR_OTHER;
    uint64_t block=off/fs->bs;
    if (block==1024/fs->bs) return E4_PR_SUPER;
    if (block>=fs->first+1u && block<(uint64_t)fs->first+1+fs->gdt_blocks) return E4_PR_DESCRIPTOR;
    if (fs->gd) for (uint32_t g=0;g<fs->groups;g++) {
        const uint8_t *d=fs->gd[g].raw;
        if (block==e4_u32(d) || block==e4_u32(d+4)) return E4_PR_BITMAP;
        uint64_t table=e4_u32(d+8),count=((uint64_t)fs->ipg*256+fs->bs-1)/fs->bs;
        if (block>=table && block-table<count) return E4_PR_INODE;
    }
    return E4_PR_OTHER;
}
static bool e4_bytes_impl(ext4_mount_t *fs, uint64_t off, void *out, size_t len,unsigned kind) {
    uint64_t cap=(uint64_t)fs->blocks*fs->bs;
    if (off>cap || len>cap-off) return false;
    while (len) {
        unsigned ss=fs->dev->sector_size;
        if (!(off%ss) && len>=ss) {
            size_t n=len<4096 ? len : 4096;n-=n%ss;
            __atomic_fetch_add(&e4_read_profile[kind].requests,1,__ATOMIC_RELAXED);
            if (!block_read_sectors(fs->dev,off/ss,(uint32_t)(n/ss),out)) return false;
            out=(uint8_t *)out+n;off+=n;len-=n;continue;
        }
        size_t skip=off % ss, n=ss-skip;
        if (n>len) n=len;
        __atomic_fetch_add(&e4_read_profile[kind].requests,1,__ATOMIC_RELAXED);
        if (!block_read_sector(fs->dev,off/ss,fs->sector)) return false;
        memcpy(out,fs->sector+skip,n);
        out=(uint8_t *)out+n; off+=n; len-=n;
    }
    return true;
}
static uint64_t e4_cache_hits,e4_cache_misses;
static void e4_cache_drop(ext4_mount_t *fs,bool disable) {
    for (unsigned i=0;i<8;i++) fs->metadata[i].valid=false;
    if (disable) fs->metadata_enabled=false;
}
static void e4_cache_invalidate(ext4_mount_t *fs,uint64_t off,size_t len) {
    if (!len) return;
    uint64_t first=off/fs->bs,last=(off+len-1)/fs->bs;
    for (unsigned i=0;i<8;i++) if (fs->metadata[i].valid &&
        fs->metadata[i].block>=first && fs->metadata[i].block<=last) fs->metadata[i].valid=false;
}
static unsigned e4_cache_slot(ext4_mount_t *fs,uint32_t block) {
    for (unsigned i=0;i<8;i++) if (fs->metadata[i].valid && fs->metadata[i].block==block) return i;
    for (unsigned i=0;i<8;i++) if (!fs->metadata[i].valid) return i;
    unsigned slot=fs->metadata_next;fs->metadata_next=(slot+1)%8;return slot;
}
/* Only already committed images may seed the clean cache. Callers retain
 * checksum/bitmap/inode validation on every use; no pending images enter it. */
static void e4_cache_committed(ext4_mount_t *fs,uint32_t block,const uint8_t *bytes) {
    if (!fs->metadata_enabled || e4_read_kind(fs,(uint64_t)block*fs->bs)>E4_PR_INODE) return;
    unsigned slot=e4_cache_slot(fs,block);
    fs->metadata[slot].valid=false;fs->metadata[slot].block=block;
    memcpy(fs->metadata[slot].bytes,bytes,fs->bs);fs->metadata[slot].valid=true;
}
static bool e4_cached_bytes(ext4_mount_t *fs,uint64_t off,void *out,size_t len,unsigned kind) {
    uint64_t cap=(uint64_t)fs->blocks*fs->bs;
    if (off>cap || len>cap-off) return false;
    if (!fs->metadata_enabled || kind>E4_PR_INODE || !len || len>fs->bs-off%fs->bs)
        return e4_bytes_impl(fs,off,out,len,kind);
    uint32_t block=(uint32_t)(off/fs->bs);
    for (unsigned i=0;i<8;i++) if (fs->metadata[i].valid && fs->metadata[i].block==block) {
        e4_cache_hits++;memcpy(out,fs->metadata[i].bytes+off%fs->bs,len);return true;
    }
    e4_cache_misses++;unsigned slot=e4_cache_slot(fs,block);
    fs->metadata[slot].valid=false;
    if (!e4_bytes_impl(fs,(uint64_t)block*fs->bs,fs->metadata[slot].bytes,fs->bs,kind)) return false;
    fs->metadata[slot].block=block;fs->metadata[slot].valid=true;
    memcpy(out,fs->metadata[slot].bytes+off%fs->bs,len);return true;
}
static bool e4_bytes_kind(ext4_mount_t *fs,uint64_t off,void *out,size_t len,unsigned kind) {
    e4_read_sample_t *p=&e4_read_profile[kind];uint64_t start=e4_tsc();
    bool ok=e4_cached_bytes(fs,off,out,len,kind);uint64_t end=e4_tsc();
    if (!ok) e4_cache_drop(fs,true);
    __atomic_fetch_add(&p->calls,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->bytes,len,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->failures,!ok,__ATOMIC_RELAXED);
    if (end>=start) __atomic_fetch_add(&p->cycles,end-start,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&p->anomalies,1,__ATOMIC_RELAXED);
    return ok;
}
static bool e4_bytes(ext4_mount_t *fs,uint64_t off,void *out,size_t len) {
    return e4_bytes_kind(fs,off,out,len,e4_read_kind(fs,off));
}
static bool e4_power(uint32_t n,uint32_t base) {
    while (n>1 && n%base==0) n/=base;
    return n==1;
}
static bool e4_backup(uint32_t g) {
    return g==0 || g==1 || e4_power(g,3) || e4_power(g,5) || e4_power(g,7);
}
static bool e4_reserve(ext4_mount_t *fs,uint32_t lo,uint32_t len) {
    if (!len || lo<fs->first || lo>=fs->blocks || len>fs->blocks-lo) return false;
    for (unsigned i=0;i<fs->ranges;i++) {
        e4_range_t r=fs->reserved[i];
        if (lo<r.lo+r.len && r.lo<lo+len) return false;
    }
    fs->reserved[fs->ranges++]=(e4_range_t){lo,len};
    return true;
}
/* Heap sort avoids quadratic alias/reservation work on bounded large trees. */
static void e4_range_heap(e4_range_t *ranges,unsigned root,unsigned count) {
    while (root<count/2) {
        unsigned child=root*2+1;
        if (child+1<count && ranges[child].lo<ranges[child+1].lo) child++;
        if (ranges[root].lo>=ranges[child].lo) break;
        e4_range_t swap=ranges[root];ranges[root]=ranges[child];ranges[child]=swap;root=child;
    }
}
static void e4_range_sort(e4_range_t *ranges,unsigned count) {
    for (unsigned i=count/2;i;i--) e4_range_heap(ranges,i-1,count);
    for (unsigned i=count;i>1;i--) {
        e4_range_t swap=ranges[0];ranges[0]=ranges[i-1];ranges[i-1]=swap;
        e4_range_heap(ranges,0,i-1);
    }
}
static bool e4_data_range(ext4_mount_t *fs,uint32_t lo,uint32_t len) {
    if (!len || lo<fs->first || lo>=fs->blocks || len>fs->blocks-lo) return false;
    for (unsigned i=0;i<fs->journal_ranges;i++) {
        e4_range_t r=fs->journal_reserved[i];
        if (lo<r.lo+r.len && r.lo<lo+len) return false;
    }
    if (fs->reserved_sorted) {
        unsigned left=0,right=fs->ranges;
        while (left<right) {
            unsigned mid=left+(right-left)/2;
            if (fs->reserved[mid].lo<lo+len) left=mid+1;else right=mid;
        }
        return !left || fs->reserved[left-1].lo+fs->reserved[left-1].len<=lo;
    }
    for (unsigned i=0;i<fs->ranges;i++) {
        e4_range_t r=fs->reserved[i];
        if (lo<r.lo+r.len && r.lo<lo+len) return false;
    }
    return true;
}
static bool e4_bit(const uint8_t *map,uint32_t bit) { return (map[bit/8] >> (bit%8)) & 1; }
static bool e4_bitmap(ext4_mount_t *fs,uint32_t group,bool inode) {
    const uint8_t *d=fs->gd[group].raw;
    unsigned flag=inode ? 1 : 2;
    if (e4_u16(d+18)&flag) return false;
    uint32_t block=e4_u32(d+(inode ? 4 : 0));
    size_t size=(inode ? fs->ipg : fs->bpg)/8;
    return e4_bytes(fs,(uint64_t)block*fs->bs,fs->scratch,fs->bs) &&
           (uint16_t)e4_crc(fs->seed,fs->scratch,size)==e4_u16(d+(inode ? 26 : 24));
}
static uint32_t e4_inode_seed(ext4_mount_t *fs,uint32_t ino,uint32_t generation) {
    uint8_t b[4]; e4_p32(b,ino);
    uint32_t seed=e4_crc(fs->seed,b,4); e4_p32(b,generation);
    return e4_crc(seed,b,4);
}
static int e4_inode(ext4_mount_t *fs,uint32_t ino,e4_inode_t *out) {
    if (!ino || ino>fs->inodes) return -VFS_EIO;
    uint32_t g=(ino-1)/fs->ipg, index=(ino-1)%fs->ipg;
    const uint8_t *gd=fs->gd[g].raw;
    if (e4_u16(gd+18)&1) return -VFS_EIO;
    if (fs->journal_bootstrap) {
        if (ino!=fs->journal_ino) return -VFS_EINVAL;
    } else if (!e4_bitmap(fs,g,true) ||
               !e4_bit(fs->scratch,index)) return -VFS_EIO;
    uint8_t raw[256];
    if (!e4_bytes(fs,(uint64_t)e4_u32(gd+8)*fs->bs+(uint64_t)index*256,raw,256)) return -VFS_EIO;
    uint16_t extra=e4_u16(raw+128);
    if (extra<4 || extra>128 || extra%4) return -VFS_EIO;
    uint32_t generation=e4_u32(raw+100);
    uint32_t stored=e4_u16(raw+124) | (uint32_t)e4_u16(raw+130)<<16;
    raw[124]=raw[125]=raw[130]=raw[131]=0;
    if (e4_crc(e4_inode_seed(fs,ino,generation),raw,256)!=stored) return -VFS_EIO;
    uint16_t mode=e4_u16(raw)&0xf000;
    if ((mode!=0x4000 && mode!=0x8000) || e4_u32(raw+32)!=0x80000 ||
        e4_u32(raw+104) || e4_u16(raw+118)) return -VFS_EOPNOTSUPP;
    uint64_t size=e4_u32(raw+4) | (uint64_t)e4_u32(raw+108)<<32;
    if (size>8ULL*1024*1024*1024) return -VFS_EFBIG;
    if (!e4_u16(raw+26) && fs->orphan_access!=ino) return -VFS_EIO;
    *out=(e4_inode_t){.fs=fs,.ino=ino,.generation=generation,.size=size,.mode=mode,.full_mode=e4_u16(raw),
        .uid=e4_u16(raw+2)|(uint32_t)e4_u16(raw+120)<<16,
        .gid=e4_u16(raw+24)|(uint32_t)e4_u16(raw+122)<<16};
    memcpy(out->extent,raw+40,60);
    return 0;
}
/* All referenced blocks are checked before a map is published. The immutable
 * RO map is owned by the mount, including maps built during failed admission. */
static int e4_reference(ext4_mount_t *fs,uint32_t lo,uint32_t len) {
    if (!e4_data_range(fs,lo,len)) return -VFS_EIO;
    if (fs->physical_count==E4_MAP_MAX*2) return -VFS_EFBIG;
    fs->physical[fs->physical_count++]=(e4_range_t){lo,len};
    if (fs->journal_bootstrap) return 0;
    while (len) {
        uint32_t g=(lo-fs->first)/fs->bpg, bit=(lo-fs->first)%fs->bpg;
        uint32_t n=fs->bpg-bit; if (n>len) n=len;
        if (!fs->validation_valid || fs->validation_group!=g) {
            fs->validation_valid=false;
            if (!e4_bitmap(fs,g,false)) return -VFS_EIO;
            memcpy(fs->validation_bitmap,fs->scratch,fs->bs);
            fs->validation_group=g;fs->validation_valid=true;
        }
        for (uint32_t i=0;i<n;i++) if (!e4_bit(fs->validation_bitmap,bit+i)) return -VFS_EIO;
        lo+=n; len-=n;
    }
    return 0;
}
static int e4_tree(e4_inode_t *in,const uint8_t *h,unsigned depth,
                   bool root,uint64_t lower,uint64_t upper) {
    ext4_mount_t *fs=in->fs;
    if (root) fs->validation_valid=false;
    if (++fs->visits>E4_MAP_MAX) return -VFS_EFBIG;
    unsigned count=e4_u16(h+2), maximum=root ? 4 : (fs->bs-12)/12;
    if (e4_u16(h)!=0xf30a || e4_u16(h+4)!=maximum || count>maximum ||
        e4_u16(h+6)!=depth || (!root && !count) || (depth && !count)) return -VFS_EIO;
    if (!root) {
        unsigned tail=12+maximum*12;
        if (e4_crc(e4_inode_seed(fs,in->ino,in->generation),h,tail)!=e4_u32(h+tail)) return -VFS_EIO;
    }
    uint64_t previous=lower;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *e=h+12+i*12;
        uint32_t logical=e4_u32(e);
        if (logical<previous || logical>=upper || (!root && !i && logical!=lower)) return -VFS_EIO;
        if (depth) {
            uint64_t limit=i+1<count ? e4_u32(e+12) : upper;
            uint32_t child=e4_u32(e+4);
            if (limit<=logical || limit>upper || e4_u16(e+8) || e4_u16(e+10)) return -VFS_EIO;
            int r=e4_reference(fs,child,1); if (r) return r;
            uint8_t *buffer=fs->tree[depth-1];
            if (!e4_bytes_kind(fs,(uint64_t)child*fs->bs,buffer,fs->bs,E4_PR_EXTENT)) return -VFS_EIO;
            r=e4_tree(in,buffer,depth-1,false,logical,limit); if (r) return r;
            previous=limit;
        } else {
            unsigned raw=e4_u16(e+4), len=raw>32768 ? raw-32768 : raw;
            uint32_t physical=e4_u32(e+8);
            uint64_t end=(uint64_t)logical+len;
            if (!len || e4_u16(e+6) || end>upper) return -VFS_EIO;
            if (fs->pending_count==E4_MAP_MAX) return -VFS_EFBIG;
            int r=e4_reference(fs,physical,len); if (r) return r;
            fs->pending[fs->pending_count++]=(e4_extent_t){logical,physical,len,raw>32768};
            previous=end;
        }
    }
    if (root) {
        e4_range_sort(fs->physical,fs->physical_count);
        for (unsigned i=1;i<fs->physical_count;i++)
            if (fs->physical[i].lo<fs->physical[i-1].lo+fs->physical[i-1].len) return -VFS_EIO;
    }
    return 0;
}
static int e4_mapping(e4_inode_t *in) {
    ext4_mount_t *fs=in->fs;
    if (fs->engine) {
        if (fs->rw_ino==in->ino && fs->rw_generation==in->generation) { in->map=fs->rw_map; return 0; }
        in->map=NULL;
    } else if (in->map) return 0;
    unsigned depth=e4_u16(in->extent+6);
    if (depth>2) return -VFS_EOPNOTSUPP;
    fs->visits=fs->pending_count=fs->physical_count=0;
    int r=e4_tree(in,in->extent,depth,true,0,1ULL<<32); if (r) return r;
    if (fs->pending_count>E4_MAP_MAX-fs->map_entries) return -VFS_EFBIG;
    e4_map_t *map=fs->engine ? fs->rw_map : kmalloc(sizeof(*map)+fs->pending_count*sizeof(e4_extent_t));
    if (!map) return -VFS_ENOMEM;
    map->count=fs->pending_count;
    memcpy(map->entries,fs->pending,map->count*sizeof(e4_extent_t));
    if (!fs->engine) { map->next=fs->maps; fs->maps=map; fs->map_entries+=map->count; }
    if (fs->engine) { fs->rw_ino=in->ino; fs->rw_generation=in->generation; }
    in->map=map;
    return 0;
}
static const e4_extent_t *e4_find(e4_inode_t *in,uint32_t logical) {
    unsigned lo=0, hi=in->map->count;
    while (lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if (in->map->entries[mid].logical<=logical) lo=mid+1; else hi=mid;
    }
    if (!lo) return NULL;
    const e4_extent_t *e=&in->map->entries[lo-1];
    return (uint64_t)logical< (uint64_t)e->logical+e->len ? e : NULL;
}
static int e4_dir_block(e4_inode_t *in,uint32_t logical,uint32_t *physical) {
    int r=e4_mapping(in); if (r) return r;
    const e4_extent_t *e=e4_find(in,logical);
    if (!e || e->unwritten) return -VFS_EIO;
    *physical=e->physical+logical-e->logical;
    return 0;
}
/* Scan validates the whole directory even after finding a requested entry.
 * Unsupported names/types fail visibly rather than becoming truncated aliases. */
static int e4_scan(e4_inode_t *in,const char *name,uint64_t wanted,vfs_dirent_t *out,uint32_t *found) {
    ext4_mount_t *fs=in->fs;
    if (in->mode!=0x4000) return -VFS_EINVAL;
    if (!in->size || in->size%fs->bs || in->size>E4_DIR_MAX) return -VFS_EFBIG;
    uint64_t count=0; int result=0; *found=0;
    for (uint32_t logical=0;logical<in->size/fs->bs;logical++) {
        uint32_t physical; int r=e4_dir_block(in,logical,&physical);
        if (r) return r;
        if (!e4_bytes_kind(fs,(uint64_t)physical*fs->bs,fs->scratch,fs->bs,E4_PR_DIRECTORY)) return -VFS_EIO;
        uint8_t *b=fs->scratch, *tail=b+fs->bs-12;
        if (e4_u32(tail) || e4_u16(tail+4)!=12 || tail[6] || tail[7]!=0xde ||
            e4_crc(e4_inode_seed(fs,in->ino,in->generation),b,fs->bs-12)!=e4_u32(tail+8)) return -VFS_EIO;
        for (unsigned at=0;at<fs->bs-12;) {
            uint8_t *e=b+at; uint32_t ino=e4_u32(e);
            unsigned len=e4_u16(e+4), nl=e[6], type=e[7];
            if (len<8 || len%4 || len>fs->bs-12-at || nl>len-8) return -VFS_EIO;
            if (ino) {
                if (ino>fs->inodes || !nl) return -VFS_EIO;
                if (nl>=VFS_MAX_NAME || (type!=1 && type!=2)) return -VFS_EOPNOTSUPP;
                for (unsigned k=0;k<nl;k++) if (!e[8+k] || e[8+k]=='/') return -VFS_EIO;
                bool match=name ? strlen(name)==nl && !memcmp(name,e+8,nl) : count==wanted;
                if (match) {
                    if (*found) return -VFS_EIO;
                    *found=ino; result=1;
                    if (out) {
                        memset(out,0,sizeof(*out)); memcpy(out->name,e+8,nl);
                        out->type=type==2 ? VFS_DIRECTORY : VFS_FILE;
                    }
                }
                count++;
            }
            at+=len;
        }
    }
    return result;
}
static int e4_can_write(vfs_node_t *node);
static int e4_open(vfs_node_t *node);
static void e4_close(vfs_node_t *node);
static int64_t e4_write(vfs_node_t *,uint64_t *,bool,const void *,size_t);
static vfs_node_t *e4_create(vfs_node_t *,const char *,vfs_node_type_t);
static int e4_unlink(vfs_node_t *,const char *);
static int e4_rename(vfs_node_t *,const char *,vfs_node_t *,const char *);
static int e4_truncate(vfs_node_t *,uint64_t);
static int64_t e4_jwrite(vfs_node_t *,uint64_t *,bool,const void *,size_t);
static vfs_node_t *e4_jcreate(vfs_node_t *,const char *,vfs_node_type_t);
static int e4_junlink(vfs_node_t *,const char *);
static int e4_jrename(vfs_node_t *,const char *,vfs_node_t *,const char *);
static int e4_jtruncate(vfs_node_t *,uint64_t);
static void e4_jclose(vfs_node_t *);
static int e4_rw_workspace(ext4_mount_t *);
static int64_t e4_read(vfs_node_t *node,uint64_t off,void *buf,size_t len) {
    if (!node || (!buf && len)) return -VFS_EINVAL;
    e4_inode_t *in=node->fs_private;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if (((e4_node_t *)node)->removed && !in->fs->journal_mounted) { spin_unlock_irqrestore(&e4_lock,flags); return -VFS_ENOENT; }
    if (in->fs->journal_mounted) {
        int status=e4_journal_refresh(node);
        if (status) { spin_unlock_irqrestore(&e4_lock,flags);return status; }
    }
    if (!len || off>=in->size) { spin_unlock_irqrestore(&e4_lock,flags); return 0; }
    if (len>65536) len=65536;
    if (len>in->size-off) len=(size_t)(in->size-off);
    if (in->fs->engine) in->map=NULL;
    int r=e4_mapping(in); size_t done=0;
    if (!r) while (done<len) {
        uint64_t position=off+done;
        uint32_t logical=(uint32_t)(position/in->fs->bs);
        unsigned skip=position%in->fs->bs;
        size_t n=in->fs->bs-skip; if (n>len-done) n=len-done;
        const e4_extent_t *e=e4_find(in,logical);
        if (!e || e->unwritten) memset((uint8_t *)buf+done,0,n);
        else if (!e4_bytes_kind(in->fs,(uint64_t)(e->physical+logical-e->logical)*in->fs->bs+skip,
                           (uint8_t *)buf+done,n,E4_PR_DATA)) { r=-VFS_EIO; break; }
        done+=n;
    }
    spin_unlock_irqrestore(&e4_lock,flags);
    return done ? (int64_t)done : r;
}
static vfs_node_t *e4_lookup(vfs_node_t *parent,const char *name);
static vfs_node_t *e4_lookup_ref(vfs_node_t *parent,const char *name,int *error);
static vfs_node_t *e4_jcreate_ref(vfs_node_t *parent,const char *name,vfs_node_type_t type,int *error);
static vfs_node_t *e4_jcreate_attrs(vfs_node_t *,const char *,vfs_node_type_t,const vfs_create_attrs_t *,int *);
static vfs_node_t *e4_create_attrs(vfs_node_t *,const char *,vfs_node_type_t,const vfs_create_attrs_t *,int *);

/* Cached children (including detached open nodes) keep their parent address
 * alive. Root and legacy raw-pointer nodes are never eviction candidates. */
static bool e4_cache_remove(ext4_mount_t *fs,e4_node_t *node) {
    spin_debug_assert_held(&e4_lock);
    if (!fs->journal_mounted || node==fs->cached[0] || node->refs || node->opens ||
        node->legacy || node->node.children) return false;
    for (unsigned i=0;i<fs->nodes;i++)
        if (fs->cached[i]->node.parent==&node->node) return false;
    unsigned at=1;
    while (at<fs->nodes && fs->cached[at]!=node) at++;
    if (at==fs->nodes) return false;
    vfs_node_t **link=&node->node.parent->children;
    while (*link && *link!=&node->node) link=&(*link)->next;
    if (*link) *link=node->node.next;
    fs->cached[at]=fs->cached[--fs->nodes];fs->cached[fs->nodes]=NULL;
    kfree(node);return true;
}
static void e4_collect_removed(e4_node_t *node) {
    ext4_mount_t *fs=node->inode.fs;
    while (node!=fs->cached[0] && node->removed) {
        e4_node_t *parent=(e4_node_t *)node->node.parent;
        if (!e4_cache_remove(fs,node)) break;
        node=parent;
    }
}
static bool e4_cache_room(ext4_mount_t *fs) {
    if (fs->nodes<E4_NODES) return true;
    for (unsigned i=1;i<fs->nodes;i++)
        if (e4_cache_remove(fs,fs->cached[i])) return true;
    return false;
}
static int e4_node_get(vfs_node_t *node) {
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_node_t *n=(e4_node_t *)node;
    int error=n->refs==UINT32_MAX ? -VFS_EFBIG : 0;
    if (!error) n->refs++;
    spin_unlock_irqrestore(&e4_lock,flags);return error;
}
static void e4_node_put(vfs_node_t *node) {
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_node_t *n=(e4_node_t *)node;
    if (n->refs) n->refs--;
    e4_collect_removed(n);
    spin_unlock_irqrestore(&e4_lock,flags);
}
static int e4_readdir(vfs_node_t *node,uint64_t cookie,void *out) {
    if (!node || !out) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    e4_inode_t *in=node->fs_private, child; uint32_t ino;
    if (in->fs->journal_mounted) {
        int r=e4_journal_refresh(node);if (r) { spin_unlock_irqrestore(&e4_lock,flags);return r; }
    }
    if (in->fs->engine) in->map=NULL;
    int r=e4_scan(in,NULL,cookie,out,&ino);
    if (r==1) {
        r=e4_inode(in->fs,ino,&child);
        if (!r) {
            vfs_dirent_t *de=out;
            if (de->type!=(child.mode==0x4000 ? VFS_DIRECTORY : VFS_FILE)) r=-VFS_EIO;
            else { de->size=child.size; r=1; }
        }
    }
    spin_unlock_irqrestore(&e4_lock,flags); return r;
}
static int e4_metadata(vfs_node_t *node,vfs_metadata_t *out) {
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    e4_node_t *n=(e4_node_t *)node;e4_inode_t in;ext4_mount_t *fs=n->inode.fs;
    int r=0;
    if (fs->journal_mounted) r=e4_journal_refresh(node);
    if (!r) {
        if (fs->journal_mounted) in=n->inode;
        else r=e4_inode(fs,n->inode.ino,&in);
    }
    if (!r) *out=(vfs_metadata_t){.size=in.size,.type=node->type,.mode=in.full_mode,
        .uid=in.uid,.gid=in.gid,.mnt_flags=node->mnt_flags};
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
static vfs_metadata_t e4_value(const e4_inode_t *);
static int e4_access_locked(vfs_node_t *,unsigned,const creds_t *);
static int e4_permission_actor(vfs_node_t *,unsigned,const creds_t *);
static vfs_node_t *e4_lookup_actor(vfs_node_t *,const char *,const creds_t *,int *);
static vfs_node_t *e4_create_actor(vfs_node_t *,const char *,vfs_node_type_t,uint32_t,const creds_t *,int *);
static int e4_open_actor(vfs_node_t *,unsigned,bool,const creds_t *);
static int e4_unlink_actor(vfs_node_t *,const char *,bool,const creds_t *);
static int e4_rename_actor(vfs_node_t *,const char *,vfs_node_t *,const char *,bool,const creds_t *);
static int e4_setattr_actor(vfs_node_t *,bool,uint32_t,creds_id_change_t,creds_id_change_t,const creds_t *);
static int64_t e4_write_actor(vfs_node_t *,uint64_t *,bool,const void *,size_t,const creds_t *);
#endif
static void e4_setup(e4_node_t *n, e4_inode_t *in) {
    n->inode=*in; n->node.fs_private=&n->inode; n->node.size=in->size;
    n->node.type=in->mode==0x4000 ? VFS_DIRECTORY : VFS_FILE;
    n->node.mode=in->full_mode;n->node.uid=in->uid;n->node.gid=in->gid;
    n->node.mnt_flags=(in->fs->engine ? 0 : VFS_MNT_RDONLY)|VFS_MNT_NOSUID|VFS_MNT_NODEV;
    n->node.metadata=e4_metadata;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    n->node.permission_actor=e4_permission_actor;n->node.open_actor=e4_open_actor;
    n->node.setattr_actor=e4_setattr_actor;
    if (in->mode==VFS_S_IFDIR) {
        n->node.lookup_actor=e4_lookup_actor;n->node.create_actor=e4_create_actor;
        n->node.unlink_actor=e4_unlink_actor;n->node.rename_actor=e4_rename_actor;
    } else n->node.write_actor=e4_write_actor;
#endif
    n->node.can_write=e4_can_write;
    if (in->fs->engine) {
        n->node.owns_nodes=true; n->node.rename_no_replace=true;
        n->node.serializes_write_offset=true;
        n->node.open=e4_open; n->node.close=e4_close;
        if (in->mode==0x4000) { n->node.create=e4_create;n->node.create_attrs_ref=e4_create_attrs; n->node.unlink=e4_unlink; n->node.rename=e4_rename; }
        else { n->node.write=e4_write; n->node.truncate=e4_truncate; }
        if (in->fs->journal_mounted) {
            n->node.get=e4_node_get;n->node.put=e4_node_put;
            n->node.close=e4_jclose;
            if (in->mode==0x4000) {
                n->node.lookup_ref=e4_lookup_ref;n->node.create_ref=e4_jcreate_ref;
                n->node.create=e4_jcreate;n->node.create_attrs_ref=e4_jcreate_attrs;n->node.unlink=e4_junlink;n->node.rename=e4_jrename;
            }
            else { n->node.write=e4_jwrite;n->node.truncate=e4_jtruncate; }
        }
    }
    if (n->node.type==VFS_DIRECTORY) { n->node.lookup=e4_lookup; n->node.readdir=e4_readdir; }
    else n->node.read=e4_read;
}
static vfs_node_t *e4_lookup_common(vfs_node_t *parent,const char *name,int *error,bool owned,const creds_t *actor) {
    if (error) *error=-VFS_EINVAL;
    if (!parent || !name || !*name || strlen(name)>=VFS_MAX_NAME) return NULL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    e4_inode_t *in=parent->fs_private, child; ext4_mount_t *fs=in->fs; uint32_t ino;
    int status=fs->journal_mounted ? e4_journal_refresh(parent) : 0;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (!status && actor) status=e4_access_locked(parent,VFS_MAY_EXEC,actor);
#else
    (void)actor;
#endif
    if (status) { if (error) *error=status;spin_unlock_irqrestore(&e4_lock,flags);return NULL; }
    vfs_dirent_t entry;
    if (fs->engine) in->map=NULL;
    int r=e4_scan(in,name,0,&entry,&ino); vfs_node_t *result=NULL;
    status=r<0 ? r : -VFS_ENOENT;
    int inode_error=r==1 ? e4_inode(fs,ino,&child) : 0;
    if (inode_error) status=inode_error;
    if (r==1 && !inode_error &&
        entry.type==(child.mode==0x4000 ? VFS_DIRECTORY : VFS_FILE)) {
        if (!strcmp(name,".")) {
            result=ino==in->ino ? parent : NULL;
            goto publish;
        }
        if (!strcmp(name,"..")) {
            if (parent==&fs->cached[0]->node) {
                result=ino==in->ino ? parent->parent : NULL;
                goto publish;
            }
            vfs_node_t *up=parent->parent;
            e4_inode_t *up_inode=up ? up->fs_private : NULL;
            result=up_inode && up_inode->fs==fs && up_inode->ino==ino ? up : NULL;
            goto publish;
        }
        for (vfs_node_t *p=parent->children;p;p=p->next) if (!strcmp(p->name,name)) { result=p; break; }
        size_t plen=strlen(parent->path), nl=strlen(name);
        if (!result && plen+1+nl<VFS_MAX_PATH && e4_cache_room(fs)) {
            e4_node_t *n=kcalloc(1,sizeof(*n));
            status=-VFS_ENOMEM;
            if (n) {
                memcpy(n->node.name,name,nl+1); memcpy(n->node.path,parent->path,plen);
                n->node.path[plen]='/'; memcpy(n->node.path+plen+1,name,nl+1);
                n->node.parent=parent; e4_setup(n,&child);
                n->node.next=parent->children; parent->children=&n->node;
                fs->cached[fs->nodes++]=n; result=&n->node;
            }
        } else if (!result) status=-VFS_EFBIG;
    }
publish:
    if (result && result->get==e4_node_get) {
        e4_node_t *node=(e4_node_t *)result;
        if (owned) {
            if (node->refs==UINT32_MAX) { result=NULL;status=-VFS_EFBIG; }
            else node->refs++;
        } else node->legacy=true;
    }
    if (error) *error=result ? 0 : status;
    spin_unlock_irqrestore(&e4_lock,flags); return result;
}
static vfs_node_t *e4_lookup(vfs_node_t *parent,const char *name) {
    return e4_lookup_common(parent,name,NULL,false,NULL);
}
static vfs_node_t *e4_lookup_ref(vfs_node_t *parent,const char *name,int *error) {
    return e4_lookup_common(parent,name,error,true,NULL);
}
static void e4_discard(ext4_mount_t *fs) {
    if (!fs) return;
    for (unsigned i=0;i<fs->nodes;i++) kfree(fs->cached[i]);
    while (fs->maps) { e4_map_t *map=fs->maps; fs->maps=map->next; kfree(map); }
    kfree(fs->journal_reserved);kfree(fs->rw_map); kfree(fs->engine); kfree(fs->reserved); kfree(fs->gd); kfree(fs);
}
/* s_last_orphan (232) is mutable transaction state, not journal identity. */
static const unsigned e4_journal_identity[][2]={{0,8},{20,24},{76,4},{88,2},{92,4},{100,20},{208,24},{254,2},{373,1}};
static bool e4_journal_backup(ext4_mount_t *fs,uint8_t *sb) {
    /* Recovery-only bootstrap after a sector-atomic partial SB checkpoint.
     * A checksummed group-1 backup must corroborate every immutable identity
     * field. Never repair or use this path for ordinary mount admission. */
    uint32_t shift=e4_u32(sb+24),bpg=e4_u32(sb+32),first=e4_u32(sb+20);
    if (shift>2 || !bpg || bpg>(1024u<<shift)*8 || first!=(shift==0)) return false;
    uint8_t backup[1024];uint64_t where=(uint64_t)(first+bpg)*(1024u<<shift);
    if (!e4_bytes(fs,where,backup,1024) || e4_u16(backup+56)!=0xef53 ||
        e4_u16(backup+90)!=1 || e4_u32(backup+1020)!=e4_crc(UINT32_MAX,backup,1020)) return false;
    for (unsigned i=0;i<sizeof(e4_journal_identity)/sizeof(e4_journal_identity[0]);i++)
        if (memcmp(sb+e4_journal_identity[i][0],backup+e4_journal_identity[i][0],e4_journal_identity[i][1])) return false;
    if ((e4_u32(sb+96)&~4u)!=(e4_u32(backup+96)&~4u)) return false;
    memcpy(sb,backup,1024);return true;
}
static bool e4_journal_descriptor_backup(ext4_mount_t *fs,uint32_t group,uint8_t *primary) {
    /* Recovery-only identity bootstrap for a torn descriptor checkpoint.
     * Borrow a checksummed backup in memory; NEVER repair primary media here.
     * Replay preview must still validate the authoritative descriptor/bitmaps. */
    if (fs->groups<2) return false;
    uint8_t backup[32],copy[32],number[4];
    uint64_t where=((uint64_t)fs->first+fs->bpg+1)*fs->bs+(uint64_t)group*32;
    if (!e4_bytes(fs,where,backup,sizeof(backup)) || memcmp(primary,backup,12)) return false;
    memcpy(copy,backup,sizeof(copy));copy[30]=copy[31]=0;e4_p32(number,group);
    if ((uint16_t)e4_crc(e4_crc(fs->seed,number,4),copy,32)!=e4_u16(backup+30)) return false;
    memcpy(primary,backup,32);return true;
}
static int e4_admit(ext4_mount_t *fs) {
    uint8_t sb[1024];
    if (!e4_bytes(fs,1024,sb,sizeof(sb))) return -VFS_EIO;
    if (e4_u16(sb+56)!=0xef53) return -VFS_EIO;
    if (e4_u32(sb+1020)!=e4_crc(UINT32_MAX,sb,1020) &&
        (!fs->journal_bootstrap || !e4_journal_backup(fs,sb))) return -VFS_EIO;
    if (e4_u32(sb+92)!=(fs->journal_bootstrap ? 4u : 0u) ||
        (fs->journal_bootstrap ? (e4_u32(sb+96)&~4u)!=0x42 : e4_u32(sb+96)!=0x42) || e4_u32(sb+100)!=0x403 ||
        e4_u32(sb+72) || e4_u32(sb+76)!=1 || e4_u16(sb+88)!=256 || sb[373]!=1 ||
        (!fs->journal_bootstrap && e4_u32(sb+224)) || e4_u32(sb+228) ||
        (!fs->journal_bootstrap && e4_u32(sb+232)) || e4_u16(sb+206)) return -VFS_EOPNOTSUPP;
    if (fs->journal_bootstrap) {
        fs->journal_ino=e4_u32(sb+224);
        if (!fs->journal_ino || fs->journal_ino>e4_u32(sb) || e4_u32(sb+228) ||
            e4_u16(sb+58)>1) return -VFS_EIO;
        uint32_t orphan=e4_u32(sb+232);
        if (orphan && (orphan<11 || orphan>e4_u32(sb) || orphan==fs->journal_ino)) return -VFS_EIO;
    } else if (e4_u16(sb+58)!=1 || e4_u32(sb+336) || e4_u32(sb+340) || e4_u32(sb+344)) return -VFS_EIO;
    uint32_t shift=e4_u32(sb+24);
    if (shift>2 || e4_u32(sb+28)!=shift) return -VFS_EOPNOTSUPP;
    fs->bs=1024u<<shift; fs->blocks=e4_u32(sb+4); fs->inodes=e4_u32(sb);
    fs->first=e4_u32(sb+20); fs->bpg=e4_u32(sb+32); fs->ipg=e4_u32(sb+40);
    if (fs->first!=(fs->bs==1024) || !fs->bpg || fs->bpg>fs->bs*8 || fs->bpg%8 ||
        !fs->ipg || fs->ipg>fs->bs*8 || fs->ipg%8 || e4_u32(sb+36)!=fs->bpg ||
        fs->blocks<=fs->first || !fs->inodes || fs->inodes>1048576 ||
        e4_u32(sb+12)>fs->blocks || e4_u32(sb+16)>fs->inodes) return -VFS_EIO;
    uint64_t bytes=(uint64_t)fs->blocks*fs->bs;
    if (bytes>8ULL*1024*1024*1024) return -VFS_EFBIG;
    if (bytes>fs->dev->sector_count*fs->dev->sector_size) return -VFS_EIO;
    fs->groups=(fs->blocks-fs->first+fs->bpg-1)/fs->bpg;
    if (!fs->groups || fs->groups>E4_GROUPS) return -VFS_EFBIG;
    if ((uint64_t)fs->groups*fs->ipg!=fs->inodes) return -VFS_EIO;
    fs->seed=e4_crc(UINT32_MAX,sb+104,16);
    fs->gdt_blocks=(fs->groups*32+fs->bs-1)/fs->bs;
    fs->gd=kcalloc(fs->groups,sizeof(*fs->gd));
    fs->reserved=kcalloc(fs->groups*4,sizeof(*fs->reserved));
    if (!fs->gd || !fs->reserved) return -VFS_ENOMEM;
    if (!e4_bytes(fs,(uint64_t)(fs->first+1)*fs->bs,fs->gd,fs->groups*32)) return -VFS_EIO;
    uint32_t itable=(fs->ipg*256+fs->bs-1)/fs->bs;
    uint64_t free_blocks=0, free_inodes=0;
    for (uint32_t g=0;g<fs->groups;g++) {
        uint8_t *d=fs->gd[g].raw, number[4], copy[32];
        memcpy(copy,d,32); copy[30]=copy[31]=0; e4_p32(number,g);
        if ((uint16_t)e4_crc(e4_crc(fs->seed,number,4),copy,32)!=e4_u16(d+30) &&
            (!fs->journal_bootstrap || !e4_journal_descriptor_backup(fs,g,d))) return -VFS_EIO;
        uint32_t start=fs->first+g*fs->bpg, end=fs->blocks-start;
        if (end>fs->bpg) end=fs->bpg;
        if (e4_u16(d+18)&~7u || e4_u32(d+20) || e4_u16(d+12)>end ||
            e4_u16(d+14)>fs->ipg || e4_u16(d+16)>fs->ipg || e4_u16(d+28)>fs->ipg) return -VFS_EIO;
        if ((e4_u16(d+18)&1) && (e4_u16(d+14)!=fs->ipg || e4_u16(d+16))) return -VFS_EIO;
        free_blocks+=e4_u16(d+12); free_inodes+=e4_u16(d+14);
        if (e4_backup(g) && !e4_reserve(fs,start,1+fs->gdt_blocks)) return -VFS_EIO;
        uint32_t b=e4_u32(d), i=e4_u32(d+4), t=e4_u32(d+8);
        if (b<start || b>=start+end || i<start || i>=start+end || t<start ||
            t>=start+end || itable>start+end-t || !e4_reserve(fs,b,1) ||
            !e4_reserve(fs,i,1) || !e4_reserve(fs,t,itable)) return -VFS_EIO;
        /* UNINIT bitmaps have no valid on-disk contents. They cannot contain
         * accessible inodes/data; skip their checksum until initialized later. */
        if (!fs->journal_bootstrap && !(e4_u16(d+18)&2)) {
            if (!e4_bytes(fs,(uint64_t)b*fs->bs,fs->scratch,fs->bs) ||
                (uint16_t)e4_crc(fs->seed,fs->scratch,fs->bpg/8)!=e4_u16(d+24)) return -VFS_EIO;
            for (unsigned j=0;j<fs->ranges;j++) {
                e4_range_t range=fs->reserved[j];
                if (range.lo>=start && range.lo<start+end)
                    for (uint32_t k=0;k<range.len;k++)
                        if (!e4_bit(fs->scratch,range.lo-start+k)) return -VFS_EIO;
            }
        }
        if (!fs->journal_bootstrap && !(e4_u16(d+18)&1)) {
            if (!e4_bytes(fs,(uint64_t)i*fs->bs,fs->scratch,fs->bs) ||
                (uint16_t)e4_crc(fs->seed,fs->scratch,fs->ipg/8)!=e4_u16(d+26)) return -VFS_EIO;
        }
    }
    if (!fs->journal_bootstrap && (free_blocks!=e4_u32(sb+12) || free_inodes!=e4_u32(sb+16))) return -VFS_EIO;
    e4_range_sort(fs->reserved,fs->ranges);fs->reserved_sorted=true;
    return 0;
}
static int e4_mount(block_dev_t *dev,const char *path,ext4_mount_t **out,bool rw) {
    spin_debug_assert_unheld();
    if (out) *out=NULL;
    if (rw && (!dev || !dev->write_sector || !dev->flush)) return -VFS_EROFS;
    if (!out || !dev || !dev->read_sector || !path || strcmp(path,"/mnt") ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    ext4_mount_t *fs=kcalloc(1,sizeof(*fs));
    if (!fs) return -VFS_ENOMEM;
    int r=rw ? e4_rw_workspace(fs) : 0;
    if (r) { e4_discard(fs); return r; }
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || e4_engine_busy || vfs_lookup_kernel(path)) { r=-VFS_EEXIST; goto fail; }
    fs->dev=dev; fs->bs=dev->sector_size;
    /* Initial byte-reader ceiling is the actual device until SB validated. */
    if (dev->sector_count>UINT32_MAX) { r=-VFS_EFBIG; goto fail; }
    fs->blocks=(uint32_t)dev->sector_count;
    r=e4_admit(fs); if (r) goto fail;
    fs->metadata_enabled=rw;
    e4_inode_t root; r=e4_inode(fs,2,&root); if (r) goto fail;
    if (root.mode!=0x4000) { r=-VFS_EIO; goto fail; }
    uint32_t ino; r=e4_scan(&root,NULL,UINT64_MAX,NULL,&ino); if (r<0) goto fail;
    r=e4_scan(&root,".",0,NULL,&ino);
    if (r!=1 || ino!=2) { r=-VFS_EIO; goto fail; }
    r=e4_scan(&root,"..",0,NULL,&ino);
    if (r!=1 || ino!=2) { r=-VFS_EIO; goto fail; }
    vfs_node_t *parent=vfs_lookup_kernel("/");
    if (!parent) { r=-VFS_EINVAL; goto fail; }
    e4_node_t *node=kcalloc(1,sizeof(*node));
    if (!node) { r=-VFS_ENOMEM; goto fail; }
    memcpy(node->node.name,"mnt",4); memcpy(node->node.path,"/mnt",5);
    node->node.parent=parent; e4_setup(node,&root);
    fs->cached[fs->nodes++]=node;
    node->node.next=parent->children; parent->children=&node->node;
    e4_active=fs; *out=fs; r=0; goto done;
fail:
    e4_discard(fs);
done:
    spin_unlock_irqrestore(&e4_lock,flags); return r;
}
int ext4_mount_ro(block_dev_t *dev,const char *path,ext4_mount_t **out) {
    return e4_mount(dev,path,out,false);
}
int ext4_mount_rw(block_dev_t *dev,const char *path,ext4_mount_t **out) {
    return e4_mount(dev,path,out,true);
}

#include "ext4_mutate.inc"
#include "ext4_write.inc"
#include "ext4_journal.inc"
#include "ext4_transaction.inc"
#include "ext4_namespace.inc"
#include "ext4_orphan.inc"
#include "ext4_mount_journal.inc"
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
#include "ext4_permissions.inc"
#endif

static size_t e4_profile_text(char *out,size_t cap,size_t n,const char *text) {
    while (*text) { if (n<cap) out[n++]=*text; text++; } return n;
}
static size_t e4_profile_number(char *out,size_t cap,size_t n,uint64_t value) {
    char digits[20];unsigned count=0;
    do { digits[count++]=(char)('0'+value%10);value/=10; } while (value);
    while (count) { char c=digits[--count];if (n<cap) out[n++]=c; } return n;
}
size_t ext4_io_profile_format(char *out,size_t capacity) {
    if (!out || !capacity) return 0;
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);size_t n=0;
    static const char *names[]={"super","descriptor","bitmap","inode","extent","directory","data","other"};
#define E4_PT(text) n=e4_profile_text(out,capacity,n,text)
#define E4_PN(value) n=e4_profile_number(out,capacity,n,value)
    for (unsigned i=0;i<E4_PR_COUNT;i++) {
        e4_read_sample_t *p=&e4_read_profile[i];
        E4_PT("[EXT4 PERF] read ");E4_PT(names[i]);
        E4_PT(" calls=");E4_PN(__atomic_load_n(&p->calls,__ATOMIC_RELAXED));
        E4_PT(" requests=");E4_PN(__atomic_load_n(&p->requests,__ATOMIC_RELAXED));
        E4_PT(" bytes=");E4_PN(__atomic_load_n(&p->bytes,__ATOMIC_RELAXED));
        E4_PT(" cycles=");E4_PN(__atomic_load_n(&p->cycles,__ATOMIC_RELAXED));
        E4_PT(" failures=");E4_PN(__atomic_load_n(&p->failures,__ATOMIC_RELAXED));
        E4_PT(" anomalies=");E4_PN(__atomic_load_n(&p->anomalies,__ATOMIC_RELAXED));E4_PT("\n");
    }
    E4_PT("[EXT4 PERF] write calls=");E4_PN(e4_write_calls);
    E4_PT(" bytes=");E4_PN(e4_write_bytes_total);
    E4_PT(" total-cycles=");E4_PN(e4_write_cycles);
    E4_PT(" failures=");E4_PN(e4_write_failures);
    E4_PT(" anomalies=");E4_PN(e4_clock_anomalies);E4_PT("\n");
    E4_PT("[EXT4 PERF] phases plan-cycles=");E4_PN(e4_plan_cycles);
    E4_PT(" commit-cycles=");E4_PN(e4_commit_cycles);
    E4_PT(" refresh-cycles=");E4_PN(e4_refresh_cycles);
    E4_PT("\n[EXT4 PERF] lock max-cycles=");E4_PN(e4_write_max_cycles);
    E4_PT(" wait-cycles=");E4_PN(e4_write_lock_wait_cycles);E4_PT("\n");
    E4_PT("[EXT4 PERF] checksum calls=");E4_PN(__atomic_load_n(&e4_crc_calls,__ATOMIC_RELAXED));
    E4_PT(" bytes=");E4_PN(__atomic_load_n(&e4_crc_bytes,__ATOMIC_RELAXED));
    E4_PT(" cycles=");E4_PN(__atomic_load_n(&e4_crc_cycles,__ATOMIC_RELAXED));
    E4_PT(" anomalies=");E4_PN(__atomic_load_n(&e4_crc_anomalies,__ATOMIC_RELAXED));E4_PT("\n");
    E4_PT("[EXT4 PERF] cache hits=");E4_PN(e4_cache_hits);
    E4_PT(" misses=");E4_PN(e4_cache_misses);E4_PT("\n");
#undef E4_PT
#undef E4_PN
    spin_unlock_irqrestore(&e4_lock,flags);return n;
}
