#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "install_payload.h"
#include "../user/tools/digest.h"

/* Labelled file-device adapter: accepted sectors persist immediately. Flush is
 * fsync, faults return before dispatch unless 'after' is selected. No NVMe,
 * volatile-cache, IRQ, sector-tear or filesystem provisioning claim. */
static const uint64_t sizes[]={INSTALL_ESP_BYTES,INSTALL_ROOT_BYTES,INSTALL_BIOS_BYTES,440};
typedef struct {
    int fd;
    uint64_t sectors,writes,reads,flushes,admissions,provisions,source_reads;
    int fail_admit,fail_provision,fail_flush,after,short_write,corrupt_read;
    uint64_t fail_write;
    bool bad_slot,source_corrupt;
} fixture_t;
static void put32(uint8_t *p,uint32_t n) {for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8));}
static void put64(uint8_t *p,uint64_t n) {for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(n>>(i*8));}
static void hash(const void *p,size_t n,uint8_t out[32]) {
    digest_t d;digest_init(&d,DIGEST_SHA256);assert(digest_update(&d,p,n));digest_final(&d,out);
}
static void pattern(install_role_t role,uint64_t off,void *bytes,size_t n) {
    uint8_t *p=bytes;
    for (size_t i=0;i<n;i++) p[i]=(role==INSTALL_ESP && off+i>=4096 && off+i<4608) ? 0 : (uint8_t)(role+off+i);
}
static int source_read(void *ctx,install_role_t role,uint64_t off,void *bytes,size_t n) {
    fixture_t *f=ctx;f->source_reads++;
    assert(role<INSTALL_ROLES && off<=sizes[role] && n<=sizes[role]-off);
    pattern(role,off,bytes,n);
    if (f->bad_slot && role==INSTALL_ESP && off<=4096 && n>4096-off) ((uint8_t *)bytes)[4096-off]=1;
    if (f->source_corrupt && role==INSTALL_ROOT && off==0) ((uint8_t *)bytes)[0]^=1;
    return 0;
}
static int admit(void *ctx) {fixture_t *f=ctx;f->admissions++;return f->fail_admit ? -13 : 0;}
static int read_disk(void *ctx,uint64_t lba,uint32_t count,void *p) {
    fixture_t *f=ctx;assert(count && count<=8 && lba<f->sectors && count<=f->sectors-lba);f->reads++;
    assert(pread(f->fd,p,count*512,(off_t)(lba*512))==(ssize_t)(count*512));
    if (f->corrupt_read) ((uint8_t *)p)[0]^=1;
    return 0;
}
static int write_disk(void *ctx,uint64_t lba,uint32_t count,const void *p) {
    fixture_t *f=ctx;assert(count && count<=8 && lba<f->sectors && count<=f->sectors-lba);f->writes++;
    bool fail=f->fail_write==f->writes;
    if (fail && !f->after) return -5;
    size_t n=count*512;if (fail && f->short_write) n-=512;
    assert(pwrite(f->fd,p,n,(off_t)(lba*512))==(ssize_t)n);
    return fail ? f->short_write ? 1 : -5 : 0;
}
static int flush(void *ctx) {
    fixture_t *f=ctx;f->flushes++;
    if (f->fail_flush==(int)f->flushes && !f->after) return -5;
    assert(!fsync(f->fd));return f->fail_flush==(int)f->flushes ? -5 : 0;
}
static int provision(void *ctx) {
    fixture_t *f=ctx;f->provisions++;
    uint8_t slot[512];assert(!read_disk(ctx,INSTALL_ESP_LBA+8,1,slot));
    for (unsigned i=0;i<512;i++) assert(!slot[i]);
    return f->fail_provision ? -28 : 0;
}
static void manifest(uint8_t m[256]) {
    memset(m,0,256);memcpy(m,"FOSPAY01",8);put32(m+8,1);put32(m+12,256);
    uint8_t buffer[4096];
    for (unsigned role=0;role<INSTALL_ROLES;role++) {
        put64(m+16+role*8,sizes[role]);digest_t d;digest_init(&d,DIGEST_SHA256);
        for (uint64_t off=0;off<sizes[role];) {
            size_t n=sizes[role]-off>sizeof(buffer) ? sizeof(buffer) : (size_t)(sizes[role]-off);
            pattern((install_role_t)role,off,buffer,n);assert(digest_update(&d,buffer,n));off+=n;
        }
        digest_final(&d,m+48+role*32);
    }
    hash(m,224,m+224);
}
static void reset(fixture_t *f,uint64_t sectors) {
    int fd=f->fd;*f=(fixture_t){.fd=fd,.sectors=sectors};assert(!ftruncate(fd,0));assert(!ftruncate(fd,(off_t)(sectors*512)));
}
static install_target_t target(fixture_t *f) {
    return (install_target_t){.context=f,.sectors=f->sectors,.sector_size=512,
        .admit=admit,.read=read_disk,.write=write_disk,.flush=flush,.provision=provision};
}
int main(int argc,char **argv) {
    assert(argc==2);
    /* Never open an existing path. Runner supplies a fresh owned fixture directory. */
    int fd=open(argv[1],O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);assert(fd>=0);
    struct stat st;assert(!fstat(fd,&st) && S_ISREG(st.st_mode));
    fixture_t f={.fd=fd};uint64_t sectors=384u*2048;reset(&f,sectors);
    install_payload_work_t *work=calloc(1,sizeof(*work));assert(work);
    install_source_t src={.context=&f,.read=source_read};uint8_t m[256];manifest(m);
    install_plan_t p={.readiness_lba=8};
    uint8_t *ids[]={p.disk_guid,p.partition_guid[0],p.partition_guid[1],p.partition_guid[2]};
    for (unsigned i=0;i<4;i++) {ids[i][0]=(uint8_t)(i+1);ids[i][7]=0x40;ids[i][8]=0x80;}
    const char readiness[]="synthetic-readiness-workbench";
    memcpy(p.readiness,readiness,sizeof(readiness)-1);
    install_target_t t=target(&f);install_result_t result;unsigned cases=0;
    for (unsigned i=0;i<256;i++) {
        m[i]^=1;assert(install_payload_deploy(m,256,&src,&t,&p,true,work,&result)<0 && !result.complete);m[i]^=1;cases++;
    }
    assert(!f.writes && !f.admissions && !f.flushes && !f.reads);
    assert(install_payload_deploy(m,255,&src,&t,&p,false,work,&result)==-22);cases++;
    uint8_t bad[256];memcpy(bad,m,256);bad[16]^=1;hash(bad,224,bad+224);
    assert(install_payload_deploy(bad,256,&src,&t,&p,false,work,&result)==-22);cases++;
    memcpy(bad,m,256);bad[176]=1;hash(bad,224,bad+224);
    assert(install_payload_deploy(bad,256,&src,&t,&p,false,work,&result)==-22);cases++;
    t.sectors=INSTALL_MIN_SECTORS-1;assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-22);cases++;t=target(&f);
    t.sector_size=4096;assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-22);cases++;t=target(&f);
    install_plan_t duplicate=p;memcpy(duplicate.disk_guid,duplicate.partition_guid[0],16);
    assert(install_payload_deploy(m,256,&src,&t,&duplicate,false,work,&result)==-22);cases++;
    assert(!install_payload_deploy(m,256,&src,&t,&p,true,work,&result) && !result.complete);cases++;
    assert(!f.writes && !f.admissions && !f.flushes && !f.reads);
    f.source_corrupt=true;assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-22);cases++;f.source_corrupt=false;
    f.bad_slot=true;assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-22);cases++;f.bad_slot=false;
    f.fail_admit=true;assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-13 && result.stage==INSTALL_STAGE_ADMIT);cases++;
    assert(!f.writes && !f.flushes && !f.reads);reset(&f,sectors);
    for (unsigned i=0;i<6;i++) {
        reset(&f,sectors);f.fail_write=i+1;
        assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-5 && !result.complete && f.writes==i+1);
        assert(result.stage==INSTALL_STAGE_INVALIDATE);cases++;
    }
    for (unsigned i=1;i<=4;i++) for (unsigned after=0;after<2;after++) {
        reset(&f,sectors);f.fail_flush=(int)i;f.after=(int)after;
        assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-5 && !result.complete && f.flushes==i);cases++;
    }
    reset(&f,sectors);f.fail_provision=true;
    assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-28 && result.stage==INSTALL_STAGE_PROVISION && !result.complete);cases++;
    reset(&f,sectors);f.fail_write=80;f.after=1;f.short_write=1;
    assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-5 && !result.complete && f.writes==80);cases++;
    reset(&f,sectors);f.corrupt_read=1;
    assert(install_payload_deploy(m,256,&src,&t,&p,false,work,&result)==-5 && result.stage==INSTALL_STAGE_LAYOUT && !result.complete);cases++;
    /* Leave the successful actual-engine output for an independent Python oracle. */
    reset(&f,sectors);assert(!install_payload_deploy(m,256,&src,&t,&p,false,work,&result));
    assert(result.complete && result.stage==INSTALL_STAGE_COMPLETE && result.copied_bytes==INSTALL_ESP_BYTES+(uint64_t)INSTALL_ROOT_BYTES+INSTALL_BIOS_BYTES);
    assert(f.flushes==4 && f.admissions==1 && f.provisions==1);cases++;
    free(work);assert(!close(fd));printf("installer deployment workbench: %u cases PASS\n",cases);
    return 0;
}
