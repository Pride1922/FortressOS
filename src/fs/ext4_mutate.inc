/* Included once by ext4.c: shares the validated reader/layout implementation. */
#include "ext4_engine.h"
#define E4_IMAGES 64u
#define E4_FREES (E4_MAP_MAX+64u)
enum { E4_ALLOC, E4_REFERENCE, E4_NEW, E4_DATA };
typedef struct { uint32_t block; unsigned role; bool ordered; uint8_t bytes[4096]; } e4_image_t;
struct ext4_engine {
    ext4_mount_t *fs;
    e4_image_t image[E4_IMAGES];
    uint8_t write_run[BLOCK_MAX_RUN_BYTES]; /* Heap workspace, bounded block API run. */
    unsigned images, frees, old_count;
    e4_range_t released[E4_FREES], zero[64];
    uint32_t old_nodes[64], new_nodes[64], keys[64];
    unsigned zeros;
    int error, reserved_inodes, inode_delta;
    uint32_t reserved[64], reserve_ino, release_ino;
    bool ready, dirty, tainted, release_dir;
    jbd2_writer_t *writer;
    void *journal_guard;
    jbd2_credits_t credits;
    uint32_t revokes[64];
    unsigned revoke_count;
    bool file_enabled, file_planning, namespace_enabled;
    bool orphan_enabled, orphan_recovery_required;
    uint32_t orphans[64];
    unsigned orphan_count;
    uint64_t handle_serial;
    struct { uint64_t token, offset; uint32_t ino, generation, refs; } handles[16];
    uint8_t orphan_tree[2][4096]; /* No recursive 4KiB stack buffers. */
};
static void e4_engine_owner(ext4_engine_t *e) {
    if (e && e->fs->journal_mounted) spin_debug_assert_held(&e4_lock);
    else spin_debug_assert_unheld();
}
static int e4_transaction_commit(ext4_engine_t *e);
static void e4_p16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void e4_set_bit(uint8_t *map,uint32_t bit,bool value) {
    uint8_t mask=(uint8_t)(1u<<(bit%8));
    if (value) map[bit/8]|=mask; else map[bit/8]&=(uint8_t)~mask;
}
static uint8_t *e4_stage_image(ext4_engine_t *e,uint32_t block,unsigned role,bool zero) {
    if (e->error) return NULL;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) {
        if (e->image[i].role!=role) { e->error=-VFS_EIO; return NULL; }
        return e->image[i].bytes;
    }
    if (e->images==E4_IMAGES) { e->error=-VFS_EFBIG; return NULL; }
    e4_image_t *image=&e->image[e->images++]; image->block=block; image->role=role;
    image->ordered=false;
    if (zero) memset(image->bytes,0,e->fs->bs);
    else if (!e4_bytes(e->fs,(uint64_t)block*e->fs->bs,image->bytes,e->fs->bs)) {
        e->error=-VFS_EIO; return NULL;
    }
    return image->bytes;
}
static uint8_t *e4_stage(ext4_engine_t *e,uint32_t block,unsigned role) {
    return e4_stage_image(e,block,role,false);
}
/* New/unwritten storage or a caller replacing every byte may skip the read.
 * Partial initialized writes preserve untouched bytes. Staged images remain. */
static uint8_t *e4_stage_zero(ext4_engine_t *e,uint32_t block) {
    return e4_stage_image(e,block,E4_NEW,true);
}
static uint8_t *e4_stage_data(ext4_engine_t *e,uint32_t block,bool zero) {
    uint8_t *bytes=e4_stage_image(e,block,E4_NEW,zero);
    if (bytes) for (unsigned i=0;i<e->images;i++)
        if (e->image[i].block==block) e->image[i].ordered=true;
    return bytes;
}
static uint8_t *e4_super(ext4_engine_t *e) {
    uint32_t block=1024/e->fs->bs;bool existed=false;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) existed=true;
    uint8_t *b=e4_stage(e,block,E4_ALLOC);if (!b) return NULL;
    uint8_t *sb=b+1024%e->fs->bs;
    if (!existed) {
        uint64_t blocks=0,inodes=0;
        for (uint32_t g=0;g<e->fs->groups;g++) { blocks+=e4_u16(e->fs->gd[g].raw+12);inodes+=e4_u16(e->fs->gd[g].raw+14); }
        if (e4_u32(sb+1020)!=e4_crc(UINT32_MAX,sb,1020) || e4_u16(sb+58)!=(e->dirty ? 0 : 1) ||
            e4_u32(sb+12)!=blocks || e4_u32(sb+16)!=inodes || e4_u32(sb+92)!=(e->writer ? 4u : 0u) ||
            e4_u32(sb+96)!=(e->writer ? 0x46u : 0x42u) || e4_u32(sb+100)!=0x403) { e->error=-VFS_EIO;return NULL; }
    }
    return sb;
}
static uint8_t *e4_descriptor(ext4_engine_t *e,uint32_t group) {
    uint32_t offset=group*32,block=e->fs->first+1+offset/e->fs->bs;
    bool existed=false;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) existed=true;
    uint8_t *b=e4_stage(e,block,E4_ALLOC);if (!b) return NULL;
    if (!existed) {
        uint32_t first=offset/e->fs->bs*(e->fs->bs/32);
        for (uint32_t g=first;g<e->fs->groups && g<first+e->fs->bs/32;g++)
            if (memcmp(b+(g-first)*32,e->fs->gd[g].raw,32)) { e->error=-VFS_EIO;return NULL; }
    }
    return b+offset%e->fs->bs;
}
static uint8_t *e4_stage_bitmap(ext4_engine_t *e,uint32_t group,bool inode) {
    ext4_mount_t *fs=e->fs;
    uint8_t *d=e4_descriptor(e,group); if (!d) return NULL;
    uint32_t block=e4_u32(d+(inode ? 4 : 0));
    for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) return e->image[i].bytes;
    uint8_t *b=e4_stage(e,block,E4_ALLOC); if (!b) return NULL;
    unsigned flag=inode ? 1 : 2;
    if (e4_u16(d+18)&flag) {
        /* Only eagerly zeroed tables may be initialized by this bounded engine. */
        if (inode && !(e4_u16(d+18)&4)) { e->error=-VFS_EOPNOTSUPP; return NULL; }
        memset(b,0,fs->bs);
        uint32_t count=inode ? fs->ipg : fs->blocks-(fs->first+group*fs->bpg);
        if (!inode && count>fs->bpg) count=fs->bpg;
        for (uint32_t bit=count;bit<fs->bs*8;bit++) e4_set_bit(b,bit,true);
        if (!inode) {
            uint32_t start=fs->first+group*fs->bpg;
            for (unsigned i=0;i<fs->ranges;i++) {
                e4_range_t r=fs->reserved[i];
                if (r.lo>=start && r.lo<start+count)
                    for (uint32_t k=0;k<r.len;k++) e4_set_bit(b,r.lo-start+k,true);
            }
        }
        e4_p16(d+18,(uint16_t)(e4_u16(d+18)&~flag));
    } else {
        size_t len=(inode ? fs->ipg : fs->bpg)/8;
        if ((uint16_t)e4_crc(fs->seed,b,len)!=e4_u16(d+(inode ? 26 : 24))) {
            e->error=-VFS_EIO; return NULL;
        }
    }
    uint32_t count=inode ? fs->ipg : fs->blocks-(fs->first+group*fs->bpg);
    if (!inode && count>fs->bpg) count=fs->bpg;
    uint32_t free=0;for (uint32_t bit=0;bit<count;bit++) if (!e4_bit(b,bit)) free++;
    if (free!=e4_u16(d+(inode ? 14 : 12))) { e->error=-VFS_EIO;return NULL; }
    return b;
}
static bool e4_account(ext4_engine_t *e,uint32_t group,bool inode,int delta) {
    uint8_t *d=e4_descriptor(e,group), *sb=e4_super(e);
    if (!d || !sb) return false;
    unsigned offset=inode ? 14 : 12, total=inode ? 16 : 12;
    int64_t n=(int64_t)e4_u16(d+offset)+delta, sum=(int64_t)e4_u32(sb+total)+delta;
    if (n<0 || n>UINT16_MAX || sum<0 || sum>UINT32_MAX) { e->error=-VFS_EIO; return false; }
    e4_p16(d+offset,(uint16_t)n); e4_p32(sb+total,(uint32_t)sum); return true;
}
static uint32_t e4_allocate(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;
    for (uint32_t g=0;g<fs->groups;g++) {
        /* Consult staged counts when present, without consuming workspace for
         * full groups. Every selected group is still checksum-validated. */
        uint32_t offset=g*32, block=fs->first+1+offset/fs->bs;
        const uint8_t *d=fs->gd[g].raw;
        for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block) d=e->image[i].bytes+offset%fs->bs;
        if (!e4_u16(d+12)) continue;
        uint8_t *map=e4_stage_bitmap(e,g,false); if (!map) return 0;
        uint32_t start=fs->first+g*fs->bpg, count=fs->blocks-start;
        if (count>fs->bpg) count=fs->bpg;
        for (uint32_t bit=0;bit<count;bit++) if (!e4_bit(map,bit)) {
            if (!e4_data_range(fs,start+bit,1)) { e->error=-VFS_EIO; return 0; }
            /* Released blocks stay unavailable until after publication. */
            bool released=false;
            for (unsigned i=0;i<e->frees;i++) if (start+bit>=e->released[i].lo &&
                start+bit-e->released[i].lo<e->released[i].len) released=true;
            if (released) continue;
            e4_set_bit(map,bit,true);
            if (!e4_account(e,g,false,-1)) return 0;
            return start+bit;
        }
    }
    e->error=-VFS_ENOSPC; return 0;
}
static bool e4_release(ext4_engine_t *e,uint32_t block,uint32_t len) {
    ext4_mount_t *fs=e->fs;
    if (!e4_data_range(fs,block,len)) { e->error=-VFS_EIO; return false; }
    if (e->frees==E4_FREES) { e->error=-VFS_EFBIG; return false; }
    e->released[e->frees++]=(e4_range_t){block,len};
    while (len) {
        uint32_t g=(block-fs->first)/fs->bpg, bit=(block-fs->first)%fs->bpg;
        uint32_t n=fs->bpg-bit; if (n>len) n=len;
        uint8_t *map=e4_stage_bitmap(e,g,false); if (!map) return false;
        for (uint32_t k=0;k<n;k++) {
            if (!e4_bit(map,bit+k)) { e->error=-VFS_EIO; return false; }
            e4_set_bit(map,bit+k,false);
        }
        if (!e4_account(e,g,false,(int)n)) return false;
        block+=n; len-=n;
    }
    return true;
}
static void e4_header(uint8_t *h,unsigned count,unsigned max,unsigned depth) {
    e4_p16(h,0xf30a); e4_p16(h+2,(uint16_t)count);
    e4_p16(h+4,(uint16_t)max); e4_p16(h+6,(uint16_t)depth);
}
static void e4_leaf(uint8_t *b,const e4_extent_t *x) {
    e4_p32(b,x->logical); e4_p16(b+4,(uint16_t)(x->len+(x->unwritten ? 32768 : 0)));
    e4_p32(b+8,x->physical);
}
static void e4_index(uint8_t *b,uint32_t key,uint32_t block) { e4_p32(b,key); e4_p32(b+4,block); }
static bool e4_old_tree(ext4_engine_t *e,const uint8_t *h,unsigned depth) {
    if (!depth) return true;
    for (unsigned i=0;i<e4_u16(h+2);i++) {
        if (e->old_count==64) { e->error=-VFS_EFBIG; return false; }
        uint32_t block=e4_u32(h+16+i*12); e->old_nodes[e->old_count++]=block;
        uint8_t *b=e->fs->tree[depth-1];
        if (!e4_bytes(e->fs,(uint64_t)block*e->fs->bs,b,e->fs->bs)) { e->error=-VFS_EIO; return false; }
        if (!e4_old_tree(e,b,depth-1)) return false;
    }
    return true;
}
static void e4_inode_sum(ext4_mount_t *fs,uint32_t ino,uint8_t *raw) {
    e4_p16(raw+124,0); e4_p16(raw+130,0);
    uint32_t sum=e4_crc(e4_inode_seed(fs,ino,e4_u32(raw+100)),raw,256);
    e4_p16(raw+124,(uint16_t)sum); e4_p16(raw+130,(uint16_t)(sum>>16));
}
static uint8_t *e4_inode_image(ext4_engine_t *e,uint32_t ino) {
    ext4_mount_t *fs=e->fs;
    if ((ino<11 && ino!=2) || ino>fs->inodes) { e->error=-VFS_EINVAL; return NULL; }
    uint32_t g=(ino-1)/fs->ipg, index=(ino-1)%fs->ipg, offset=index*256;
    uint8_t *b=e4_stage(e,e4_u32(fs->gd[g].raw+8)+offset/fs->bs,E4_REFERENCE);
    return b ? b+offset%fs->bs : NULL;
}
static int e4_plan_begin(ext4_engine_t *e) {
    if (!e) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->writer && !e->file_planning) return -VFS_EOPNOTSUPP;
    if (e->ready) return -VFS_EINVAL;
    e->images=e->frees=e->old_count=e->zeros=0;
    e->error=e->inode_delta=0; e->reserve_ino=e->release_ino=0; e->release_dir=false;
    return 0;
}
static int e4_plan_end(ext4_engine_t *e) {
    if (!e->error) (void)e4_super(e);
    e->ready=!e->error;
    return e->error;
}
static void e4_rebuild(ext4_engine_t *e,e4_inode_t *in,uint8_t *raw) {
    ext4_mount_t *fs=e->fs; unsigned count=fs->pending_count, capacity=(fs->bs-12)/12;
    memset(raw+40,0,60);
    unsigned leaves=count>4 ? (count+capacity-1)/capacity : 0;
    unsigned indexes=leaves>4 ? (leaves+capacity-1)/capacity : 0;
    if (indexes>4 || leaves+indexes>64) { e->error=-VFS_EFBIG; return; }
    e4_header(raw+40,leaves ? (indexes ? indexes : leaves) : count,4,indexes ? 2 : leaves ? 1 : 0);
    uint32_t seed=e4_inode_seed(fs,in->ino,in->generation);
    uint64_t blocks=leaves+indexes;
    for (unsigned i=0;i<count;i++) blocks+=fs->pending[i].len;
    if (blocks>UINT32_MAX/(fs->bs/512)) { e->error=-VFS_EFBIG; return; }
    for (unsigned i=0;i<leaves;i++) {
        uint32_t block=e4_allocate(e); if (!block) return;
        uint8_t *b=e4_stage_zero(e,block); if (!b) return;
        memset(b,0,fs->bs); unsigned at=i*capacity, n=count-at; if (n>capacity) n=capacity;
        e4_header(b,n,capacity,0);
        for (unsigned k=0;k<n;k++) e4_leaf(b+12+k*12,&fs->pending[at+k]);
        e4_p32(b+12+capacity*12,e4_crc(seed,b,12+capacity*12));
        e->new_nodes[i]=block; e->keys[i]=fs->pending[at].logical;
        if (!indexes) e4_index(raw+52+i*12,e->keys[i],block);
    }
    for (unsigned i=0;i<indexes;i++) {
        uint32_t block=e4_allocate(e); if (!block) return;
        uint8_t *b=e4_stage_zero(e,block); if (!b) return;
        memset(b,0,fs->bs); unsigned at=i*capacity,n=leaves-at; if (n>capacity) n=capacity;
        e4_header(b,n,capacity,1);
        for (unsigned k=0;k<n;k++) e4_index(b+12+k*12,e->keys[at+k],e->new_nodes[at+k]);
        e4_p32(b+12+capacity*12,e4_crc(seed,b,12+capacity*12));
        e4_index(raw+52+i*12,e->keys[at],block);
    }
    if (!leaves) for (unsigned i=0;i<count;i++) e4_leaf(raw+52+i*12,&fs->pending[i]);
    for (unsigned i=0;i<e->old_count;i++) if (!e4_release(e,e->old_nodes[i],1)) return;
    e4_p32(raw+28,(uint32_t)blocks*(fs->bs/512)); e4_p16(raw+116,0);
    e4_p32(raw+4,(uint32_t)in->size); e4_p32(raw+108,(uint32_t)(in->size>>32));
    e4_inode_sum(fs,in->ino,raw);
}
static int e4_change(ext4_engine_t *e,uint32_t ino,uint32_t logical,uint32_t count,bool trim) {
    int r=e4_plan_begin(e); if (r) return r;
    ext4_mount_t *fs=e->fs; e4_inode_t in;
    r=e4_inode(fs,ino,&in); if (r) { e->error=r; return e4_plan_end(e); }
    uint8_t *raw=e4_inode_image(e,ino);
    if (!raw) return e4_plan_end(e);
    if (in.mode!=0x8000 || e4_u16(raw+26)!=1 || e4_u16(raw+116)) e->error=-VFS_EOPNOTSUPP;
    if (!trim && (!count || count>65536/fs->bs || (uint64_t)logical+count>8ULL*1024*1024*1024/fs->bs))
        e->error=-VFS_EFBIG;
    if (trim && (uint64_t)logical*fs->bs>in.size) e->error=-VFS_EINVAL;
    if (e->error) return e4_plan_end(e);
    fs->pending_count=fs->physical_count=fs->visits=0;
    unsigned depth=e4_u16(in.extent+6);
    if (depth>2) e->error=-VFS_EOPNOTSUPP;
    else e->error=e4_tree(&in,in.extent,depth,true,0,1ULL<<32);
    if (e->error || !e4_old_tree(e,in.extent,depth)) return e4_plan_end(e);
    uint64_t old_blocks=e->old_count;
    for (unsigned i=0;i<fs->pending_count;i++) old_blocks+=fs->pending[i].len;
    if (old_blocks*(fs->bs/512)!=e4_u32(raw+28)) { e->error=-VFS_EIO; return e4_plan_end(e); }
    if (trim) {
        unsigned keep=0;
        for (unsigned i=0;i<fs->pending_count;i++) {
            e4_extent_t x=fs->pending[i]; uint64_t end=(uint64_t)x.logical+x.len;
            if (end>logical) {
                unsigned removed=x.logical>=logical ? x.len : (unsigned)(end-logical);
                if (!e4_release(e,x.physical+x.len-removed,removed)) return e4_plan_end(e);
                x.len-=removed;
            }
            if (x.len) fs->pending[keep++]=x;
        }
        fs->pending_count=keep; in.size=(uint64_t)logical*fs->bs;
    } else {
        /* Insert individual allocations, then canonicalize adjacent runs. */
        uint64_t end=(uint64_t)logical+count;
        for (unsigned i=0;i<fs->pending_count;i++) {
            e4_extent_t x=fs->pending[i];
            if (logical<(uint64_t)x.logical+x.len && x.logical<end) { e->error=-VFS_EEXIST; return e4_plan_end(e); }
        }
        if (count>E4_MAP_MAX-fs->pending_count) { e->error=-VFS_EFBIG; return e4_plan_end(e); }
        unsigned at=0; while (at<fs->pending_count && fs->pending[at].logical<logical) at++;
        for (unsigned i=fs->pending_count;i>at;i--) fs->pending[i+count-1]=fs->pending[i-1];
        for (unsigned k=0;k<count;k++) {
            uint32_t block=e4_allocate(e); if (!block) return e4_plan_end(e);
            fs->pending[at+k]=(e4_extent_t){logical+k,block,1,false};
            e->zero[e->zeros++]=(e4_range_t){block,1};
        }
        fs->pending_count+=count;
        if (in.size<end*fs->bs) in.size=end*fs->bs;
        unsigned used=0;
        for (unsigned i=0;i<fs->pending_count;i++) {
            e4_extent_t x=fs->pending[i];
            if (used && fs->pending[used-1].unwritten==x.unwritten &&
                (uint64_t)fs->pending[used-1].logical+fs->pending[used-1].len==x.logical &&
                fs->pending[used-1].physical+fs->pending[used-1].len==x.physical &&
                fs->pending[used-1].len+x.len<=(x.unwritten ? 32767u : 32768u)) fs->pending[used-1].len+=x.len;
            else fs->pending[used++]=x;
        }
        fs->pending_count=used;
    }
    e4_rebuild(e,&in,raw); return e4_plan_end(e);
}
int ext4_engine_grow(ext4_engine_t *e,uint32_t ino,uint32_t logical,uint32_t count) {
    spin_debug_assert_unheld(); uint64_t f=spin_lock_irqsave(&e4_lock);
    int r=e4_change(e,ino,logical,count,false); spin_unlock_irqrestore(&e4_lock,f); return r;
}
int ext4_engine_trim(ext4_engine_t *e,uint32_t ino,uint32_t keep) {
    spin_debug_assert_unheld(); uint64_t f=spin_lock_irqsave(&e4_lock);
    int r=e4_change(e,ino,keep,0,true); spin_unlock_irqrestore(&e4_lock,f); return r;
}
static bool e4_write_bytes(ext4_engine_t *e,uint64_t off,const uint8_t *data,size_t len) {
    /* Journal workbench can only reach home storage through JBD2 checkpoint. */
    if (e->writer) return false;
    ext4_mount_t *fs=e->fs; uint64_t cap=(uint64_t)fs->blocks*fs->bs;
    if (off>cap || len>cap-off || block_get_max_run_bytes(fs->dev)<fs->dev->sector_size) return false;
    e4_cache_invalidate(fs,off,len);
    while (len) {
        if (!(off%fs->dev->sector_size) && len>=fs->dev->sector_size) {
            unsigned ss=fs->dev->sector_size;size_t limit=block_get_max_run_bytes(fs->dev);size_t n=len<limit ? len : limit;n-=n%ss;
            if (!block_write_sectors(fs->dev,off/ss,(uint32_t)(n/ss),data)) return false;
            off+=n;data+=n;len-=n;continue;
        }
        unsigned ss=fs->dev->sector_size; size_t skip=off%ss,n=ss-skip; if (n>len) n=len;
        if ((skip || n!=ss) && !block_read_sector(fs->dev,off/ss,fs->sector)) return false;
        memcpy(fs->sector+skip,data,n);
        if (!block_write_sector(fs->dev,off/ss,fs->sector)) return false;
        off+=n; data+=n; len-=n;
    }
    return true;
}
static void e4_seal(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;
    for (uint32_t g=0;g<fs->groups;g++) {
        uint32_t offset=g*32, gdblock=fs->first+1+offset/fs->bs;
        uint8_t *d=NULL;
        for (unsigned i=0;i<e->images;i++) if (e->image[i].block==gdblock) d=e->image[i].bytes+offset%fs->bs;
        if (!d) continue;
        for (unsigned kind=0;kind<2;kind++) for (unsigned i=0;i<e->images;i++) {
            if (e->image[i].block==e4_u32(d+kind*4))
                e4_p16(d+24+kind*2,(uint16_t)e4_crc(fs->seed,e->image[i].bytes,(kind ? fs->ipg : fs->bpg)/8));
        }
        uint8_t number[4]; e4_p32(number,g); e4_p16(d+30,0);
        e4_p16(d+30,(uint16_t)e4_crc(e4_crc(fs->seed,number,4),d,32));
    }
    uint8_t *sb=e4_super(e);
    if (sb) { e4_p16(sb+58,0); e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020)); }
}
static bool e4_hold_frees(ext4_engine_t *e,bool hold) {
    ext4_mount_t *fs=e->fs;
    for (unsigned i=0;i<e->frees;i++) {
        uint32_t lo=e->released[i].lo,len=e->released[i].len;
        while (len) {
            uint32_t g=(lo-fs->first)/fs->bpg,bit=(lo-fs->first)%fs->bpg;
            uint32_t n=fs->bpg-bit; if (n>len) n=len;
            uint8_t *map=e4_stage_bitmap(e,g,false); if (!map) return false;
            for (uint32_t k=0;k<n;k++) e4_set_bit(map,bit+k,hold);
            if (!e4_account(e,g,false,hold ? -(int)n : (int)n)) return false;
            lo+=n;len-=n;
        }
    }
    if (e->release_ino) {
        uint32_t g=(e->release_ino-1)/fs->ipg,bit=(e->release_ino-1)%fs->ipg;
        uint8_t *map=e4_stage_bitmap(e,g,true); if (!map) return false;
        e4_set_bit(map,bit,hold); if (!e4_account(e,g,true,hold ? -1 : 1)) return false;
        if (e->release_dir) {
            uint8_t *d=e4_descriptor(e,g);
            e4_p16(d+16,(uint16_t)(e4_u16(d+16)+(hold ? 1 : -1)));
        }
    }
    return true;
}
static bool e4_write_role(ext4_engine_t *e,unsigned role) {
    /* Zeroed allocations also precede publication, even without NEW images. */
    bool wrote=role==E4_NEW && e->zeros;
    for (unsigned i=0;i<e->images;i++) if (e->image[i].role==role) {
        unsigned count=1,bs=e->fs->bs;
        /* Preserve staging order and role barriers. Merge only consecutive
         * images whose disk blocks are adjacent, within the advertised device run cap. */
        while (count<block_get_max_run_bytes(e->fs->dev)/bs && i+count<e->images &&
               e->image[i+count].role==role &&
               (uint64_t)e->image[i].block+count==e->image[i+count].block) count++;
        const uint8_t *data=e->image[i].bytes;
        if (count>1) {
            for (unsigned j=0;j<count;j++) memcpy(e->write_run+j*bs,e->image[i+j].bytes,bs);
            data=e->write_run;
        }
        if (!e4_write_bytes(e,(uint64_t)e->image[i].block*bs,data,count*bs)) return false;
        i+=count-1;
        wrote=true;
    }
    return !wrote || block_flush(e->fs->dev);
}
static int e4_commit_unlocked(ext4_engine_t *e) {
    if (!e) return -VFS_EINVAL;
    if (e->writer) return -VFS_EOPNOTSUPP; /* Legacy direct-home commit only. */
    int r=-VFS_EINVAL;
    if (e->tainted) { r=-VFS_EIO; goto done; }
    if (!e->ready) goto done;
    /* Preflight all interim counters/checksums before the first disk write. */
    if (!e4_hold_frees(e,true)) { r=e->error; e->ready=false; if (r==-VFS_EIO) e->tainted=true; goto done; }
    e4_seal(e); if (e->error) { r=e->error; e->ready=false; if (r==-VFS_EIO) e->tainted=true; goto done; }
    uint8_t *sb=e4_super(e);
    if (!e->dirty) {
        if (!e4_bytes(e->fs,1024,e->fs->scratch,1024)) goto fail;
        e4_p16(e->fs->scratch+58,0);
        e4_p32(e->fs->scratch+1020,e4_crc(UINT32_MAX,e->fs->scratch,1020));
        if (!e4_write_bytes(e,1024,e->fs->scratch,1024) || !block_flush(e->fs->dev)) goto fail;
        e->dirty=true;
    }
    memset(e->fs->scratch,0,e->fs->bs);
    for (unsigned i=0;i<e->zeros;i++) if (!e4_write_bytes(e,(uint64_t)e->zero[i].lo*e->fs->bs,e->fs->scratch,e->fs->bs)) goto fail;
    if (!e4_write_role(e,E4_NEW) || !e4_write_role(e,E4_ALLOC) || !e4_write_role(e,E4_REFERENCE)) goto fail;
    if (e->frees || e->release_ino) {
        if (!e4_hold_frees(e,false)) goto fail;
        e4_seal(e);
        if (e->error || !sb || !e4_write_role(e,E4_ALLOC)) goto fail;
    }
    for (uint32_t g=0;g<e->fs->groups;g++) {
        uint32_t offset=g*32,block=e->fs->first+1+offset/e->fs->bs;
        for (unsigned i=0;i<e->images;i++) if (e->image[i].block==block)
            memcpy(e->fs->gd[g].raw,e->image[i].bytes+offset%e->fs->bs,32);
    }
    if (e->inode_delta>0) e->reserved[e->reserved_inodes++]=e->reserve_ino;
    if (e->inode_delta<0) for (int i=0;i<e->reserved_inodes;i++) if (e->reserved[i]==e->release_ino) {
        e->reserved[i]=e->reserved[--e->reserved_inodes]; break;
    }
    /* All role/free barriers succeeded and descriptor state now matches disk. */
    for (unsigned i=0;i<e->images;i++) e4_cache_committed(e->fs,e->image[i].block,e->image[i].bytes);
    e->ready=false; r=0; goto done;
fail:
    e->tainted=true; e->ready=false; r=-VFS_EIO;
done:
    if (e->tainted) e4_cache_drop(e->fs,true);
    return r;
}
int ext4_engine_commit(ext4_engine_t *e) {
    spin_debug_assert_unheld();
    if (e && e->writer) return e4_transaction_commit(e);
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    int r=e4_commit_unlocked(e); spin_unlock_irqrestore(&e4_lock,flags); return r;
}
static int e4_reserve_inode_unlocked(ext4_engine_t *e,bool directory,uint32_t *ino) {
    if (ino) *ino=0;
    if (!ino) return -VFS_EINVAL;
    int r=e4_plan_begin(e);
    if (r) goto done;
    if (e->reserved_inodes==64) { e->error=-VFS_EFBIG; goto end; }
    for (uint32_t g=0;g<e->fs->groups;g++) {
        if (!e4_u16(e->fs->gd[g].raw+14)) continue;
        uint8_t *map=e4_stage_bitmap(e,g,true); if (!map) goto end;
        for (uint32_t bit=0;bit<e->fs->ipg;bit++) {
            uint32_t n=g*e->fs->ipg+bit+1;
            if (n<11 || e4_bit(map,bit)) continue;
            uint8_t *raw=e4_inode_image(e,n); if (!raw) goto end;
            uint32_t generation=e4_u32(raw+100);
            if (generation==UINT32_MAX) { e->error=-VFS_EFBIG; goto end; }
            memset(raw,0,256); e4_p16(raw,directory ? VFS_S_IFDIR|0755 : VFS_S_IFREG|0644);
            e4_p32(raw+100,generation+1); e4_p32(raw+32,0x80000);
            e4_p16(raw+128,32); e4_header(raw+40,0,4,0); e4_inode_sum(e->fs,n,raw);
            e4_set_bit(map,bit,true); if (!e4_account(e,g,true,-1)) goto end;
            uint8_t *d=e4_descriptor(e,g);
            if (directory) {
                if (e4_u16(d+16)>=e->fs->ipg) { e->error=-VFS_EIO; goto end; }
                e4_p16(d+16,(uint16_t)(e4_u16(d+16)+1));
            }
            if (e4_u16(d+28)>e->fs->ipg-bit-1) e4_p16(d+28,(uint16_t)(e->fs->ipg-bit-1));
            e->inode_delta=1; e->reserve_ino=n; *ino=n; goto end;
        }
    }
    e->error=-VFS_ENOSPC;
end:
    r=e4_plan_end(e); if (r) *ino=0;
done:
    return r;
}
int ext4_engine_reserve_inode_kind(ext4_engine_t *e,bool directory,uint32_t *ino) {
    spin_debug_assert_unheld(); uint64_t flags=spin_lock_irqsave(&e4_lock);
    int r=e4_reserve_inode_unlocked(e,directory,ino); spin_unlock_irqrestore(&e4_lock,flags); return r;
}
int ext4_engine_reserve_inode(ext4_engine_t *e,uint32_t *ino) {
    return ext4_engine_reserve_inode_kind(e,false,ino);
}
int ext4_engine_release_inode(ext4_engine_t *e,uint32_t ino) {
    spin_debug_assert_unheld(); uint64_t flags=spin_lock_irqsave(&e4_lock);
    int r=e4_plan_begin(e); if (r) goto done;
    bool owned=false;
    for (int i=0;i<e->reserved_inodes;i++) if (e->reserved[i]==ino) owned=true;
    if (!owned) { e->error=-VFS_EINVAL; goto end; }
    uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;
    uint8_t *map=e4_stage_bitmap(e,g,true), *raw=e4_inode_image(e,ino);
    if (!map || !raw) goto end;
    if (!e4_bit(map,bit) || e4_u16(raw+26) || e4_u32(raw+4) || e4_u32(raw+108) ||
        e4_u32(raw+28) || e4_u16(raw+42) || e4_u16(raw+46)) { e->error=-VFS_EIO; goto end; }
    uint32_t stored=e4_u16(raw+124) | (uint32_t)e4_u16(raw+130)<<16;
    e4_inode_sum(e->fs,ino,raw);
    if (stored!=(e4_u16(raw+124) | (uint32_t)e4_u16(raw+130)<<16)) { e->error=-VFS_EIO; goto end; }
    e->release_dir=(e4_u16(raw)&0xf000)==0x4000;
    if (e->release_dir) {
        uint8_t *d=e4_descriptor(e,g);
        if (!e4_u16(d+16)) { e->error=-VFS_EIO; goto end; }
        e4_p16(d+16,(uint16_t)(e4_u16(d+16)-1));
    }
    uint32_t generation=e4_u32(raw+100);
    memset(raw,0,256); e4_p32(raw+100,generation); e4_p16(raw+128,32);
    e4_inode_sum(e->fs,ino,raw); e4_set_bit(map,bit,false);
    if (!e4_account(e,g,true,1)) goto end;
    e->inode_delta=-1; e->release_ino=ino;
end:
    r=e4_plan_end(e);
done:
    spin_unlock_irqrestore(&e4_lock,flags); return r;
}
int ext4_engine_open(block_dev_t *dev,ext4_engine_t **out) {
    spin_debug_assert_unheld(); if (out) *out=NULL;
    if (!out || !dev || !dev->read_sector || !dev->write_sector || !dev->flush ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    ext4_engine_t *e=kcalloc(1,sizeof(*e));
    ext4_mount_t *fs=kcalloc(1,sizeof(*fs));
    if (!e || !fs) { kfree(e); kfree(fs); return -VFS_ENOMEM; }
    e->fs=fs; fs->dev=dev; fs->bs=dev->sector_size; fs->blocks=(uint32_t)dev->sector_count;
    uint64_t flags=spin_lock_irqsave(&e4_lock); int r=-VFS_EEXIST;
    if (!e4_active && !e4_engine_busy) {
        r=e4_admit(fs);
        if (!r) { e4_engine_busy=true; *out=e; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);
    if (r) { e4_discard(fs); kfree(e); } return r;
}
void ext4_engine_abort(ext4_engine_t *e) {
    e4_engine_owner(e); if (!e) return;
    if (e->writer) {
        if (jbd2_writer_state(e->writer)==JBD2_WRITE_STAGING) (void)jbd2_writer_abort(e->writer);
        e->ready=false; e->error=0; e->images=e->revoke_count=0; return;
    }
    uint64_t flags=spin_lock_irqsave(&e4_lock); e->ready=false;
    spin_unlock_irqrestore(&e4_lock,flags);
}
int ext4_engine_finish(ext4_engine_t *e) {
    spin_debug_assert_unheld(); if (!e) return -VFS_EINVAL;
    if (e->writer) return e->tainted ? -VFS_EIO : -VFS_EOPNOTSUPP;
    uint64_t flags=spin_lock_irqsave(&e4_lock); int r=0;
    if (e->tainted) r=-VFS_EIO;
    else if (e->ready || e->reserved_inodes) r=-VFS_EINVAL;
    else if (e->dirty) {
        if (!e4_bytes(e->fs,1024,e->fs->scratch,1024)) r=-VFS_EIO;
        else {
            e4_p16(e->fs->scratch+58,1);
            e4_p32(e->fs->scratch+1020,e4_crc(UINT32_MAX,e->fs->scratch,1020));
            if (!block_flush(e->fs->dev) || !e4_write_bytes(e,1024,e->fs->scratch,1024) || !block_flush(e->fs->dev)) r=-VFS_EIO;
        }
        if (r) { e->tainted=true;e4_cache_drop(e->fs,true); } else e->dirty=false;
    }
    spin_unlock_irqrestore(&e4_lock,flags); return r;
}
static void e4_engine_dispose(ext4_engine_t *e) {
    spin_debug_assert_unheld();if (!e) return;
    jbd2_writer_close(e->writer); kfree(e->journal_guard);
    if (e->fs->engine==e) e->fs->engine=NULL;
    e4_discard(e->fs);kfree(e);
}
void ext4_engine_close(ext4_engine_t *e) {
    spin_debug_assert_unheld(); if (!e) return;
    e4_engine_dispose(e);
    uint64_t flags=spin_lock_irqsave(&e4_lock); e4_engine_busy=false;
    spin_unlock_irqrestore(&e4_lock,flags);
}
