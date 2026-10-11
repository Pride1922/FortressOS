#include "install_fat.h"
#include "string.h"
#include "../../user/tools/digest.h"

static uint16_t u16(const uint8_t *p) {return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p) {return (uint32_t)u16(p)|((uint32_t)u16(p+2)<<16);}
static void put32(uint8_t *p,uint32_t v) {for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));}
static bool zeros(const uint8_t *p,size_t n) {for (size_t i=0;i<n;i++) if (p[i]) return false;return true;}
static bool disjoint(const void *a,size_t an,const void *b,size_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=UINTPTR_MAX-an && y<=UINTPTR_MAX-bn && (x+an<=y || y+bn<=x);
}
static bool cluster_valid(const install_fat_work_t *w,uint32_t c) {return c>=2 && c<w->clusters+2;}
static uint32_t cluster_offset(const install_fat_work_t *w,uint32_t c) {return w->data+(c-2)*w->cluster_bytes;}
static uint32_t next(const uint8_t *b,const install_fat_work_t *w,uint32_t c) {return u32(b+w->fat+c*4)&0xfffffff;}
static bool claim(install_fat_work_t *w,uint32_t c) {
    if (!cluster_valid(w,c) || (w->owned[c/8]&(1u<<(c%8)))) return false;
    w->owned[c/8]|=(uint8_t)(1u<<(c%8));return true;
}
static bool file_chain(const uint8_t *b,install_fat_work_t *w,uint32_t c,uint32_t size) {
    if (size>INSTALL_FAT_BYTES) return false;
    uint32_t count=(size+w->cluster_bytes-1)/w->cluster_bytes;
    if (!count) return !c;
    for (uint32_t i=0;i<count;i++) {
        if (!claim(w,c)) return false;
        c=next(b,w,c);
        if (i+1<count ? !cluster_valid(w,c) : c<0xffffff8) return false;
    }
    return true;
}
static uint8_t checksum(const uint8_t *name) {
    uint8_t s=0;for (unsigned i=0;i<11;i++) s=(uint8_t)(((s&1)<<7)+(s>>1)+name[i]);return s;
}
static bool short_name(const uint8_t *e,char *out) {
    if (e[12]&~0x18u || e[0]==' ' || e[0]=='.') return false;
    unsigned n=0;
    for (unsigned part=0;part<2;part++) {
        unsigned start=part ? 8 : 0,end=part ? 11 : 8;
        unsigned length=end;while (length>start && e[length-1]==' ') length--;
        if (part && length>start) out[n++]='.';
        for (unsigned i=start;i<length;i++) {
            uint8_t c=e[i];
            if (c<'!' || c>'~' || c=='/' || c=='\\' || c==':' || c==' ' || c=='.') return false;
            out[n++]=(char)(c>='A' && c<='Z' ? c+32 : c);
        }
    }
    out[n]=0;return n!=0;
}
static const char *const paths[INSTALL_FAT_SLOTS]={"boot/bootstrap.tar","boot/readiness.v1","limine.conf",
    "boot/limine.conf","boot/limine/limine.conf","efi/boot/limine.conf"};
static const uint32_t sizes[INSTALL_FAT_SLOTS]={8u*1024*1024,512,4096,4096,4096,4096};
static bool unique_name(install_fat_work_t *w,const char *parent,const char *name) {
    digest_t digest;uint8_t hash[32];digest_init(&digest,DIGEST_SHA256);
    (void)digest_update(&digest,parent,strlen(parent));(void)digest_update(&digest,"/",1);
    (void)digest_update(&digest,name,strlen(name));digest_final(&digest,hash);
    if (w->names==8192) return false;
    for (unsigned i=0;i<w->names;i++) if (!memcmp(w->name_hashes[i],hash,16)) return false;
    memcpy(w->name_hashes[w->names++],hash,16);return true;
}
/* Directory traversal is bounded at eight levels and 4096 live/deleted entries.
 * Every allocation is claimed once, rejecting cycles and cross-linked files. */
static bool directory(const uint8_t *b,install_fat_work_t *w,uint32_t c,const char *parent,unsigned depth,uint32_t parent_cluster) {
    if (depth>8) return false;
    uint32_t self=c;
    char lfn[66]={0};unsigned sequence=0,total=0;uint8_t sum=0;bool ended=false;
    while (cluster_valid(w,c)) {
        if (!claim(w,c)) return false;
        uint32_t base=cluster_offset(w,c);
        for (uint32_t off=0;off<w->cluster_bytes;off+=32) {
            const uint8_t *e=b+base+off;
            if (ended) {if (!zeros(e,32)) return false;continue;}
            if (!e[0]) {if (sequence || total) return false;ended=true;if (!zeros(e,32)) return false;continue;}
            if (++w->visits>4096) return false;
            if (e[0]==0xe5) {if (sequence || total) return false;continue;}
            if (e[11]==15) {
                unsigned order=e[0]&31;
                if (!order || order>5 || (e[0]&0xa0) || e[12] || u16(e+26)) return false;
                if (e[0]&64) {
                    if (sequence || total) return false;
                    sequence=order;total=order;sum=e[13];memset(lfn,0xff,sizeof(lfn));lfn[65]=0;
                }
                if (!sequence || order!=sequence || e[13]!=sum) return false;
                static const uint8_t positions[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
                for (unsigned j=0;j<13;j++) {
                    uint16_t ch=u16(e+positions[j]);
                    if (ch!=0xffff && ch && (ch<'!' || ch>'~' || ch=='/' || ch=='\\' || ch==':')) return false;
                    lfn[(order-1)*13+j]=ch==0xffff ? (char)0xff : (char)(ch>='A' && ch<='Z' ? ch+32 : ch);
                }
                sequence--;continue;
            }
            char name[66],alias[16];bool long_name=total!=0;
            if (total) {
                if (sequence || checksum(e)!=sum) return false;
                unsigned length=0;while (length<total*13 && lfn[length] && (uint8_t)lfn[length]!=0xff) length++;
                if (!length || length>63 || (length<total*13 && lfn[length])) return false;
                for (unsigned j=length+1;j<total*13;j++) if ((uint8_t)lfn[j]!=0xff) return false;
                memcpy(name,lfn,length);name[length]=0;total=0;
                if (!short_name(e,alias)) return false;
            } else if (e[0]=='.') {
                if (e[11]!=16 || u32(e+28) || (memcmp(e,".          ",11) && memcmp(e,"..         ",11))) return false;
                uint32_t target=((uint32_t)u16(e+20)<<16)|u16(e+26);
                if (!depth || target!=(e[1]=='.' ? parent_cluster : self)) return false;
                continue;
            } else if (!short_name(e,name)) return false;
            if (e[11]&~0x3fu || (e[11]&8)) {
                if (e[11]!=8 || depth || u32(e+28) || u16(e+20) || u16(e+26)) return false;
                continue;
            }
            if (!unique_name(w,parent,name) || (long_name && strcmp(alias,name) && !unique_name(w,parent,alias))) return false;
            char path[256];size_t pn=strlen(parent),nn=strlen(name);
            if (pn+nn+2>sizeof(path)) return false;
            memcpy(path,parent,pn);if (pn) path[pn++]='/';memcpy(path+pn,name,nn+1);
            uint32_t first=((uint32_t)u16(e+20)<<16)|u16(e+26),size=u32(e+28);
            if (e[11]&16) {
                if (size || !directory(b,w,first,path,depth+1,depth ? self : 0)) return false;
            } else {
                if (!file_chain(b,w,first,size)) return false;
                if (!strcmp(path,"boot/fortress.elf")) {w->kernel_first=first;w->kernel_size=size;}
                for (unsigned i=0;i<INSTALL_FAT_SLOTS;i++) if (!strcmp(path,paths[i])) {
                    if ((w->found&(1u<<i)) || size!=sizes[i] || (e[11]&7)) return false;
                    w->found|=1u<<i;w->entry[i]=base+off;w->first[i]=first;w->capacity[i]=size;
                }
            }
        }
        uint32_t nc=next(b,w,c);
        if (nc>=0xffffff8) return !sequence && !total;
        if (!cluster_valid(w,nc)) return false;
        c=nc;
    }
    return false;
}
int install_fat_validate(const void *bytes,size_t n,install_fat_work_t *w) {
    if (!bytes || !w || n!=INSTALL_FAT_BYTES || !disjoint(bytes,n,w,sizeof(*w))) return -1;
    memset(w,0,sizeof(*w));const uint8_t *b=bytes;
    uint32_t spc=b[13],reserved=u16(b+14),fat=u32(b+36),total=u32(b+32);
    if (u16(b+11)!=512 || !spc || (spc&(spc-1)) || spc>128 || reserved<8 || b[16]!=2 ||
        u16(b+17) || u16(b+19) || u16(b+22) || u16(b+40) || u16(b+42) || total!=n/512 ||
        !fat || fat>(total-reserved)/2 || b[510]!=0x55 || b[511]!=0xaa) return -1;
    w->fat=reserved*512;w->fat_sectors=fat;w->data=(reserved+2*fat)*512;
    w->cluster_bytes=spc*512;w->clusters=(uint32_t)(n-w->data)/w->cluster_bytes;
    w->root=u32(b+44);w->fsinfo=u16(b+48)*512;w->backup=u16(b+50)*512;
    if (w->clusters<65525 || w->clusters+2>INSTALL_FAT_BITMAP_BYTES*8 ||
        w->clusters+2>fat*128 || !cluster_valid(w,w->root) || w->fsinfo<512 ||
        w->fsinfo>=w->fat || w->backup<512 || w->backup+w->fsinfo+512>w->fat ||
        memcmp(b,b+w->backup,512) || memcmp(b+w->fat,b+w->fat+fat*512,fat*512)) return -1;
    const uint8_t *info=b+w->fsinfo;
    if (u32(info)!=0x41615252 || u32(info+484)!=0x61417272 || u32(info+508)!=0xaa550000 ||
        memcmp(info,b+w->backup+w->fsinfo,512) || (next(b,w,0)&0xffffff8)!=0xffffff8 || next(b,w,1)<0xffffff8) return -1;
    if (!directory(b,w,w->root,"",0,0) || w->found!=(1u<<INSTALL_FAT_SLOTS)-1) return -1;
    w->next_free=UINT32_MAX;
    for (uint32_t c=2;c<w->clusters+2;c++) {
        uint32_t value=next(b,w,c);bool owned=(w->owned[c/8]&(1u<<(c%8)))!=0;
        if (value ? !owned : owned) return -1;
        if (!value) {w->free_clusters++;if (w->next_free==UINT32_MAX) w->next_free=c;}
    }
    /* mtools can leave the cached FSInfo count unknown/stale; graph is authoritative.
     * Personalization always replaces both hints with exact freshly counted values. */
    return 0;
}
int install_fat_kernel_matches(const void *bytes,size_t n,const void *kernel,size_t kn,install_fat_work_t *w) {
    if (!kernel || !kn || kn>32u*1024*1024 || install_fat_validate(bytes,n,w) || w->kernel_size!=kn) return -1;
    const uint8_t *b=bytes,*k=kernel;uint32_t c=w->kernel_first;size_t at=0;
    while (at<kn) {
        if (!cluster_valid(w,c)) return -1;
        size_t take=kn-at;if (take>w->cluster_bytes) take=w->cluster_bytes;
        if (memcmp(b+cluster_offset(w,c),k+at,take)) return -1;
        at+=take;c=next(b,w,c);
    }
    return 0;
}
static void fat_set(uint8_t *b,install_fat_work_t *w,uint32_t c,uint32_t value) {
    for (unsigned i=0;i<2;i++) {
        uint8_t *p=b+w->fat+i*w->fat_sectors*512+c*4;
        put32(p,(u32(p)&0xf0000000)|value);
    }
}
static void replace(uint8_t *b,install_fat_work_t *w,unsigned slot,const void *bytes,size_t n) {
    uint32_t c=w->first[slot],keep=(uint32_t)(n+w->cluster_bytes-1)/w->cluster_bytes;
    size_t at=0;unsigned count=0;
    while (cluster_valid(w,c)) {
        uint32_t nc=next(b,w,c);uint8_t *data=b+cluster_offset(w,c);
        memset(data,0,w->cluster_bytes);
        if (count<keep) {
            size_t amount=n-at;if (amount>w->cluster_bytes) amount=w->cluster_bytes;
            memcpy(data,(const uint8_t *)bytes+at,amount);at+=amount;
            if (count+1==keep) fat_set(b,w,c,0xfffffff);
        } else {fat_set(b,w,c,0);w->free_clusters++;if (c<w->next_free) w->next_free=c;}
        count++;c=nc;
    }
    put32(b+w->entry[slot]+28,(uint32_t)n);
}
int install_fat_personalize(void *bytes,size_t n,const install_fat_patch_t *patch,install_fat_work_t *w,uint64_t *lba) {
    if (!patch || !lba || !patch->bootstrap || !patch->config || !patch->bootstrap_size ||
        patch->bootstrap_size>sizes[0] || !patch->config_size || patch->config_size>4096 ||
        !disjoint(bytes,n,patch->bootstrap,patch->bootstrap_size) || !disjoint(bytes,n,patch->config,patch->config_size) ||
        !disjoint(bytes,n,patch,sizeof(*patch)) || !disjoint(bytes,n,lba,sizeof(*lba)) ||
        !disjoint(w,sizeof(*w),patch,sizeof(*patch)) || !disjoint(w,sizeof(*w),lba,sizeof(*lba)) ||
        !disjoint(lba,sizeof(*lba),patch,sizeof(*patch)) ||
        !disjoint(lba,sizeof(*lba),patch->bootstrap,patch->bootstrap_size) ||
        !disjoint(lba,sizeof(*lba),patch->config,patch->config_size) ||
        !disjoint(w,sizeof(*w),patch->bootstrap,patch->bootstrap_size) || !disjoint(w,sizeof(*w),patch->config,patch->config_size)) return -1;
    if (install_fat_validate(bytes,n,w)) return -1;
    uint8_t *b=bytes;*lba=cluster_offset(w,w->first[1])/512;
    replace(b,w,0,patch->bootstrap,patch->bootstrap_size);
    uint8_t readiness[512]={0};replace(b,w,1,readiness,sizeof(readiness));
    for (unsigned i=2;i<INSTALL_FAT_SLOTS;i++) replace(b,w,i,patch->config,patch->config_size);
    for (unsigned i=0;i<2;i++) {
        uint8_t *info=b+w->fsinfo+i*w->backup;
        put32(info+488,w->free_clusters);put32(info+492,w->next_free);
    }
    return 0;
}
