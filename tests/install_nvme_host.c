/* Actual installer transport policy; NVMe Identify/block/clock are adapters.
 * This does not exercise driver MMIO, DMA, polling deadlines or hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "install_nvme.h"
#include "install_control.h"
#include "install_runtime.h"
#include "vfs.h"
/* Deployment engine has its own actual-code integration gate. */
int install_runtime_deploy(const boot_info_t *boot,const install_backend_t *backend,
    const install_identity_v1_t *id,const install_deploy_v1_t *request,install_status_v1_t *status) {
    (void)boot;(void)backend;(void)id;(void)request;(void)status;return -VFS_EOPNOTSUPP;
}
#include "nvme.h"
#include "ext4.h"
#include "../user/tools/digest.h"
static install_backend_t registered;
static install_identity_v1_t hardware;
static bool bad_inventory,mounted,prepared,fatal,quarantined,io_failure;
static unsigned writes,reads,flushes,registrations;
static uint8_t header[512];
static bool deploying;
void install_control_boot(const install_backend_t *b,bool mode) {assert(mode);registered=*b;registrations++;}
void install_identity_digest(install_identity_v1_t *id) {
    digest_t d;digest_init(&d,DIGEST_SHA256);assert(digest_update(&d,id,128));digest_final(&d,id->digest);
}
size_t block_get_dev_count(void) {return mounted;}
bool ext4_storage_owned(void) {assert(!deploying);return prepared;}
bool nvme_is_fatal(void) {return fatal;}
bool nvme_is_dma_quarantined(void) {return quarantined;}
bool nvme_installer_identity(install_identity_v1_t *id) {if (bad_inventory) return false;*id=hardware;return true;}
bool nvme_read_sector(uint64_t lba,void *bytes) {assert(lba==1);reads++;if (io_failure) return false;memcpy(bytes,header,512);return true;}
bool nvme_read_sectors(uint64_t lba,uint32_t count,void *bytes) {(void)lba;reads++;if (io_failure) return false;memset(bytes,0,count*512);return true;}
bool nvme_write_sectors(uint64_t lba,uint32_t count,const void *bytes) {(void)lba;(void)count;(void)bytes;writes++;return !io_failure;}
bool nvme_flush(void) {flushes++;return !io_failure;}
#include "../src/kernel/install_nvme.c"
static boot_info_t info;
static void reset(void) {
    started=failed=false;optical=false;bad_inventory=mounted=prepared=fatal=quarantined=io_failure=false;
    registered=(install_backend_t){0};registrations=writes=reads=flushes=0;baseline=(install_identity_v1_t){0};
    hardware=(install_identity_v1_t){.size=192,.version=1,.generation=1,.sector_count=786432,.sector_size=512,.nsid=1,.flags=INSTALL_ID_ELIGIBLE};
    memcpy(hardware.serial,"fixture NVMe",12);install_identity_digest(&hardware);
    info=(boot_info_t){.installer_source_valid=true,.installer_source_optical=true};memcpy(info.cmdline,"boot_mode=installer",20);
    memset(header,0,512);
}
static void make_header(uint8_t guid) {
    memcpy(header,"EFI PART",8);header[10]=1;header[12]=92;header[56]=guid;
    uint32_t crc=crc32(header,92);for (unsigned i=0;i<4;i++) header[16+i]=(uint8_t)(crc>>(i*8));
}
int main(void) {
    reset();assert(!install_nvme_boot(&info,false) && !registrations && !writes && !reads);
    info.installer_source_valid=false;assert(!install_nvme_boot(&info,true) && !registrations);
    reset();mounted=true;assert(!install_nvme_boot(&info,true) && !registrations);
    reset();prepared=true;assert(!install_nvme_boot(&info,true) && !registrations);
    reset();bad_inventory=true;assert(!install_nvme_boot(&info,true) && failed && !writes);
    reset();memcpy(info.cmdline,"boot_mode=live",15);assert(!install_nvme_boot(&info,true) && !registrations);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;make_header(1);
    assert(!install_nvme_boot(&info,true) && !registrations && !writes);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;make_header(2);
    header[16]^=1;assert(!install_nvme_boot(&info,true) && failed && !writes);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;make_header(2);
    assert(install_nvme_boot(&info,true) && registrations==1);memset(header,0,512);
    install_identity_v1_t out;assert(!registered.identity(NULL,&out));assert(!memcmp(&out,&hardware,sizeof(out)));
    hardware.serial[0]^=1;install_identity_digest(&hardware);
    assert(registered.identity(NULL,&out)==-VFS_EIO && failed);
    uint8_t bytes[512]={0};assert(registered.write(NULL,0,1,bytes)==-VFS_EIO && !writes);
    reset();assert(install_nvme_boot(&info,true));prepared=true;
    deploying=true;assert(!registered.identity(NULL,&out));deploying=false;
    assert(registered.identity(NULL,&out)==-VFS_EIO && !writes);
    reset();assert(install_nvme_boot(&info,true));io_failure=true;assert(registered.read(NULL,0,1,bytes)==-VFS_EIO && failed);
    io_failure=false;assert(registered.flush(NULL)==-VFS_EIO && !flushes);
    reset();assert(install_nvme_boot(&info,true));assert(!registered.write(NULL,0,1,bytes) && writes==1);
    io_failure=true;assert(registered.flush(NULL)==-VFS_EIO && failed && flushes==1);
    puts("PASS actual installer NVMe policy: provenance, parent exclusion, invalid GPT, active/prepared storage, replacement and terminal I/O failure");
}
