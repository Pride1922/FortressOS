#include "jbd2.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"

#define J_MAGIC 0xc03b3998u
typedef struct { uint32_t block, ordinal; uint8_t *bytes; } j_image_t;
typedef struct { uint32_t block, ordinal; } j_revoke_t;
struct jbd2_plan {
    block_dev_t *dev;
    uint32_t bs, blocks, length, first, seed, super_block;
    uint32_t *sorted;
    unsigned images, revokes;
    j_image_t image[JBD2_IMAGES_MAX];
    j_revoke_t revoke[JBD2_REVOKES_MAX];
    uint8_t super[4096], scratch[4096], data[4096];
    jbd2_report_t report;
    bool poisoned, applied;
};
static uint32_t j_be(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static void j_put(uint8_t *p,uint32_t n) {
    p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;
}
static uint32_t j_crc(uint32_t crc,const uint8_t *p,size_t n) {
    while (n--) { crc^=*p++;for (unsigned b=0;b<8;b++) crc=(crc>>1)^(0x82f63b78u&(0u-(crc&1))); }
    return crc;
}
static bool j_sum(uint8_t *p,size_t n,size_t offset,uint32_t seed) {
    uint32_t stored=j_be(p+offset);j_put(p+offset,0);
    uint32_t got=j_crc(seed,p,n);j_put(p+offset,stored);return got==stored;
}
static bool j_io(jbd2_plan_t *p,uint32_t block,uint8_t *bytes,bool write) {
    if (block>=p->blocks) return false;
    unsigned ss=p->dev->sector_size;
    /* Sub-sector filesystem blocks preserve unrelated bytes. */
    uint8_t sector[4096]; uint64_t off=(uint64_t)block*p->bs;
    if (!(off%ss) && p->bs>=ss)
        return write ? block_write_sectors(p->dev,off/ss,p->bs/ss,bytes) :
                       block_read_sectors(p->dev,off/ss,p->bs/ss,bytes);
    for (unsigned at=0;at<p->bs;) {
        unsigned skip=(unsigned)(off%ss),n=ss-skip;if (n>p->bs-at) n=p->bs-at;
        if ((!write || skip || n!=ss) && !block_read_sector(p->dev,off/ss,sector)) return false;
        if (write) {
            memcpy(sector+skip,bytes+at,n);
            if (!block_write_sector(p->dev,off/ss,sector)) return false;
        } else memcpy(bytes+at,sector+skip,n);
        off+=n;at+=n;
    }
    return true;
}
static void j_heap(uint32_t *a,unsigned root,unsigned n) {
    while (root<n/2) {
        unsigned c=root*2+1;if (c+1<n && a[c]<a[c+1]) c++;
        if (a[root]>=a[c]) break;
        uint32_t t=a[root];a[root]=a[c];a[c]=t;root=c;
    }
}
static bool j_target(jbd2_plan_t *p,uint32_t block) {
    if (block>=p->blocks) return false;
    unsigned lo=0,hi=p->length;
    while (lo<hi) { unsigned mid=lo+(hi-lo)/2;if (p->sorted[mid]<block) lo=mid+1;else hi=mid; }
    return lo==p->length || p->sorted[lo]!=block;
}
static uint32_t j_next(jbd2_plan_t *p,uint32_t n) { return n+1==p->length ? p->first : n+1; }
void jbd2_release(jbd2_plan_t *p) {
    if (!p) return;
    for (unsigned i=0;i<p->images;i++) kfree(p->image[i].bytes);
    kfree(p->sorted);kfree(p);
}
int jbd2_analyze(block_dev_t *dev,const jbd2_source_t *s,jbd2_plan_t **out,jbd2_report_t *report) {
    spin_debug_assert_unheld();if (out) *out=NULL;if (report) memset(report,0,sizeof(*report));
    if (!out || !s || !dev || !dev->read_sector || !s->journal_map ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT64_MAX/dev->sector_size ||
        (s->block_size!=1024 && s->block_size!=2048 && s->block_size!=4096) ||
        !s->filesystem_blocks || s->journal_blocks<4) return -VFS_EINVAL;
    if (s->journal_blocks>JBD2_MAP_MAX) return -VFS_EFBIG;
    if ((uint64_t)s->filesystem_blocks*s->block_size>dev->sector_count*dev->sector_size) return -VFS_EIO;
    jbd2_plan_t *p=kcalloc(1,sizeof(*p));if (!p) return -VFS_ENOMEM;
    p->dev=dev;p->bs=s->block_size;p->blocks=s->filesystem_blocks;p->length=s->journal_blocks;
    int r=-VFS_EIO;p->sorted=kmalloc(p->length*sizeof(uint32_t));if (!p->sorted) { r=-VFS_ENOMEM;goto fail; }
    memcpy(p->sorted,s->journal_map,p->length*sizeof(uint32_t));
    for (unsigned i=p->length/2;i;i--) j_heap(p->sorted,i-1,p->length);
    for (unsigned i=p->length;i>1;i--) { uint32_t t=p->sorted[0];p->sorted[0]=p->sorted[i-1];p->sorted[i-1]=t;j_heap(p->sorted,0,i-1); }
    for (unsigned i=0;i<p->length;i++) if (!p->sorted[i] || p->sorted[i]>=p->blocks || (i && p->sorted[i]==p->sorted[i-1])) goto fail;
    p->super_block=s->journal_map[0];if (!j_io(p,p->super_block,p->super,false)) goto fail;
    uint8_t *sb=p->super;
    if (j_be(sb)!=J_MAGIC || j_be(sb+4)!=4 || j_be(sb+12)!=p->bs || j_be(sb+16)!=p->length ||
        j_be(sb+32) || memcmp(sb+48,s->uuid,16) || j_be(sb+64)!=1) goto fail;
    if (j_be(sb+36) || j_be(sb+40)!=0x11 || j_be(sb+44) || sb[80]!=4 || j_be(sb+84)) { r=-VFS_EOPNOTSUPP;goto fail; }
    if (!j_sum(sb,1024,252,UINT32_MAX)) goto fail;
    p->seed=j_crc(UINT32_MAX,s->uuid,16);p->first=j_be(sb+20);
    uint32_t cursor=j_be(sb+28),sequence=j_be(sb+24);
    if (!p->first || p->first>=p->length || (cursor && (cursor<p->first || cursor>=p->length))) goto fail;
    p->report.next_sequence=sequence;
    if (!cursor) goto success;
    unsigned consumed=0,committed_images=0,committed_revokes=0,tx_blocks=0;
    while (consumed<p->length-p->first) {
        if (!j_io(p,s->journal_map[cursor],p->scratch,false)) goto fail;
        uint8_t *b=p->scratch;
        if (j_be(b)!=J_MAGIC) { p->report.incomplete_tail=tx_blocks!=0;break; }
        if (j_be(b+8)!=sequence) {
            /* Older records can remain behind the live tail; a future ID
             * would skip a transaction and is not an admissible boundary. */
            if ((uint32_t)(j_be(b+8)-sequence)<0x80000000u) goto fail;
            p->report.incomplete_tail=tx_blocks!=0;break;
        }
        unsigned type=j_be(b+4);cursor=j_next(p,cursor);consumed++;tx_blocks++;
        if (tx_blocks>128 || p->report.transactions==JBD2_TX_MAX) { r=-VFS_EFBIG;goto fail; }
        if (type==1) {
            if (!j_sum(b,p->bs,p->bs-4,p->seed)) goto fail;
            unsigned at=12;bool last=false;
            while (at+16<=p->bs-4) {
                uint32_t home=j_be(b+at),flags=j_be(b+at+4),sum=j_be(b+at+12);
                if (j_be(b+at+8) || (flags&~11u) || !j_target(p,home)) goto fail;
                at+=16;
                if (!(flags&2)) { if (at+16>p->bs-4 || memcmp(b+at,s->uuid,16)) goto fail;at+=16; }
                if (consumed>=p->length-p->first) { p->report.incomplete_tail=true;goto tail; }
                if (!j_io(p,s->journal_map[cursor],p->data,false)) goto fail;
                uint8_t seq[4];j_put(seq,sequence);
                if (sum!=j_crc(j_crc(p->seed,seq,4),p->data,p->bs)) goto fail;
                if (flags&1) { if (j_be(p->data)) goto fail;j_put(p->data,J_MAGIC); }
                if (s->validate_home && !s->validate_home(s->context,home,p->data)) goto fail;
                if (p->images==JBD2_IMAGES_MAX || p->images-committed_images==64) { r=-VFS_EFBIG;goto fail; }
                j_image_t *image=&p->image[p->images];image->bytes=kmalloc(p->bs);
                if (!image->bytes) { r=-VFS_ENOMEM;goto fail; }
                image->block=home;image->ordinal=p->report.transactions;memcpy(image->bytes,p->data,p->bs);p->images++;
                cursor=j_next(p,cursor);consumed++;tx_blocks++;
                if (tx_blocks>128) { r=-VFS_EFBIG;goto fail; }
                if (flags&8) { last=true;break; }
            }
            if (!last) goto fail;
        } else if (type==5) {
            if (!j_sum(b,p->bs,p->bs-4,p->seed)) goto fail;
            uint32_t count=j_be(b+12);
            if (count<16 || count>p->bs-4 || (count-16)%4) goto fail;
            for (unsigned at=16;at<count;at+=4) {
                uint32_t home=j_be(b+at);if (!j_target(p,home)) goto fail;
                if (p->revokes==JBD2_REVOKES_MAX || p->revokes-committed_revokes==64) { r=-VFS_EFBIG;goto fail; }
                p->revoke[p->revokes++]=(j_revoke_t){home,p->report.transactions};
            }
        } else if (type==2) {
            if (b[12] || b[13] || !j_sum(b,p->bs,16,p->seed)) goto fail;
            committed_images=p->images;committed_revokes=p->revokes;
            p->report.transactions++;sequence++;p->report.next_sequence=sequence;tx_blocks=0;
        } else { r=-VFS_EOPNOTSUPP;goto fail; }
    }
    if (consumed==p->length-p->first && tx_blocks) p->report.incomplete_tail=true;
tail:
    while (p->images>committed_images) kfree(p->image[--p->images].bytes);
    p->revokes=committed_revokes;
success:
    p->report.images=p->images;p->report.revokes=p->revokes;
    for (unsigned i=0;i<p->images;i++) {
        bool revoked=false;
        for (unsigned k=0;k<p->revokes;k++) if (p->revoke[k].block==p->image[i].block && p->revoke[k].ordinal>=p->image[i].ordinal) revoked=true;
        if (revoked) p->report.revoked++;else p->report.replayed++;
    }
    if (report) *report=p->report;
    *out=p;return 0;
fail:
    jbd2_release(p);return r;
}
static bool j_revoked(const jbd2_plan_t *p,unsigned i) {
    for (unsigned k=0;k<p->revokes;k++)
        if (p->revoke[k].block==p->image[i].block && p->revoke[k].ordinal>=p->image[i].ordinal) return true;
    return false;
}
bool jbd2_preview_read_sector(jbd2_plan_t *p,uint64_t lba,void *bytes) {
    spin_debug_assert_unheld();
    if (!p || p->poisoned || !bytes || lba>=p->dev->sector_count) return false;
    if (!block_read_sector(p->dev,lba,bytes)) return false;
    uint64_t lo=lba*p->dev->sector_size,hi=lo+p->dev->sector_size;
    for (unsigned i=0;i<=p->images;i++) {
        uint32_t block;const uint8_t *source;
        if (i<p->images) {
            if (j_revoked(p,i)) continue;
            block=p->image[i].block;source=p->image[i].bytes;
        } else {
            block=p->super_block;memcpy(p->scratch,p->super,p->bs);
            if (j_be(p->super+28)) {
                j_put(p->scratch+24,p->report.next_sequence);j_put(p->scratch+28,0);j_put(p->scratch+88,p->first);
                j_put(p->scratch+252,0);j_put(p->scratch+252,j_crc(UINT32_MAX,p->scratch,1024));
            }
            source=p->scratch;
        }
        uint64_t start=(uint64_t)block*p->bs,end=start+p->bs;
        if (start<hi && end>lo) {
            uint64_t a=start>lo ? start : lo,b=end<hi ? end : hi;
            memcpy((uint8_t *)bytes+a-lo,source+a-start,(size_t)(b-a));
        }
    }
    return true;
}
#ifdef FORTRESS_EXT4_RECOVERY_PAUSE_TEST
static block_dev_t *j_pause_partition;
static void (*j_pause_callback)(void);
int jbd2_test_arm_recovery_pause(block_dev_t *partition,void (*pause)(void)) {
    spin_debug_assert_unheld();
    if (!partition || !pause || j_pause_callback) return -VFS_EINVAL;
    j_pause_partition=partition;j_pause_callback=pause;return 0;
}
#endif
int jbd2_replay(jbd2_plan_t *p,bool admitted) {
    spin_debug_assert_unheld();if (!p) return -VFS_EINVAL;
    if (!admitted || !p->dev->write_sector || !p->dev->flush) return -VFS_EROFS;
    if (p->poisoned) return -VFS_EIO;
    if (p->applied || !j_be(p->super+28)) return 0;
    for (unsigned i=0;i<p->images;i++) {
        if (!j_revoked(p,i) && !j_io(p,p->image[i].block,p->image[i].bytes,true)) goto fail;
#ifdef FORTRESS_EXT4_RECOVERY_PAUSE_TEST
        if (!j_revoked(p,i) && j_pause_partition==p->dev && j_pause_callback) {
            bool later=false;
            for (unsigned k=i+1;k<p->images;k++)
                if (!j_revoked(p,k) && p->image[k].block!=p->image[i].block) later=true;
            if (!later) goto fail;
            if (!block_flush(p->dev)) goto fail;
            void (*pause)(void)=j_pause_callback;
            j_pause_callback=NULL;j_pause_partition=NULL;pause();
            goto fail; /* A terminal hook must not return. */
        }
#endif
    }
    if (!block_flush(p->dev)) goto fail;
    j_put(p->super+24,p->report.next_sequence);j_put(p->super+28,0);j_put(p->super+88,p->first);
    j_put(p->super+252,0);j_put(p->super+252,j_crc(UINT32_MAX,p->super,1024));
    if (!j_io(p,p->super_block,p->super,true) || !block_flush(p->dev)) goto fail;
    p->applied=true;return 0;
fail:
    p->poisoned=true;return -VFS_EIO;
}

#include "jbd2_write.inc"
