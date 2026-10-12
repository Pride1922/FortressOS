#include "install_payload.h"
#include "string.h"
#include "crc32.h"
#include "../../user/tools/digest.h"

/* Local errors use VFS-compatible errno values without requiring VFS state. */
#define BAD (-22)
#define IO (-5)
static const uint64_t lengths[INSTALL_ROLES]={INSTALL_ESP_BYTES,INSTALL_ROOT_BYTES,INSTALL_BIOS_BYTES,440};
static const uint64_t starts[3]={INSTALL_ESP_LBA,INSTALL_ROOT_LBA,INSTALL_BIOS_LBA};
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t le64(const uint8_t *p) {return le32(p)|(uint64_t)le32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t n) {for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n) {for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(n>>(8*i));}
static bool zero(const uint8_t *p,size_t n) {for (size_t i=0;i<n;i++) if (p[i]) return false;return true;}
static void hash(const void *p,size_t n,uint8_t out[32]) {
    digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,out);
}
static int shape(const void *bytes,size_t n,const install_source_t *source) {
    if (!bytes || n!=INSTALL_PAYLOAD_MANIFEST_BYTES || !source || !source->read) return BAD;
    const uint8_t *m=bytes;uint8_t h[32];hash(m,224,h);
    if (memcmp(m,"FOSPAY01",8) || le32(m+8)!=1 || le32(m+12)!=n ||
        !zero(m+176,48) || memcmp(h,m+224,32)) return BAD;
    for (unsigned i=0;i<INSTALL_ROLES;i++) if (le64(m+16+i*8)!=lengths[i]) return BAD;
    return 0;
}
int install_payload_validate(const void *bytes,size_t n,const install_source_t *source,uint8_t buffer[4096]) {
    if (!buffer) return BAD;
    int r=shape(bytes,n,source);if (r) return r;
    const uint8_t *m=bytes;uint8_t h[32];
    for (unsigned role=0;role<INSTALL_ROLES;role++) {
        digest_t d;digest_init(&d,DIGEST_SHA256);
        for (uint64_t off=0;off<lengths[role];) {
            size_t count=lengths[role]-off>INSTALL_CHUNK_BYTES ? INSTALL_CHUNK_BYTES : (size_t)(lengths[role]-off);
            r=source->read(source->context,(install_role_t)role,off,buffer,count);
            if (r) return r<0 ? r : IO;
            if (!digest_update(&d,buffer,count)) return BAD;
            off+=count;
        }
        digest_final(&d,h);if (memcmp(h,m+48+role*32,32)) return BAD;
    }
    return 0;
}
static bool uuid(const uint8_t *p) {
    /* UUID version nibble is byte 7 in GPT's mixed endian representation. */
    return !zero(p,16) && (p[7]&0xf0)==0x40 && (p[8]&0xc0)==0x80;
}
static int plan_valid(const install_target_t *t,const install_plan_t *p) {
    if (!t || !p || t->sector_size!=512 || t->sectors<INSTALL_MIN_SECTORS ||
        t->sectors>UINT64_MAX/512 || !t->admit || !t->read || !t->write || !t->flush || !t->provision ||
        p->readiness_lba>=INSTALL_ESP_BYTES/512 || !uuid(p->disk_guid) || zero(p->readiness,512)) return BAD;
    const uint8_t *ids[4]={p->disk_guid,p->partition_guid[0],p->partition_guid[1],p->partition_guid[2]};
    for (unsigned i=0;i<4;i++) {
        if (!uuid(ids[i])) return BAD;
        for (unsigned j=0;j<i;j++) if (!memcmp(ids[i],ids[j],16)) return BAD;
    }
    return 0;
}
static void entry(uint8_t *e,const uint8_t type[16],const uint8_t id[16],uint64_t start,uint64_t count,const char *name) {
    memcpy(e,type,16);memcpy(e+16,id,16);put64(e+32,start);put64(e+40,start+count-1);
    for (unsigned i=0;name[i] && i<36;i++) e[56+i*2]=(uint8_t)name[i];
}
static void entries(uint8_t *array,const install_plan_t *p) {
    static const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    static const uint8_t root[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    static const uint8_t bios[16]={0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49};
    memset(array,0,512);
    entry(array,esp,p->partition_guid[0],starts[0],lengths[0]/512,"Fortress ESP");
    entry(array+128,root,p->partition_guid[1],starts[1],lengths[1]/512,"Fortress Root");
    entry(array+256,bios,p->partition_guid[2],starts[2],lengths[2]/512,"Fortress BIOS");
}
static void header(uint8_t *h,uint64_t current,uint64_t backup,uint64_t array,uint64_t sectors,const uint8_t *id,uint32_t crc) {
    memset(h,0,512);memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,current);put64(h+32,backup);put64(h+40,34);put64(h+48,sectors-34);
    memcpy(h+56,id,16);put64(h+72,array);put32(h+80,128);put32(h+84,128);put32(h+88,crc);
    put32(h+16,crc32(0,h,92));
}
static int exact(int r) {return r>0 ? IO : r;}
static int checked_write(const install_target_t *t,uint64_t lba,uint32_t count,const void *bytes,uint8_t verify[4096]) {
    if (!count || count>8 || lba>=t->sectors || count>t->sectors-lba) return BAD;
    int r=exact(t->write(t->context,lba,count,bytes));if (r) return r;
    r=exact(t->read(t->context,lba,count,verify));if (r) return r;
    return memcmp(bytes,verify,count*512) ? IO : 0;
}
int install_payload_deploy(const void *bytes,size_t n,const install_source_t *source,
    const install_target_t *target,const install_plan_t *plan,bool dry_run,install_payload_work_t *work,install_result_t *result) {
    if (!result) return BAD;
    *result=(install_result_t){.stage=INSTALL_STAGE_VALIDATE};
    int r=work ? plan_valid(target,plan) : BAD;
    if (!r) r=install_payload_validate(bytes,n,source,work->data);
    if (!r) {
        uint8_t slot[512];
        r=exact(source->read(source->context,INSTALL_ESP,plan->readiness_lba*512,slot,sizeof(slot)));
        if (!r && !zero(slot,sizeof(slot))) r=BAD;
    }
    if (r || dry_run) {result->error=r;return r;}
    /* Caller keeps its source immutable for the whole operation. The engine also
     * rehashes each copied role; changed bytes cannot reach readiness publication. */
    uint8_t *data=work->data,*verify=work->verify,*first=work->first,*mbr=work->mbr;
    result->stage=INSTALL_STAGE_ADMIT;if (target->progress) target->progress(target->context,INSTALL_STAGE_ADMIT);
    r=exact(target->admit(target->context));if (r) goto done;
    result->stage=INSTALL_STAGE_INVALIDATE;if (target->progress) target->progress(target->context,INSTALL_STAGE_INVALIDATE);
    memset(data,0,INSTALL_CHUNK_BYTES);
    /* Invalidate old MBR, both GPT headers, old fixed ESP and root superblock
     * before deploying new bytes. An interrupted disk may need external rescue. */
    const uint64_t invalid[]={0,1,target->sectors-1,INSTALL_ESP_LBA,
        INSTALL_ESP_LBA+plan->readiness_lba,INSTALL_ROOT_LBA+2};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        r=exact(target->write(target->context,invalid[i],1,data));if (r) goto done;
    }
    r=exact(target->flush(target->context));if (r) goto done;
    result->stage=INSTALL_STAGE_LAYOUT;if (target->progress) target->progress(target->context,INSTALL_STAGE_LAYOUT);entries(first,plan);
    uint32_t crc=crc32(0,first,512);
    for (unsigned i=1;i<32;i++) crc=crc32(crc,data,512);
    for (unsigned side=0;side<2;side++) {
        uint64_t lba=side ? target->sectors-33 : 2;
        for (unsigned i=0;i<32;i++) {
            r=checked_write(target,lba+i,1,i ? data : first,verify);if (r) goto done;
        }
        header(data,side ? target->sectors-1 : 1,side ? 1 : target->sectors-1,lba,target->sectors,plan->disk_guid,crc);
        r=checked_write(target,side ? target->sectors-1 : 1,1,data,verify);if (r) goto done;
        memset(data,0,INSTALL_CHUNK_BYTES);
    }
    result->stage=INSTALL_STAGE_PAYLOAD;if (target->progress) target->progress(target->context,INSTALL_STAGE_PAYLOAD);
    const uint8_t *manifest=bytes;
    for (unsigned role=0;role<3;role++) {
        digest_t d;digest_init(&d,DIGEST_SHA256);
        for (uint64_t off=0;off<lengths[role];off+=INSTALL_CHUNK_BYTES) {
            r=exact(source->read(source->context,(install_role_t)role,off,data,INSTALL_CHUNK_BYTES));if (r) goto done;
            if (!digest_update(&d,data,INSTALL_CHUNK_BYTES)) {r=BAD;goto done;}
            if (role==INSTALL_ESP && off<=plan->readiness_lba*512 && plan->readiness_lba*512-off<INSTALL_CHUNK_BYTES &&
                !zero(data+plan->readiness_lba*512-off,512)) {r=BAD;goto done;}
            r=checked_write(target,starts[role]+off/512,8,data,verify);if (r) goto done;
            result->copied_bytes+=INSTALL_CHUNK_BYTES;
        }
        uint8_t h[32];digest_final(&d,h);if (memcmp(h,manifest+48+role*32,32)) {r=BAD;goto done;}
    }
    memset(mbr,0,512);
    r=exact(source->read(source->context,INSTALL_MBR,0,mbr,440));if (r) goto done;
    uint8_t mh[32];hash(mbr,440,mh);if (memcmp(mh,manifest+48+INSTALL_MBR*32,32)) {r=BAD;goto done;}
    mbr[447]=0;mbr[448]=2;mbr[450]=0xee;mbr[451]=mbr[452]=mbr[453]=0xff;
    put32(mbr+454,1);put32(mbr+458,target->sectors-1>UINT32_MAX ? UINT32_MAX : (uint32_t)(target->sectors-1));
    mbr[510]=0x55;mbr[511]=0xaa;
    r=checked_write(target,0,1,mbr,verify);if (r) goto done;
    r=exact(target->flush(target->context));if (r) goto done;
    result->stage=INSTALL_STAGE_PROVISION;if (target->progress) target->progress(target->context,INSTALL_STAGE_PROVISION);
    r=exact(target->provision(target->context));if (r) goto done;
    result->stage=INSTALL_STAGE_VERIFY;if (target->progress) target->progress(target->context,INSTALL_STAGE_VERIFY);
    /* Re-audit immutable ESP, BIOS, MBR and GPT after the provisioning handoff.
     * Root changes are validated/frozen by the provision callback, never compared
     * against original payload hashes after legitimate account changes. */
    for (unsigned role=0;role<3;role+=2) {
        digest_t d;digest_init(&d,DIGEST_SHA256);
        for (uint64_t off=0;off<lengths[role];off+=INSTALL_CHUNK_BYTES) {
            r=exact(target->read(target->context,starts[role]+off/512,8,data));if (r) goto done;
            (void)digest_update(&d,data,INSTALL_CHUNK_BYTES);
        }
        uint8_t h[32];digest_final(&d,h);if (memcmp(h,manifest+48+role*32,32)) {r=IO;goto done;}
    }
    r=exact(target->read(target->context,0,1,verify));if (r) goto done;
    if (memcmp(mbr,verify,512)) {r=IO;goto done;}
    entries(first,plan);
    memset(data,0,INSTALL_CHUNK_BYTES);
    for (unsigned side=0;side<2;side++) {
        uint64_t lba=side ? target->sectors-33 : 2;
        for (unsigned i=0;i<32;i++) {
            r=exact(target->read(target->context,lba+i,1,verify));if (r) goto done;
            if (memcmp(i ? data : first,verify,512)) {r=IO;goto done;}
        }
        header(data,side ? target->sectors-1 : 1,side ? 1 : target->sectors-1,lba,target->sectors,plan->disk_guid,crc);
        r=exact(target->read(target->context,side ? target->sectors-1 : 1,1,verify));if (r) goto done;
        if (memcmp(data,verify,512)) {r=IO;goto done;}
        memset(data,0,INSTALL_CHUNK_BYTES);
    }
    r=exact(target->flush(target->context));if (r) goto done;
    r=checked_write(target,INSTALL_ESP_LBA+plan->readiness_lba,1,plan->readiness,verify);if (r) goto done;
    r=exact(target->flush(target->context));if (r) goto done;
    r=exact(target->read(target->context,INSTALL_ESP_LBA+plan->readiness_lba,1,verify));if (r) goto done;
    if (memcmp(plan->readiness,verify,512)) {r=IO;goto done;}
    result->stage=INSTALL_STAGE_COMPLETE;if (target->progress) target->progress(target->context,INSTALL_STAGE_COMPLETE);result->complete=true;
done:
    result->error=r;return r;
}
