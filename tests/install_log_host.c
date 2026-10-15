/* Actual bounded log writer, disposable regular-file adapter only. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "install_log.h"
#include "vfs.h"
static int disk;
static unsigned writes,flushes;
static bool fail_write,fail_flush,fail_read,corrupt_read;
static bool read_sector(block_dev_t *d,uint64_t l,void *p) {
    (void)d;if (fail_read) return false;
    bool ok=pread(disk,p,512,l*512)==512;
    if (ok && corrupt_read) ((uint8_t *)p)[0]^=1;
    return ok;
}
static void diagnostic(const char *reason) {
    size_t n;const char *s=install_log_snapshot(&n);
    assert(n<4096 && strstr(s,reason));
}
static bool write_sector(block_dev_t *d,uint64_t l,const void *p) {
    (void)d;assert(l>=INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA && l<INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA+8);writes++;
    return !fail_write && pwrite(disk,p,512,l*512)==512;
}
static bool flush(block_dev_t *d) {(void)d;flushes++;return !fail_flush && !fsync(disk);}
bool block_read_sector(block_dev_t *d,uint64_t l,void *p) {return d && l<d->sector_count && d->read_sector && d->read_sector(d,l,p);}
bool block_write_sector(block_dev_t *d,uint64_t l,const void *p) {return d && l<d->sector_count && d->write_sector && d->write_sector(d,l,p);}
bool block_flush(block_dev_t *d) {return d && d->flush && d->flush(d);}
int main(int argc,char **argv) {
    assert(argc==2);disk=open(argv[1],O_RDWR);assert(disk>=0);struct stat st;assert(!fstat(disk,&st) && S_ISREG(st.st_mode));
    block_dev_t d={.sector_size=512,.sector_count=(uint64_t)st.st_size/512,.read_sector=read_sector,.write_sector=write_sector,.flush=flush};
    boot_info_t b={.installer_source_valid=true};uint8_t sector[512];assert(read_sector(&d,1,sector));memcpy(b.installer_source_disk_guid,sector+56,16);
    snprintf(b.cmdline,sizeof(b.cmdline),"%s","boot_mode=installer installer_log_sha256=6344b66005859b74133e2d217b4676beae033489c7addf8bc0838bc25a82e827");
    assert(install_log_bind(&d,&b));assert(!writes);
    install_log_event_v1_t e={.size=32,.version=1};assert(!install_log_event(&e));
    install_identity_v1_t id={.sector_count=512u*2048,.sector_size=512};memcpy(id.model,"SK Hynix test",13);install_log_target(&id,"fabio");
    for (unsigned stage=0;stage<8;stage++) install_log_stage(stage);
    install_log_result(7,0,true);assert(install_log_usb_ok());
    char bytes[4097]={0};assert(pread(disk,bytes,4096,(INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA)*512)==4096);
    assert(strstr(bytes,"user=fabio\n") && strstr(bytes,"result=installation complete") && !strstr(bytes,"$5$"));
    assert(bytes[4095]=='\n');
    e.event=INSTALL_LOG_FAILED;e.stage=4;e.error=-5;e.call=-9;assert(!install_log_event(&e));
    assert(pread(disk,bytes,4096,(INSTALL_LOG_VOLUME_LBA+INSTALL_LOG_DATA_LBA)*512)==4096);
    assert(strstr(bytes,"Installation failed: stage=4 error=-5 call=-9"));
    unsigned before=writes;e.reserved=1;assert(install_log_event(&e)==-VFS_EINVAL && writes==before);e.reserved=0;
    fail_write=true;assert(install_log_event(&e)==-VFS_EIO && !install_log_usb_ok());diagnostic("writing the log data failed; sector=");before=writes;fail_write=false;
    assert(install_log_event(&e)==-VFS_EIO && writes==before);install_log_stage(2);assert(writes==before);
    assert(install_log_bind(&d,&b));fail_flush=true;assert(install_log_event(&e)==-VFS_EIO);diagnostic("flushing USB writes failed");before=writes;fail_flush=false;
    assert(install_log_event(&e)==-VFS_EIO && writes==before);
    assert(install_log_bind(&d,&b));fail_read=true;assert(install_log_event(&e)==-VFS_EIO);diagnostic("reading back the written log failed; sector=");fail_read=false;
    before=writes;assert(install_log_event(&e)==-VFS_EIO && writes==before);
    assert(install_log_bind(&d,&b));corrupt_read=true;assert(install_log_event(&e)==-VFS_EIO);diagnostic("written log did not match its read-back; sector=");corrupt_read=false;
    before=writes;assert(install_log_event(&e)==-VFS_EIO && writes==before);
    b.installer_source_disk_guid[0]^=1;assert(!install_log_bind(&d,&b) && writes==before);diagnostic("boot-source identity does not match");b.installer_source_disk_guid[0]^=1;
    b.installer_source_optical=true;assert(!install_log_bind(&d,&b));b.installer_source_optical=false;
    d.sector_size=4096;assert(!install_log_bind(&d,&b));d.sector_size=512;
    d.write_sector=NULL;assert(!install_log_bind(&d,&b));d.write_sector=write_sector;
    /* Metadata corruption must reject before writes: both FATs, directory and BPB. */
    const unsigned offsets[]={0,512,65*512,129*512+96};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++) {
        off_t at=(off_t)INSTALL_LOG_VOLUME_LBA*512+offsets[i];uint8_t old,x;assert(pread(disk,&old,1,at)==1);x=old^1;
        assert(pwrite(disk,&x,1,at)==1);assert(!install_log_bind(&d,&b) && writes==before);diagnostic("USB log filesystem metadata changed");assert(pwrite(disk,&old,1,at)==1);
    }
    install_log_unavailable(INSTALL_LOG_NO_USB);diagnostic("no initialized USB mass-storage transport");
    install_log_unavailable(INSTALL_LOG_UNKNOWN_DURABILITY);diagnostic("USB cache durability could not be established");
    install_log_unavailable(INSTALL_LOG_READ_ONLY);diagnostic("USB transport is read-only");
    assert(install_log_bind(&d,&b));e.event=INSTALL_LOG_START;e.stage=0;e.error=0;e.call=0;assert(!install_log_event(&e));
    e.event=INSTALL_LOG_FAILED;e.stage=4;e.error=INT32_MIN;e.call=INT64_MIN;
    for (unsigned i=0;i<100;i++) assert(!install_log_event(&e));
    size_t size;const char *recent=install_log_snapshot(&size);
    assert(size<4096 && !memcmp(recent,"FORTRESS-INSTALL-LOG-1\n",23) && strstr(recent,"call=-9223372036854775808"));
    install_log_target(&id,"fabio");for (unsigned stage=0;stage<8;stage++) install_log_stage(stage);install_log_result(7,0,true);
    close(disk);puts("PASS actual log slot: exact source/GPT/FAT binding, bounded file-only writes, readable results, immutable metadata, rejection and no retry after write/flush failure");
}
