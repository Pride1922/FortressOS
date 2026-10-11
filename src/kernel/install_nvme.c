#include "install_nvme.h"
#include "install_control.h"
#include "root_boot.h"
#include "nvme.h"
#include "block.h"
#include "ext4.h"
#include "vfs.h"
#include "string.h"
#include "install_runtime.h"

static bool started,failed,optical,deploying;
static const boot_info_t *source_boot;
static install_backend_t runtime_backend;
static uint8_t source_guid[16];
static install_identity_v1_t baseline;
static uint32_t crc32(const uint8_t *bytes,size_t n) {
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<n;i++) {crc^=bytes[i];for (unsigned j=0;j<8;j++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    return ~crc;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static int identity(void *context,install_identity_v1_t *out) {
    (void)context;
    if (failed || block_get_dev_count() || (!deploying && ext4_storage_owned()) || nvme_is_fatal() || nvme_is_dma_quarantined()) return -VFS_EIO;
    install_identity_v1_t id;
    if (!nvme_installer_identity(&id)) {failed=true;return -VFS_EIO;}
    /* Never automatically reinitialize a replaced/failing device in this boot. */
    if (started && memcmp(&id,&baseline,sizeof(id))) {failed=true;return -VFS_EIO;}
    if (!optical && !started) {
        uint8_t sector[512];
        if (!nvme_read_sector(1,sector) || memcmp(sector,"EFI PART",8) || le32(sector+8)!=0x10000 ||
            le32(sector+12)!=92 || le32(sector+20)) {failed=true;return -VFS_EIO;}
        uint32_t expected=le32(sector+16);memset(sector+16,0,4);
        if (crc32(sector,92)!=expected) {failed=true;return -VFS_EIO;}
        if (!memcmp(sector+56,source_guid,16)) id.flags=INSTALL_ID_SOURCE;
    }
    install_identity_digest(&id);*out=id;return 0;
}
static int read_run(void *context,uint64_t lba,uint32_t count,void *bytes) {
    (void)context;
    if (failed || !nvme_read_sectors(lba,count,bytes)) {failed=true;return -VFS_EIO;}
    return 0;
}
static int write_run(void *context,uint64_t lba,uint32_t count,const void *bytes) {
    (void)context;
    if (failed || !nvme_write_sectors(lba,count,bytes)) {failed=true;return -VFS_EIO;}
    return 0;
}
static int flush(void *context) {
    (void)context;
    if (failed || !nvme_flush()) {failed=true;return -VFS_EIO;}
    return 0;
}
static int deploy(void *context,const install_deploy_v1_t *request,install_status_v1_t *status) {
    (void)context;
    if (deploying || ext4_storage_owned()) return -VFS_EACCES;
    deploying=true;
    int r=install_runtime_deploy(source_boot,&runtime_backend,&baseline,request,status);
    deploying=false;
    if (r && !request->flags) failed=true;
    return r;
}
bool install_nvme_boot(const boot_info_t *info,bool disposable) {
    if (started || failed || !disposable || !info || !info->installer_source_valid || block_get_dev_count() || ext4_storage_owned()) return false;
    root_boot_config_t c=root_boot_parse(info->cmdline,sizeof(info->cmdline),info->cmdline_truncated);
    if (!c.valid || c.mode!=ROOT_BOOT_INSTALLER || !c.explicit_mode) return false;
    optical=info->installer_source_optical;memcpy(source_guid,info->installer_source_disk_guid,16);
    install_identity_v1_t id;
    if (identity(NULL,&id) || id.flags!=INSTALL_ID_ELIGIBLE) return false;
    baseline=id;started=true;
    source_boot=info;
    runtime_backend=(install_backend_t){.identity=identity,.read=read_run,.write=write_run,.flush=flush,.deploy=deploy};
    install_control_boot(&runtime_backend,true);return true;
}
