#include "kernel_update.h"
#include "root_boot.h"
#include "gpt.h"
#include "crc32.h"
#include "string.h"
#include "../../user/tools/digest.h"

static uint32_t u32(const uint8_t *p) {uint32_t v=0;for (unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i);return v;}
static uint64_t u64(const uint8_t *p) {return u32(p)|((uint64_t)u32(p+4)<<32);}
static bool disjoint(const void *a,size_t an,const void *b,size_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=UINTPTR_MAX-an && y<=UINTPTR_MAX-bn && (x+an<=y || y+bn<=x);
}
static bool zero(const uint8_t *p,size_t n) {for (size_t i=0;i<n;i++) if (p[i]) return false;return true;}
static void hash(const void *p,size_t n,char out[65]) {
    static const char h[]="0123456789abcdef";digest_t d;uint8_t raw[32];
    digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,raw);
    for (unsigned i=0;i<32;i++) {out[2*i]=h[raw[i]>>4];out[2*i+1]=h[raw[i]&15];}out[64]=0;
}
static bool append(char *b,size_t cap,size_t *at,const char *s) {
    size_t n=strlen(s);if (*at>=cap || n>=cap-*at) return false;
    memcpy(b+*at,s,n);*at+=n;b[*at]=0;return true;
}
size_t kernel_update_menu(char *b,size_t cap,const char *uuid,unsigned active) {
    gpt_guid_t guid;if (!b || !uuid || active>1 || !gpt_str_to_guid(uuid,&guid)) return 0;
    size_t n=0;
    if (!append(b,cap,&n,active ? "default_entry: 3\ntimeout: 5\n" : "default_entry: 1\ntimeout: 5\n")) return 0;
    for (unsigned bank=0;bank<2;bank++) for (unsigned recovery=0;recovery<2;recovery++) {
        if (!append(b,cap,&n,bank ? "/FortressOS kernel B" : "/FortressOS kernel A") ||
            !append(b,cap,&n,recovery ? " authenticated recovery\n" : " installed\n") ||
            !append(b,cap,&n,"    protocol: limine\n    kernel_path: boot():/boot/") ||
            !append(b,cap,&n,bank ? "kernel-b.elf" : "fortress.elf") ||
            !append(b,cap,&n,"\n    kernel_cmdline: boot_mode=") ||
            !append(b,cap,&n,recovery ? "recovery" : "installed") ||
            !append(b,cap,&n," root=PARTUUID=") || !append(b,cap,&n,uuid) ||
            !append(b,cap,&n,"\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/") ||
            !append(b,cap,&n,bank ? "ready-b.v1" : "readiness.v1") ||
            !append(b,cap,&n,"\n    module_cmdline: role=readiness\n")) return 0;
    }
    return n;
}
static int read_runs(const kernel_update_io_t *io,uint64_t lba,uint8_t *p,unsigned sectors) {
    while (sectors) {unsigned take=sectors>8 ? 8 : sectors;
        if (io->read(io->context,lba,p,take)) return -1;
        lba+=take;p+=take*512;sectors-=take;
    }return 0;
}
static bool header(uint8_t *p,uint64_t at,uint64_t other,uint64_t count) {
    if (memcmp(p,"EFI PART",8) || u32(p+8)!=0x10000 || u32(p+12)!=92 || u32(p+20) ||
        u64(p+24)!=at || u64(p+32)!=other || u64(p+40)!=34 || u64(p+48)!=count-34 ||
        u64(p+72)!=(at==1 ? 2 : count-33) || u32(p+80)!=128 || u32(p+84)!=128 ||
        zero(p+56,16) || !zero(p+92,420)) return false;
    uint32_t expected=u32(p+16);memset(p+16,0,4);uint32_t actual=crc32(0,p,92);
    for (unsigned i=0;i<4;i++) p[16+i]=(uint8_t)(expected>>(8*i));
    return actual==expected;
}
static int disk(const kernel_update_io_t *io,kernel_update_work_t *w) {
    if (io->sectors<786432 || io->sectors>2147483655ULL ||
        io->read(io->context,1,w->primary,1) || io->read(io->context,io->sectors-1,w->backup,1) ||
        !header(w->primary,1,io->sectors-1,io->sectors) || !header(w->backup,io->sectors-1,1,io->sectors) ||
        memcmp(w->primary+40,w->backup+40,32) || memcmp(w->primary+80,w->backup+80,12) ||
        read_runs(io,2,w->entries,32) || read_runs(io,io->sectors-33,w->backup_entries,32) ||
        memcmp(w->entries,w->backup_entries,16384) || crc32(0,w->entries,16384)!=u32(w->primary+88)) return -1;
    /* Deliberately narrow enrollment: exact installer partition geometry and
     * exactly three nonzero entries. Reject other operating-system layouts. */
    static const uint8_t bios[16]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};
    const uint8_t *e=w->entries,*r=e+128,*a=r+128;
    if (memcmp(a,bios,16) || memcmp(e,GPT_GUID_ESP.bytes,16) || memcmp(r,GPT_GUID_LINUX_FS.bytes,16) ||
        u64(a+32)!=2048 || u64(a+40)!=4095 || u64(e+32)!=4096 || u64(e+40)!=135167 ||
        u64(r+32)!=135168 || u64(r+40)!=io->sectors-34 || !zero(w->entries+384,16384-384) ||
        zero(a+16,16) || zero(e+16,16) || zero(r+16,16) || !memcmp(a+16,e+16,16) ||
        !memcmp(a+16,r+16,16) || !memcmp(e+16,r+16,16)) return -1;
    w->esp=4096;w->root=135168;w->root_sectors=io->sectors-34-w->root+1;return 0;
}
static bool extent(const uint8_t *b,const install_fat_work_t *f,uint32_t c,uint32_t size,uint32_t *offset) {
    if (!size || f->cluster_bytes!=512 || c<2 || c>=f->clusters+2) return false;
    uint32_t first=c,count=(size+511)/512;
    for (uint32_t i=0;i<count;i++) {
        uint32_t next=u32(b+f->fat+c*4)&0xfffffff;
        if (i+1<count ? next!=c+1 : next<0xffffff8) return false;
        c=next;
    }
    *offset=f->data+(first-2)*512;return *offset<=KUPDATE_PREFIX_BYTES && size<=KUPDATE_PREFIX_BYTES-*offset;
}
static bool identity(const uint8_t *ready,root_recovery_t *r) {
    /* root_readiness_validate below validates the complete record and identity.
     * Copy only fields at the canonical offsets, never accept an alternate parser. */
    size_t at=strlen("FORTRESS-READY-1\ninstall_id=");
    memcpy(r->install_id,ready+at,32);r->install_id[32]=0;
    at+=32+strlen("\nroot_partuuid=");memcpy(r->partuuid,ready+at,36);r->partuuid[36]=0;
    at+=36+strlen("\nfilesystem_uuid=");memcpy(r->filesystem_uuid,ready+at,36);r->filesystem_uuid[36]=0;
    return true;
}
int kernel_update_prepare(const kernel_update_io_t *io,void *prefix,size_t n,kernel_update_work_t *w,const char *predecessor) {
    if (!io || !io->read || !io->write || !io->flush || !prefix || !w || !predecessor ||
        n!=KUPDATE_PREFIX_BYTES || strlen(predecessor)!=64 ||
        !disjoint(prefix,n,w,sizeof(*w)) || !disjoint(prefix,n,io,sizeof(*io)) ||
        !disjoint(w,sizeof(*w),io,sizeof(*io)) || !disjoint(prefix,n,predecessor,65) ||
        !disjoint(w,sizeof(*w),predecessor,65)) return -1;
    memset(w,0,sizeof(*w));if (disk(io,w) || read_runs(io,w->esp,prefix,n/512)) return -1;
    uint8_t *b=prefix;install_fat_work_t *f=&w->fat;
    if (install_fat_inspect(b,n,f) || install_fat_banked(f)!=1 || f->capacity[1]!=512 ||
        !extent(b,f,f->kernel_first,f->kernel_size,&w->kernel[0]) ||
        !extent(b,f,f->bank_first,f->bank_size,&w->kernel[1]) ||
        !extent(b,f,f->first[1],512,&w->readiness[0]) ||
        !extent(b,f,f->ready_b_first,512,&w->readiness[1]) ||
        !extent(b,f,f->first[0],f->capacity[0],&w->bootstrap)) return -1;
    w->bootstrap_size=f->capacity[0];root_recovery_t r={0};identity(b+w->readiness[0],&r);
    gpt_guid_t root_guid;if (!gpt_str_to_guid(r.partuuid,&root_guid) || memcmp(root_guid.bytes,w->entries+128+16,16)) return -1;
    for (unsigned bank=0;bank<2;bank++) if (!root_readiness_validate(b+w->readiness[bank],512,&r,
        b+w->kernel[bank],KUPDATE_BANK_BYTES,b+w->bootstrap,w->bootstrap_size)) return -1;
    /* The ext4 identity is read only; never replay or mount it. */
    if (io->read(io->context,w->root+2,w->verify,2) || w->verify[56]!=0x53 || w->verify[57]!=0xef ||
        !gpt_str_to_guid(r.filesystem_uuid,&root_guid)) return -1;
    /* ext4 UUIDs are network byte order; GPT text conversion is mixed endian. */
    const uint8_t *g=root_guid.bytes;uint8_t fsuuid[16]={g[3],g[2],g[1],g[0],g[5],g[4],g[7],g[6],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]};
    if (memcmp(w->verify+104,fsuuid,16)) return -1;
    char *menu=(char *)w->entries; /* GPT scratch is no longer needed. */
    if (!extent(b,f,f->first[2],f->capacity[2],&w->configs[0])) return -1;
    const char *config=(const char *)b+w->configs[0];
    if (f->capacity[2]<17 || memcmp(config,"default_entry: ",15) || (config[15]!='1' && config[15]!='3')) return -1;
    w->active=config[15]=='3';size_t size=kernel_update_menu(menu,sizeof(w->entries),r.partuuid,w->active);
    if (!size) return -1;
    for (unsigned i=0;i<4;i++) {
        if (f->capacity[i+2]!=size || !extent(b,f,f->first[i+2],f->capacity[i+2],&w->configs[i]) ||
            memcmp(b+w->configs[i],menu,size)) return -1;
        w->config_sizes[i]=(uint32_t)size;
    }
    char actual[65];hash(b+w->kernel[w->active],KUPDATE_BANK_BYTES,actual);
    if (strcmp(actual,predecessor)) return -1;
    memcpy(w->ready,b+w->readiness[w->active],512);w->prepared=true;return 0;
}
static int publish(const kernel_update_io_t *io,kernel_update_work_t *w,uint32_t offset,const uint8_t *p,unsigned bytes) {
    if ((offset|bytes)&511 || offset>KUPDATE_PREFIX_BYTES || bytes>KUPDATE_PREFIX_BYTES-offset) return -1;
    uint64_t lba=w->esp+offset/512;unsigned sectors=bytes/512;
    for (unsigned at=0;at<sectors;) {
        unsigned take=sectors-at>8 ? 8 : sectors-at;
        if (io->write(io->context,lba+at,p+at*512,take)) return -1;
        at+=take;
    }
    if (io->flush(io->context)) return -1;
    for (unsigned at=0;at<sectors;) {
        unsigned take=sectors-at>8 ? 8 : sectors-at;
        if (io->read(io->context,lba+at,w->verify,take) || memcmp(w->verify,p+at*512,take*512)) return -1;
        at+=take;
    }return 0;
}
int kernel_update_apply(const kernel_update_io_t *io,void *prefix,kernel_update_work_t *w,const void *candidate,size_t n) {
    if (!io || !prefix || !w || !w->prepared || !candidate || n<64 || n>KUPDATE_BANK_BYTES ||
        !disjoint(prefix,KUPDATE_PREFIX_BYTES,w,sizeof(*w)) ||
        !disjoint(prefix,KUPDATE_PREFIX_BYTES,candidate,n) || !disjoint(w,sizeof(*w),candidate,n) ||
        !disjoint(prefix,KUPDATE_PREFIX_BYTES,io,sizeof(*io)) || !disjoint(w,sizeof(*w),io,sizeof(*io))) return -1;
    w->prepared=false;const uint8_t *elf=candidate;
    if (memcmp(elf,"\177ELF\2\1\1",7) || elf[18]!=62 || elf[19]) return -1;
    uint8_t *b=prefix;unsigned bank=w->active^1;uint8_t *kernel=b+w->kernel[bank];
    memset(kernel,0,KUPDATE_BANK_BYTES);memcpy(kernel,candidate,n);
    char kh[65];hash(kernel,KUPDATE_BANK_BYTES,kh);
    /* Change exactly the kernel hash and record checksum in the canonical
     * already validated record. Everything else, including binding, stays. */
    char *ready=(char *)w->ready,*key=strstr(ready,"\nkernel_sha256="),*sum=strstr(ready,"\nchecksum=");
    if (!key || !sum || sum-ready>440) return -1;
    memcpy(key+strlen("\nkernel_sha256="),kh,64);hash(ready,(size_t)(sum+1-ready),kh);memcpy(sum+strlen("\nchecksum="),kh,64);
    root_recovery_t r={0};identity(w->ready,&r);
    if (!root_readiness_validate(w->ready,512,&r,kernel,KUPDATE_BANK_BYTES,b+w->bootstrap,w->bootstrap_size)) return -1;
    if (publish(io,w,w->kernel[bank],kernel,KUPDATE_BANK_BYTES) || publish(io,w,w->readiness[bank],w->ready,512)) return -1;
    for (unsigned i=0;i<4;i++) {
        uint8_t *menu=b+w->configs[i];menu[15]=bank ? '3' : '1';
        if (publish(io,w,w->configs[i],menu,512)) return -1;
    }
    return 0;
}
