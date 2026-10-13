/* Actual installer transport policy; NVMe Identify/block/clock are adapters.
 * This does not exercise driver MMIO, DMA, polling deadlines or hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "install_nvme.h"
#include "install_control.h"
#include "install_runtime.h"
#include "vfs.h"
void console_puts(const char *s) {(void)s;}
void console_terminal_write(const char *s,size_t n) {(void)s;(void)n;}
void serial_puts(const char *s) {(void)s;}
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
static uint8_t mbr[512],backup[512];
static bool deploying;
bool install_control_owned(void) {return false;}
void install_control_boot(const install_backend_t *b,bool mode) {assert(mode);registered=*b;registrations++;}
void install_identity_digest(install_identity_v1_t *id) {
    digest_t d;digest_init(&d,DIGEST_SHA256);assert(digest_update(&d,id,128));digest_final(&d,id->digest);
}
size_t block_get_dev_count(void) {return mounted;}
bool ext4_storage_owned(void) {assert(!deploying);return prepared;}
bool nvme_is_fatal(void) {return fatal;}
bool nvme_is_dma_quarantined(void) {return quarantined;}
bool nvme_installer_identity(install_identity_v1_t *id) {if (bad_inventory) return false;*id=hardware;return true;}
bool nvme_read_sector(uint64_t lba,void *bytes) {
    reads++;if (io_failure) return false;
    const uint8_t *source=lba==0 ? mbr : lba==1 ? header : backup;
    assert(lba==0 || lba==1 || lba==hardware.sector_count-1);
    memcpy(bytes,source,512);return true;
}
bool nvme_read_sectors(uint64_t lba,uint32_t count,void *bytes) {(void)lba;reads++;if (io_failure) return false;memset(bytes,0,count*512);return true;}
bool nvme_write_sectors(uint64_t lba,uint32_t count,const void *bytes) {(void)lba;(void)count;(void)bytes;writes++;return !io_failure;}
bool nvme_flush(void) {flushes++;return !io_failure;}
#ifdef INSTALLER_MULTI
static install_identity_v1_t inventory_ids[2];
static bool inventory_ok=true;
static unsigned inventory_index;
size_t nvme_inventory_boot(void) {return inventory_ok ? 2 : 0;}
bool nvme_inventory_valid(void) {return inventory_ok;}
bool nvme_inventory_select(unsigned index) {
    if (!inventory_ok || index>=2) return false;
    inventory_index=index;hardware=inventory_ids[index];return true;
}
#endif
#include "../src/kernel/install_nvme.c"
static boot_info_t info;
static void reset(void) {
#ifdef INSTALLER_MULTI
    multi_count=multi_selected=0;inventory_ok=true;
    memset(multi_ids,0,sizeof(multi_ids));memset(multi_raw,0,sizeof(multi_raw));
#endif
#ifdef INSTALLER_PHYSICAL
    physical_bound=false;memset(physical_expected,0,sizeof(physical_expected));
#endif
    started=failed=false;optical=false;bad_inventory=mounted=prepared=fatal=quarantined=io_failure=false;
    registered=(install_backend_t){0};registrations=writes=reads=flushes=0;baseline=(install_identity_v1_t){0};
    hardware=(install_identity_v1_t){.size=192,.version=1,.generation=1,.sector_count=786432,.sector_size=512,.nsid=1,.flags=INSTALL_ID_ELIGIBLE};
    memcpy(hardware.serial,"fixture NVMe",12);install_identity_digest(&hardware);
    info=(boot_info_t){.installer_source_valid=true,.installer_source_optical=true};memcpy(info.cmdline,"boot_mode=installer",20);
    memset(header,0,512);
    memset(mbr,0,512);memset(backup,0,512);
}
static void make_header(uint8_t guid) {
    memcpy(header,"EFI PART",8);header[10]=1;header[12]=92;header[56]=guid;
    uint32_t crc=crc32(header,92);for (unsigned i=0;i<4;i++) header[16+i]=(uint8_t)(crc>>(i*8));
}
int main(void) {
#ifdef INSTALLER_MULTI
    reset();info.installer_payload_valid=true;inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    inventory_ids[1].serial[0]='2';install_identity_digest(&inventory_ids[1]);
    assert(install_nvme_multi_boot(&info) && registrations==1 && !writes && !flushes);
    install_identity_v1_t listed;
    assert(registered.enumerate(NULL,0,&listed)==1 && listed.serial[0]!='2');
    assert(registered.enumerate(NULL,1,&listed)==1 && listed.serial[0]=='2');
    assert(!registered.enumerate(NULL,2,&listed));
    assert(!registered.select(NULL,1) && inventory_index==1);
    assert(!registered.identity(NULL,&listed) && listed.serial[0]=='2');
    inventory_ids[1].serial[1]^=1;install_identity_digest(&inventory_ids[1]);
    assert(registered.identity(NULL,&listed)==-VFS_EIO && failed);
    reset();info.installer_payload_valid=true;inventory_ok=false;
    assert(!install_nvme_multi_boot(&info) && !registrations && !writes);inventory_ok=true;
    /* Non-optical boot admits drive with existing MBR and detects partition summary */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    mbr[510]=0x55;mbr[511]=0xAA;mbr[446+4]=0x07;
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    inventory_ids[1].serial[0]='2';install_identity_digest(&inventory_ids[1]);
    assert(install_nvme_multi_boot(&info) && registrations==1);
    assert(registered.enumerate(NULL,0,&listed)==1 && listed.flags==INSTALL_ID_ELIGIBLE);
    assert(listed.reserved2[0]==INSTALL_SUMMARY_VALID && listed.reserved2[1]==INSTALL_SUMMARY_MBR);
    assert(listed.reserved2[2]==1 && (listed.reserved2[3]&INSTALL_SUMMARY_WINDOWS));
    assert(!registered.select(NULL,0) && inventory_index==0);
    assert(!registered.identity(NULL,&listed) && listed.flags==INSTALL_ID_ELIGIBLE);

    /* Non-optical boot flags drive matching source_guid as INSTALL_ID_SOURCE and denies lease */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    info.installer_source_disk_guid[0]=1;make_header(1);
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    inventory_ids[1].serial[0]='2';install_identity_digest(&inventory_ids[1]);
    assert(install_nvme_multi_boot(&info) && registrations==1);
    assert(registered.enumerate(NULL,0,&listed)==1 && (listed.flags & INSTALL_ID_SOURCE));
    assert(registered.select(NULL,0)==-VFS_EACCES);

    /* Non-matching GPT is admitted as eligible */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    info.installer_source_disk_guid[0]=1;make_header(2);
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    inventory_ids[1].serial[0]='2';install_identity_digest(&inventory_ids[1]);
    assert(install_nvme_multi_boot(&info) && registrations==1);
    assert(registered.enumerate(NULL,0,&listed)==1 && listed.flags==INSTALL_ID_ELIGIBLE);
    assert(!registered.select(NULL,0) && !registered.identity(NULL,&listed));

    /* A read error never becomes a writable target. */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;io_failure=true;
    assert(!install_nvme_multi_boot(&info) && failed && !registrations && !writes && !flushes);

    /* Damaged primary still excludes a source identified by its backup. */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    info.installer_source_disk_guid[0]=1;make_header(1);memcpy(backup,header,512);header[0]=0;
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    assert(install_nvme_multi_boot(&info));
    assert(registered.enumerate(NULL,0,&listed)==1 && listed.flags==INSTALL_ID_SOURCE);
    assert(registered.select(NULL,0)==-VFS_EACCES && !writes);

    /* Invalid unrelated GPT is listed but cannot acquire a write lease. */
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    info.installer_source_disk_guid[0]=1;make_header(2);header[16]^=1;
    inventory_ids[0]=hardware;inventory_ids[1]=hardware;
    assert(install_nvme_multi_boot(&info));
    assert(registered.enumerate(NULL,0,&listed)==1 && !listed.flags);
    assert(registered.select(NULL,0)==-VFS_EACCES && !writes);

    puts("PASS multi-drive transport policy: indexed identity/selection, replacement and incomplete inventory fail closed");
    return 0;
#endif
#ifdef INSTALLER_PHYSICAL
    reset();info.installer_payload_valid=true;
    assert(!install_nvme_physical_boot(&info) && failed && !reads && !writes && !flushes && !registrations);
    reset();info.installer_payload_valid=true;
    install_identity_v1_t bound=hardware;bound.flags=0;install_identity_digest(&bound);
    assert(physical_boot_expected(&info,bound.digest) && registrations==1 && !reads && !writes);
    install_identity_v1_t revalidated;assert(!registered.identity(NULL,&revalidated));
    hardware.serial[0]^=1;install_identity_digest(&hardware);
    assert(registered.identity(NULL,&revalidated)==-VFS_EIO && failed);
    uint8_t untouched[512]={0};assert(registered.write(NULL,0,1,untouched)==-VFS_EIO && !writes);
    reset();info.installer_payload_valid=true;info.installer_source_optical=false;
    info.installer_source_disk_guid[0]=1;make_header(1);
    bound=hardware;bound.flags=0;install_identity_digest(&bound);
    assert(!physical_boot_expected(&info,bound.digest) && !registrations && !writes);
    reset();info.installer_payload_valid=false;
    assert(!physical_boot_expected(&info,bound.digest) && !reads && !registrations);
    reset();info.installer_payload_valid=true;info.installer_source_valid=false;
    assert(!physical_boot_expected(&info,bound.digest) && !reads && !registrations);
    reset();info.installer_payload_valid=true;mounted=true;
    assert(!physical_boot_expected(&info,bound.digest) && !reads && !registrations);
    reset();info.installer_payload_valid=true;prepared=true;
    assert(!physical_boot_expected(&info,bound.digest) && !reads && !registrations);
    puts("PASS bound physical policy: wrong drive zero sector access, normalized digest, replacement, source and storage exclusion");
#endif
#ifdef TEST_INSTALLER_INSPECT
    reset();assert(install_nvme_inspect_boot(&info) && registrations==1);
    assert(!registered.read && !registered.write && !registered.flush && !registered.deploy);
    install_identity_v1_t inspected;assert(!registered.identity(NULL,&inspected));
    assert(!inspected.flags && !memcmp(inspected.serial,hardware.serial,20) && !reads && !writes && !flushes);
    reset();memcpy(info.cmdline,"boot_mode=live",15);assert(!install_nvme_inspect_boot(&info) && !registrations);
    reset();bad_inventory=true;assert(!install_nvme_inspect_boot(&info) && !registrations);
    puts("PASS identity-only inspection: real identity, ineligible lease, no sector/deploy callbacks");
#endif
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;
    assert(install_nvme_boot(&info,true) && reads==3 && registrations==1 && !writes && !flushes);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;mbr[510]=0x55;
    assert(!install_nvme_boot(&info,true) && failed && !writes && !registrations);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;backup[0]='E';
    assert(!install_nvme_boot(&info,true) && failed && !writes && !registrations);
    reset();info.installer_source_optical=false;info.installer_source_disk_guid[0]=1;header[0]=1;
    assert(!install_nvme_boot(&info,true) && failed && !writes && !registrations);
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
