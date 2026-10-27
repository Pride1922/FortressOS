#include "install_nvme.h"
#include "install_control.h"
#include "root_boot.h"
#include "storage.h"
#include "block.h"
#include "ext4.h"
#include "vfs.h"
#include "string.h"
#include "install_runtime.h"
#include "console.h"
#include "serial.h"
#ifdef INSTALLER_PHYSICAL
#include "install_physical_identity.h"
static bool physical_bound;
static uint8_t physical_expected[32];
#endif

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
static bool zero_sector(const uint8_t *bytes) {
    for (unsigned i=0;i<512;i++) if (bytes[i]) return false;
    return true;
}
static int identity(void *context,install_identity_v1_t *out) {
    (void)context;
    if (failed || block_get_dev_count() || (!deploying && ext4_storage_owned()) || storage_is_fatal() || storage_is_dma_quarantined()) return -VFS_EIO;
    install_identity_v1_t id;
    if (!storage_installer_identity(&id)) {failed=true;return -VFS_EIO;}
#ifdef INSTALLER_PHYSICAL
    if (physical_bound) {
        install_identity_v1_t inspected=id;inspected.flags=0;
        install_identity_digest(&inspected);
        /* Reject before any target sector access, including source exclusion. */
        if (memcmp(inspected.digest,physical_expected,32)) {
            failed=true;return -VFS_EIO;
        }
    }
#endif
    /* Never automatically reinitialize a replaced/failing device in this boot. */
    if (started && memcmp(&id,&baseline,sizeof(id))) {failed=true;return -VFS_EIO;}
#ifndef INSTALLER_MULTI
    if (!optical && !started) {
        uint8_t sector[512];
        if (!storage_read_sector(1,sector)) {failed=true;return -VFS_EIO;}
        if (zero_sector(sector)) {
            /* Admit only an unformatted boundary tuple. A damaged primary on
             * the source still has a nonzero protective MBR or backup header.
             * This is read-only admission, never a GPT repair heuristic. */
            if (id.sector_count<2 || !storage_read_sector(0,sector) || !zero_sector(sector) ||
                !storage_read_sector(id.sector_count-1,sector) || !zero_sector(sector)) {
                failed=true;return -VFS_EIO;
            }
        } else {
            if (memcmp(sector,"EFI PART",8) || le32(sector+8)!=0x10000 ||
                le32(sector+12)!=92 || le32(sector+20)) {failed=true;return -VFS_EIO;}
            uint32_t expected=le32(sector+16);memset(sector+16,0,4);
            if (crc32(sector,92)!=expected) {failed=true;return -VFS_EIO;}
            if (!memcmp(sector+56,source_guid,16)) id.flags=INSTALL_ID_SOURCE;
        }
    }
#else
    if (!optical && !started) {
        uint8_t primary[512],backup[512],mbr[512];
        if (id.sector_count<2 || !storage_read_sector(1,primary) ||
            !storage_read_sector(id.sector_count-1,backup) || !storage_read_sector(0,mbr)) {
            failed=true;return -VFS_EIO;
        }
        /* Source exclusion precedes CRC admission. Even a damaged source
         * primary must remain excluded when its backup retains the GUID. */
        bool p=!memcmp(primary,"EFI PART",8),b=!memcmp(backup,"EFI PART",8);
        if ((p && !memcmp(primary+56,source_guid,16)) || (b && !memcmp(backup+56,source_guid,16))) {
            id.flags=INSTALL_ID_SOURCE;
        } else if (p) {
            uint32_t expected=le32(primary+16);memset(primary+16,0,4);
            if (le32(primary+8)!=0x10000 || le32(primary+12)!=92 || le32(primary+20) || crc32(primary,92)!=expected)
                id.flags=0;
        } else if (b) {
            id.flags=0; /* Unknown/degraded GPT is visible but not selectable. */
        } else if (mbr[510]==0x55 && mbr[511]==0xaa) {
            for (unsigned i=0;i<4;i++) if (mbr[446+i*16+4]==0xee) id.flags=0;
        } else if (!zero_sector(primary) || !zero_sector(backup) || !zero_sector(mbr)) {
            id.flags=0;
        }
    }
#endif
    install_identity_digest(&id);*out=id;return 0;
}
/* Progress bar. Percent is driven by stage plus bytes written (payload) and read
 * (verification), so it moves steadily through the long copy. Console only: the
 * per-percent redraw must not flood the serial/dmesg log. */
#define PROGRESS_PAYLOAD_BYTES ((uint64_t)(64u+256u+1u)*1024*1024)
#define PROGRESS_VERIFY_BYTES ((uint64_t)(64u+1u)*1024*1024)
static unsigned progress_stage;
static uint64_t progress_written,progress_read;
static int progress_last=-1;
static void progress_draw(unsigned stage,unsigned pct) {
    static const char *const labels[]={"Checking installer files","Checking the selected disk","Preparing the disk",
        "Creating partitions","Copying FortressOS files","Creating your account","Verifying installed files","Finishing"};
    char line[128];size_t n=0;line[n++]='\r';line[n++]='[';
    for (unsigned i=0;i<25;i++) line[n++]=i<pct*25/100 ? '#' : '-';
    line[n++]=']';line[n++]=' ';
    line[n++]=pct>=100 ? '1' : ' ';
    line[n++]=pct>=100 ? '0' : pct>=10 ? (char)('0'+pct/10) : ' ';
    line[n++]=pct>=100 ? '0' : (char)('0'+pct%10);
    line[n++]='%';line[n++]=' ';line[n++]=' ';
    const char *label=labels[stage<8 ? stage : 7];
    for (size_t i=0;label[i] && n<sizeof(line)-8;i++) line[n++]=label[i];
    line[n++]='.';line[n++]='.';line[n++]='.';
    line[n++]=27;line[n++]='[';line[n++]='K';line[n]=0;
    console_terminal_write(line,n);
}
static void progress_update(void) {
    uint64_t span;unsigned pct;
    switch (progress_stage) {
    case 0: pct=1;break;
    case 1: pct=2;break;
    case 2: pct=3;break;
    case 3: pct=5;break;
    case 4: span=progress_written>PROGRESS_PAYLOAD_BYTES ? PROGRESS_PAYLOAD_BYTES : progress_written;pct=5+(unsigned)(span*80/PROGRESS_PAYLOAD_BYTES);break;
    case 5: pct=87;break;
    case 6: span=progress_read>PROGRESS_VERIFY_BYTES ? PROGRESS_VERIFY_BYTES : progress_read;pct=88+(unsigned)(span*11/PROGRESS_VERIFY_BYTES);break;
    default: pct=100;break;
    }
    int key=(int)(progress_stage*1000+pct);
    if (key==progress_last) return;
    progress_last=key;progress_draw(progress_stage,pct);
}
static int read_run(void *context,uint64_t lba,uint32_t count,void *bytes) {
    (void)context;
    if (failed || !storage_read_sectors(lba,count,bytes)) {failed=true;return -VFS_EIO;}
    if (deploying && progress_stage==6) {progress_read+=(uint64_t)count*512;progress_update();}
    return 0;
}
static int write_run(void *context,uint64_t lba,uint32_t count,const void *bytes) {
    (void)context;
    if (failed || !storage_write_sectors(lba,count,bytes)) {failed=true;return -VFS_EIO;}
    if (deploying && progress_stage==4) {progress_written+=(uint64_t)count*512;progress_update();}
    return 0;
}
static int flush(void *context) {
    (void)context;
    if (failed || !storage_flush()) {failed=true;return -VFS_EIO;}
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
static void show_progress(void *context,unsigned stage) {
    (void)context;
    if (stage<8) {
        if (stage<=1) {progress_written=0;progress_read=0;progress_last=-1;}
        progress_stage=stage;progress_update();
    }
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
    runtime_backend=(install_backend_t){.identity=identity,.read=read_run,.write=write_run,.flush=flush,.deploy=deploy,.progress=show_progress};
    install_control_boot(&runtime_backend,true);return true;
}
#ifdef INSTALLER_PHYSICAL
static bool physical_boot_expected(const boot_info_t *info,const uint8_t expected[32]) {
    if (started || failed || physical_bound || !info || !info->installer_payload_valid ||
        !info->installer_source_valid || install_control_owned()) return false;
    physical_bound=true;
    memcpy(physical_expected,expected,32);
    /* Shared transport exclusions and fresh identity checks remain mandatory.
     * This entry is compile-gated and authorizes only the fixed fingerprint. */
    return install_nvme_boot(info,true);
}
bool install_nvme_physical_boot(const boot_info_t *info) {
    return physical_boot_expected(info,install_physical_digest);
}
#endif
#if defined(INSTALLER_MULTI) || (!defined(INSTALLER_PHYSICAL) && !defined(TEST_INSTALLER_INSPECT))
static install_identity_v1_t multi_ids[8],multi_raw[8];
static unsigned multi_count,multi_selected;
static uint8_t multi_summary[8][32];
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p)|((uint64_t)le32(p+4)<<32);}
/* Read-only look at the existing partition table so the user can be warned about data. */
static void summarize_disk(uint64_t sectors,uint8_t out[32]) {
    uint8_t sector[512],entries[512];unsigned count=0,kinds=0,scheme=0;bool unreadable=false;
    memset(out,0,32);out[0]=INSTALL_SUMMARY_VALID;
    if (!storage_read_sector(1,sector)) {failed=true;out[3]=INSTALL_SUMMARY_UNREADABLE;return;}
    if (!memcmp(sector,"EFI PART",8)) {
        scheme=INSTALL_SUMMARY_GPT;
        uint64_t at=le64(sector+72);uint32_t total=le32(sector+80),size=le32(sector+84);
        if (size!=128 || sectors<8 || at<2 || at>=sectors || at>sectors-8) unreadable=true;
        else {
            if (total>32) total=32;
            for (unsigned s=0;s<(total+3)/4 && !unreadable;s++) {
                if (!storage_read_sector(at+s,entries)) {failed=true;unreadable=true;break;}
                for (unsigned e=0;e<4 && s*4+e<total;e++) {
                    const uint8_t *g=entries+e*128;
                    bool unused=true;
                    for (unsigned k=0;k<16;k++) if (g[k]) unused=false;
                    if (unused) continue;
                    count++;
                    uint32_t t0=le32(g);
                    if (t0==0xC12A7328u) kinds|=INSTALL_SUMMARY_EFI;
                    else if (t0==0xEBD0A0A2u || t0==0xE3C9E316u || t0==0xDE94BBA4u) kinds|=INSTALL_SUMMARY_WINDOWS;
                    else if (t0==0x0FC63DAFu || t0==0xE6D6D379u || t0==0x0657FD6Du) kinds|=INSTALL_SUMMARY_LINUX;
                    else kinds|=INSTALL_SUMMARY_OTHER;
                }
            }
        }
    } else if (!storage_read_sector(0,sector)) {failed=true;unreadable=true;
    } else if (sector[510]==0x55 && sector[511]==0xAA) {
        scheme=INSTALL_SUMMARY_MBR;
        for (unsigned i=0;i<4;i++) {
            uint8_t type=sector[446+i*16+4];
            if (!type) continue;
            count++;
            if (type==0x07 || type==0x0B || type==0x0C || type==0x27) kinds|=INSTALL_SUMMARY_WINDOWS;
            else if (type==0x83 || type==0x82 || type==0x8E) kinds|=INSTALL_SUMMARY_LINUX;
            else if (type==0xEF) kinds|=INSTALL_SUMMARY_EFI;
            else kinds|=INSTALL_SUMMARY_OTHER;
        }
    }
    out[1]=(uint8_t)scheme;out[2]=(uint8_t)(count>255 ? 255 : count);out[3]=(uint8_t)(kinds|(unreadable ? INSTALL_SUMMARY_UNREADABLE : 0));
}
static int multi_enumerate(void *context,unsigned index,install_identity_v1_t *out) {
    (void)context;
    if (failed || !storage_inventory_valid()) {failed=true;return -VFS_EIO;}
    if (index>=multi_count) return 0;
    if (!storage_inventory_select(index)) {failed=true;return -VFS_EIO;}
    install_identity_v1_t now;
    if (!storage_installer_identity(&now) || memcmp(&now,&multi_raw[index],sizeof(now))) {
        failed=true;return -VFS_EIO;
    }
    *out=multi_ids[index];memcpy(out->reserved2,multi_summary[index],32);return 1;
}
static int multi_select(void *context,unsigned index) {
    install_identity_v1_t id;
    int r=multi_enumerate(context,index,&id);
    if (r!=1) return r<0 ? r : -VFS_EINVAL;
    if (id.flags!=INSTALL_ID_ELIGIBLE) return -VFS_EACCES;
    multi_selected=index;baseline=multi_ids[index];return 0;
}
static int multi_identity(void *context,install_identity_v1_t *out) {
    if (!storage_inventory_select(multi_selected)) {failed=true;return -VFS_EIO;}
    return identity(context,out);
}
bool install_nvme_multi_boot(const boot_info_t *info) {
    if (started || failed || !info || !info->installer_payload_valid ||
        !info->installer_source_valid || block_get_dev_count() || ext4_storage_owned() || install_control_owned()) return false;
    root_boot_config_t c=root_boot_parse(info->cmdline,sizeof(info->cmdline),info->cmdline_truncated);
    if (!c.valid || !c.explicit_mode || c.mode!=ROOT_BOOT_INSTALLER) return false;
    optical=info->installer_source_optical;memcpy(source_guid,info->installer_source_disk_guid,16);
    multi_count=storage_inventory_boot();
    if (!multi_count || multi_count>8) return false;
    for (unsigned i=0;i<multi_count;i++) {
        if (!storage_inventory_select(i) || !storage_installer_identity(&multi_raw[i]) || identity(NULL,&multi_ids[i])) return false;
        summarize_disk(multi_ids[i].sector_count,multi_summary[i]);
        if (failed) return false;
    }
    unsigned default_sel=0;
    for (unsigned i=0;i<multi_count;i++) if (multi_ids[i].flags==INSTALL_ID_ELIGIBLE) {default_sel=i;break;}
    multi_selected=default_sel;baseline=multi_ids[default_sel];started=true;source_boot=info;
    runtime_backend=(install_backend_t){.identity=multi_identity,.read=read_run,.write=write_run,.flush=flush,
        .deploy=deploy,.progress=show_progress,.enumerate=multi_enumerate,.select=multi_select};
    install_control_boot(&runtime_backend,true);return true;
}
#endif
#ifdef TEST_INSTALLER_INSPECT
static int inspect_identity(void *context,install_identity_v1_t *out) {
    (void)context;
    if (block_get_dev_count() || ext4_storage_owned() || storage_is_fatal() ||
        storage_is_dma_quarantined() || !storage_installer_identity(out)) return -VFS_EIO;
    out->flags=0; /* Inspection is deliberately ineligible for a raw lease. */
    install_identity_digest(out);return 0;
}
bool install_nvme_inspect_boot(const boot_info_t *info) {
    if (!info || started || failed || install_control_owned()) return false;
    root_boot_config_t c=root_boot_parse(info->cmdline,sizeof(info->cmdline),info->cmdline_truncated);
    if (!c.valid || !c.explicit_mode || c.mode!=ROOT_BOOT_INSTALLER) return false;
    install_identity_v1_t out;
    if (inspect_identity(NULL,&out)) return false;
    const install_backend_t inspection={.identity=inspect_identity};
    install_control_boot(&inspection,true);return true;
}
#endif
