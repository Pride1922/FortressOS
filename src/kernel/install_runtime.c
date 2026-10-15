#include "install_runtime.h"
#include "install_log.h"
#include "install_payload.h"
#include "install_archive.h"
#include "install_fat.h"
#include "root_boot.h"
#include "accounts.h"
#include "heap.h"
#include "string.h"
#include "../../user/tools/userdb.h"
#include "../../user/tools/digest.h"

typedef struct {
    const boot_info_t *boot;
    const install_backend_t *backend;
    install_identity_v1_t identity;
    uint8_t *esp,*archive;
    size_t archive_size;
    install_plan_t plan;
    install_expand_work_t expand;
    install_payload_work_t copy;
    install_fat_work_t fat;
    uint8_t manifest[256];
    char recovery[512],root_record[512],config[4096],shadow[256],passwd[256],group[256],username[32],hostname[65];
    size_t recovery_size,root_size;
    root_recovery_t binding;
    block_dev_t partition;
    uint32_t log_warnings;
    char log_copy[INSTALL_LOG_BYTES];
} runtime_t;

static bool add(char *out,size_t cap,size_t *at,const char *s) {
    size_t n=strlen(s);if (*at>=cap || n>=cap-*at) return false;
    memcpy(out+*at,s,n);*at+=n;out[*at]=0;return true;
}
static void hex(const uint8_t *p,size_t n,char *out) {
    const char *a="0123456789abcdef";
    for (size_t i=0;i<n;i++) {out[i*2]=a[p[i]>>4];out[i*2+1]=a[p[i]&15];}out[n*2]=0;
}
static void hash(const void *p,size_t n,uint8_t out[32]) {
    digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,out);
}
static bool identity_lines(char *out,size_t cap,size_t *n,const root_recovery_t *r) {
    return add(out,cap,n,"install_id=") && add(out,cap,n,r->install_id) &&
        add(out,cap,n,"\nroot_partuuid=") && add(out,cap,n,r->partuuid) &&
        add(out,cap,n,"\nfilesystem_uuid=") && add(out,cap,n,r->filesystem_uuid) && add(out,cap,n,"\n");
}
static bool guid_valid(const uint8_t *p) {return (p[7]&0xf0)==0x40 && (p[8]&0xc0)==0x80;}
static int source_read(void *ctx,install_role_t role,uint64_t at,void *p,size_t n) {
    runtime_t *w=ctx;
    if ((unsigned)role>=INSTALL_ROLES || at>w->boot->installer_payload_size[role] ||
        n>w->boot->installer_payload_size[role]-at) return -VFS_EINVAL;
    const uint8_t *base=role==INSTALL_ESP && w->esp ? w->esp : (const void *)w->boot->installer_payload_vaddr[role];
    memcpy(p,base+at,n);return 0;
}
static int revalidate(runtime_t *w) {
    install_identity_v1_t now;int r=w->backend->identity(w->backend->context,&now);
    return r ? r<0 ? r : -VFS_EIO : memcmp(&now,&w->identity,sizeof(now)) ? -VFS_EIO : 0;
}
static int admit(void *ctx) {return revalidate(ctx);}
static void progress(void *ctx,install_stage_t stage) {
    install_log_stage((unsigned)stage);
    runtime_t *w=ctx;if (w->backend->progress) w->backend->progress(w->backend->context,(unsigned)stage);
}
static int raw_read(void *ctx,uint64_t l,uint32_t n,void *p) {
    runtime_t *w=ctx;int r=revalidate(w);return r ? r : w->backend->read(w->backend->context,l,n,p);
}
static int raw_write(void *ctx,uint64_t l,uint32_t n,const void *p) {
    runtime_t *w=ctx;int r=revalidate(w);return r ? r : w->backend->write(w->backend->context,l,n,p);
}
static int raw_flush(void *ctx) {
    runtime_t *w=ctx;int r=revalidate(w);return r ? r : w->backend->flush(w->backend->context);
}
/* Partition adapter is private and never registered in block/devfs/GPT. The
 * outer control gate stays busy for the entire detached filesystem lifetime. */
static bool part_read(block_dev_t *d,uint64_t l,void *p) {
    return l<d->sector_count && !raw_read(d->priv,INSTALL_ROOT_LBA+l,1,p);
}
static bool part_write(block_dev_t *d,uint64_t l,const void *p) {
    return l<d->sector_count && !raw_write(d->priv,INSTALL_ROOT_LBA+l,1,p);
}
static bool part_flush(block_dev_t *d) {return !raw_flush(d->priv);}

static int root_file_at(ext4_mount_t *fs,const char *directory,const char *name,const char *bytes,size_t size,bool existing,uint32_t mode) {
    creds_t actor;creds_init_root(&actor);int r;
    vfs_node_t *parent=vfs_lookup_at_creds(ext4_prepared_root(fs),directory,&actor,&r);
    if (!parent) return r;
    vfs_create_attrs_t attrs={.mode=mode};
    vfs_node_t *node=existing ? vfs_lookup_at_creds(parent,name,&actor,&r) : parent->create_attrs_ref ? parent->create_attrs_ref(parent,name,VFS_FILE,&attrs,&r) : NULL;
    vfs_node_put(parent);if (!node) return r ? r : -VFS_EOPNOTSUPP;
    uint64_t offset=0;int64_t n=node->write ? node->write(node,&offset,false,bytes,size) : -VFS_EOPNOTSUPP;
    r=n<0 ? (int)n : (size_t)n!=size ? -VFS_EIO : 0;
    /* Read authoritative bytes and metadata before releasing the reference. */
    vfs_metadata_t meta;char verify[512];
    if (!r) r=vfs_metadata(node,&meta);
    if (!r && (meta.size!=size || meta.uid || meta.gid || (meta.mode&07777)!=mode)) r=-VFS_EINVAL;
    for (size_t at=0;!r && at<size;at+=sizeof(verify)) {
        size_t n=size-at;if (n>sizeof(verify)) n=sizeof(verify);
        if (!node->read || node->read(node,at,verify,n)!=(int64_t)n || memcmp(verify,bytes+at,n)) r=-VFS_EIO;
    }
    vfs_node_put(node);return r;
}
static int root_file(ext4_mount_t *fs,const char *name,const char *bytes,size_t size) {
    return root_file_at(fs,"/etc",name,bytes,size,false,0644);
}
static int write_install_log(runtime_t *w,ext4_mount_t *fs,bool existing) {
    size_t n;const char *bytes=install_log_snapshot(&n);
    if (n>sizeof(w->log_copy)) return -VFS_EINVAL;
    memset(w->log_copy,' ',sizeof(w->log_copy));memcpy(w->log_copy,bytes,n);
    w->log_copy[sizeof(w->log_copy)-1]='\n';
    return root_file_at(fs,"/var/log","fortress-install.log",w->log_copy,sizeof(w->log_copy),existing,0600);
}
/* Optional logging errors are warnings, but filesystem freeze/discard must still
 * succeed: ignoring a tainted write would falsely report a healthy installation. */
static int finish_install_log(runtime_t *w) {
    ext4_mount_t *fs=NULL;int r=ext4_prepare_installer_root(&w->partition,true,&fs,NULL);
    if (r) {w->log_warnings|=INSTALL_STATUS_ROOT_LOG_WARNING;return r==-VFS_ENOMEM ? 0 : r;}
    if (write_install_log(w,fs,true)) w->log_warnings|=INSTALL_STATUS_ROOT_LOG_WARNING;
    r=ext4_freeze_and_sync(fs);int discarded=ext4_discard_prepared_root(fs);return r ? r : discarded;
}
static int provision(void *ctx) {
    runtime_t *w=ctx;ext4_mount_t *fs=NULL;
    if (w->plan.root_sectors) {
        install_target_t target={.context=w,.sectors=w->identity.sector_count,.sector_size=512,.read=raw_read,.write=raw_write,.flush=raw_flush};
        int expanded=install_root_expand(&target,w->plan.root_sectors,&w->expand);if (expanded) return expanded;
    }
    w->partition=(block_dev_t){.sector_size=512,.sector_count=w->plan.root_sectors ? w->plan.root_sectors : INSTALL_ROOT_BYTES/512,
        .read_sector=part_read,.write_sector=part_write,.flush=part_flush,.priv=w};
    int r=ext4_prepare_installer_root(&w->partition,true,&fs,NULL);
    if (r) return r;
    if (strcmp(w->username,"operator")) {
        creds_t actor;creds_init_root(&actor);
        vfs_node_t *parent=vfs_lookup_at_creds(ext4_prepared_root(fs),"/home",&actor,&r);
        if (parent) {
            vfs_create_attrs_t attrs={.mode=0700,.uid=1000,.gid=1000};
            vfs_node_t *home=parent->create_attrs_ref ? parent->create_attrs_ref(parent,w->username,VFS_DIRECTORY,&attrs,&r) : NULL;
            if (!home && !r) r=-VFS_EOPNOTSUPP;
            if (home) {
                vfs_metadata_t meta;r=vfs_metadata(home,&meta);
                if (!r && (meta.type!=VFS_DIRECTORY || meta.uid!=1000 || meta.gid!=1000 || (meta.mode&07777)!=0700)) r=-VFS_EINVAL;
                vfs_node_put(home);
            }
            vfs_node_put(parent);
        }
    }
    const char *bytes[]={w->passwd,w->group,w->shadow};size_t sizes[]={strlen(w->passwd),strlen(w->group),strlen(w->shadow)};
    if (!r) r=accounts_provision_detached(fs,bytes,sizes);
    if (!r) r=root_file(fs,"fortress-install.v1",w->root_record,w->root_size);
    if (!r) r=root_file(fs,"hostname",w->hostname,strlen(w->hostname));
    if (!r && write_install_log(w,fs,false)) w->log_warnings|=INSTALL_STATUS_ROOT_LOG_WARNING;
    int frozen=ext4_freeze_and_sync(fs);if (!r) r=frozen;
    int discarded=ext4_discard_prepared_root(fs);if (!r) r=discarded;
    return r;
}
static int personalize(runtime_t *w,const install_deploy_v1_t *req) {
    const char *username=req->version>=2 ? ((const install_deploy_v2_t *)req)->username : "operator";
    if (!install_username_valid(username)) return -VFS_EINVAL;
    memcpy(w->username,username,strlen(username)+1);
    const char *hostname=req->version==3 ? ((const install_deploy_v3_t *)req)->hostname : "fortress";
    if (!install_hostname_valid(hostname)) return -VFS_EINVAL;
    size_t hn=strlen(hostname);memcpy(w->hostname,hostname,hn);w->hostname[hn]='\n';w->hostname[hn+1]=0;
    if (req->version==3) {
        if (w->identity.sector_count<INSTALL_MIN_SECTORS) return -VFS_EINVAL;
        w->plan.root_sectors=w->identity.sector_count-INSTALL_ROOT_LBA-33;
        if (w->plan.root_sectors/8>INSTALL_ROOT_MAX_BLOCKS) return -VFS_EFBIG;
    }
    size_t at=0;
    if (!add(w->passwd,sizeof(w->passwd),&at,"root:x:0:0::/root:/bin/shell\n") ||
        !add(w->passwd,sizeof(w->passwd),&at,username) || !add(w->passwd,sizeof(w->passwd),&at,":x:1000:1000::/home/") ||
        !add(w->passwd,sizeof(w->passwd),&at,username) || !add(w->passwd,sizeof(w->passwd),&at,":/bin/shell\n")) return -VFS_EINVAL;
    at=0;
    if (!add(w->group,sizeof(w->group),&at,"root:x:0:\n") || !add(w->group,sizeof(w->group),&at,username) ||
        !add(w->group,sizeof(w->group),&at,":x:1000:\nwheel:x:10:") || !add(w->group,sizeof(w->group),&at,username) ||
        !add(w->group,sizeof(w->group),&at,"\n")) return -VFS_EINVAL;
    for (unsigned i=0;i<4;i++) {
        const uint8_t *g=i ? req->partition_guid[i-1] : req->disk_guid;
        if (!guid_valid(g) || (!w->boot->installer_source_optical && !memcmp(g,w->boot->installer_source_disk_guid,16)) ||
            !memcmp(g,w->identity.nguid,16) || !memcmp(g,req->install_id,16)) return -VFS_EINVAL;
        for (unsigned j=0;j<i;j++) if (!memcmp(g,j ? req->partition_guid[j-1] : req->disk_guid,16)) return -VFS_EINVAL;
    }
    bool nonzero=false;for (unsigned i=0;i<16;i++) nonzero|=req->install_id[i]!=0;
    if (!nonzero || req->operator_hash[77] || req->recovery_hash[77] ||
        strncmp(req->operator_hash,"$5$rounds=100000$",17) || strlen(req->operator_hash)!=77 ||
        strncmp(req->recovery_hash,"$5$rounds=100000$",17) || strlen(req->recovery_hash)!=77 ||
        !db_hash_valid(req->operator_hash) || !db_hash_valid(req->recovery_hash) ||
        !memcmp(req->operator_hash+17,req->recovery_hash+17,16)) return -VFS_EINVAL;
    root_recovery_t *b=&w->binding;hex(req->install_id,16,b->install_id);
    gpt_guid_t guid;memcpy(guid.bytes,req->partition_guid[1],16);gpt_guid_to_str(&guid,b->partuuid);
    for (unsigned i=0;i<36;i++) if (b->partuuid[i]>='A' && b->partuuid[i]<='F') b->partuuid[i]+='a'-'A';
    memcpy(b->filesystem_uuid,"813ea876-9933-4e1c-ae62-2f6c0f90a441",37);
    memcpy(b->hash,req->recovery_hash,78);
    size_t n=0;
    if (!add(w->recovery,512,&n,"FORTRESS-RECOVERY-1\n") || !identity_lines(w->recovery,512,&n,b) ||
        !add(w->recovery,512,&n,"password_hash=") || !add(w->recovery,512,&n,b->hash) || !add(w->recovery,512,&n,"\n")) return -VFS_EINVAL;
    w->recovery_size=n;root_recovery_t check;
    if (!root_recovery_parse(w->recovery,n,&check)) return -VFS_EINVAL;
    n=0;if (!add(w->root_record,512,&n,"FORTRESS-INSTALL-1\n") || !identity_lines(w->root_record,512,&n,b) ||
        !add(w->root_record,512,&n,"payload_version=1\n")) return -VFS_EINVAL;
    w->root_size=n;if (!root_manifest_validate(w->root_record,n,b)) return -VFS_EINVAL;
    n=0;if (!add(w->shadow,sizeof(w->shadow),&n,"root:!:::::::\n") || !add(w->shadow,sizeof(w->shadow),&n,username) || !add(w->shadow,sizeof(w->shadow),&n,":") ||
        !add(w->shadow,sizeof(w->shadow),&n,req->operator_hash) || !add(w->shadow,sizeof(w->shadow),&n,":::::::\n")) return -VFS_EINVAL;
    int r=install_archive_personalize((void *)w->boot->initramfs_vaddr,w->boot->initramfs_size,
        w->recovery,w->recovery_size,w->archive,INSTALL_ARCHIVE_MAX,&w->archive_size);
    if (r) return r;
    n=0;
    const char *modes[]={"installed","recovery"};
    if (!add(w->config,sizeof(w->config),&n,"timeout: 5\n")) return -VFS_EINVAL;
    for (unsigned i=0;i<2;i++) {
        if (!add(w->config,sizeof(w->config),&n,i ? "/FortressOS authenticated recovery\n" : "/FortressOS installed\n") ||
            !add(w->config,sizeof(w->config),&n,"    protocol: limine\n    kernel_path: boot():/boot/fortress.elf\n    kernel_cmdline: boot_mode=") ||
            !add(w->config,sizeof(w->config),&n,modes[i]) || !add(w->config,sizeof(w->config),&n," root=PARTUUID=") ||
            !add(w->config,sizeof(w->config),&n,b->partuuid) ||
            !add(w->config,sizeof(w->config),&n,"\n    module_path: boot():/boot/bootstrap.tar\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/readiness.v1\n    module_cmdline: role=readiness\n")) return -VFS_EINVAL;
    }
    install_fat_patch_t patch={.bootstrap=w->archive,.bootstrap_size=w->archive_size,.config=w->config,.config_size=n};
    r=install_fat_personalize(w->esp,INSTALL_ESP_BYTES,&patch,&w->fat,&w->plan.readiness_lba);if (r) return r;
    memcpy(w->plan.disk_guid,req->disk_guid,16);memcpy(w->plan.partition_guid,req->partition_guid,48);
    uint8_t raw[32];char text[65];n=0;char *ready=(void *)w->plan.readiness;
    if (!add(ready,512,&n,"FORTRESS-READY-1\n") || !identity_lines(ready,512,&n,b) || !add(ready,512,&n,"payload_version=1\nkernel_sha256=")) return -VFS_EINVAL;
    hash((void *)w->boot->kernel_file_vaddr,w->boot->kernel_file_size,raw);hex(raw,32,text);
    if (!add(ready,512,&n,text) || !add(ready,512,&n,"\nbootstrap_sha256=")) return -VFS_EINVAL;
    hash(w->archive,w->archive_size,raw);hex(raw,32,text);
    if (!add(ready,512,&n,text) || !add(ready,512,&n,"\n")) return -VFS_EINVAL;
    hash(ready,n,raw);hex(raw,32,text);
    if (!add(ready,512,&n,"checksum=") || !add(ready,512,&n,text) || !add(ready,512,&n,"\n") ||
        !root_readiness_validate(ready,512,b,(void *)w->boot->kernel_file_vaddr,w->boot->kernel_file_size,w->archive,w->archive_size)) return -VFS_EINVAL;
    hash(w->esp,INSTALL_ESP_BYTES,w->manifest+48);hash(w->manifest,224,w->manifest+224);return 0;
}
int install_runtime_deploy(const boot_info_t *boot,const install_backend_t *backend,
    const install_identity_v1_t *id,const install_deploy_v1_t *req,install_status_v1_t *status) {
    if (!boot || !boot->installer_payload_valid || !backend || !id || !req || !status) return -VFS_EINVAL;
    runtime_t *w=kcalloc(1,sizeof(*w));if (!w) return -VFS_ENOMEM;
    w->boot=boot;w->backend=backend;w->identity=*id;
    install_source_t source={.context=w,.read=source_read};
    int r=install_payload_validate((void *)boot->installer_payload_vaddr[4],256,&source,w->copy.data);
    if (!r) r=install_fat_kernel_matches((void *)boot->installer_payload_vaddr[0],INSTALL_ESP_BYTES,
        (void *)boot->kernel_file_vaddr,boot->kernel_file_size,&w->fat);
    if (!r && req->flags==INSTALL_DRY_RUN) goto done;
    if (r) goto done;
    w->esp=kmalloc_large(INSTALL_ESP_BYTES);w->archive=kmalloc_large(INSTALL_ARCHIVE_MAX);
    if (!w->esp || !w->archive) {r=-VFS_ENOMEM;goto done;}
    memcpy(w->esp,(void *)boot->installer_payload_vaddr[0],INSTALL_ESP_BYTES);
    memcpy(w->manifest,(void *)boot->installer_payload_vaddr[4],256);
    r=personalize(w,req);if (r) goto done;
    install_log_target(id,w->username);
    install_target_t target={.context=w,.sectors=id->sector_count,.sector_size=id->sector_size,
        .admit=admit,.read=raw_read,.write=raw_write,.flush=raw_flush,.provision=provision,.progress=progress};
    install_result_t result;
    r=install_payload_deploy(w->manifest,256,&source,&target,&w->plan,false,&w->copy,&result);
    status->stage=result.stage;status->copied_bytes=result.copied_bytes;status->complete=result.complete;
    install_log_result(result.stage,r,result.complete);
    if (!r && result.complete) {r=finish_install_log(w);if (r) {status->complete=0;install_log_result(result.stage,r,false);}}
done:
    status->error=r;status->reserved=w->log_warnings | (install_log_usb_ok() ? 0 : INSTALL_STATUS_USB_LOG_WARNING);
    if (w->archive) {db_wipe(w->archive,INSTALL_ARCHIVE_MAX);kfree(w->archive);}
    if (w->esp) {db_wipe(w->esp,INSTALL_ESP_BYTES);kfree(w->esp);}
    /* A failed discard must never leave a mounted device pointing at freed
     * callback context. The outer adapter latches failure; retain this owner
     * for boot lifetime until a separately supported teardown exists. */
    if (ext4_storage_owned()) {
        w->archive=NULL;w->esp=NULL;
        db_wipe(w->shadow,sizeof(w->shadow));db_wipe(w->recovery,sizeof(w->recovery));
        db_wipe(w->binding.hash,sizeof(w->binding.hash));
        status->complete=0;status->error=r ? r : -VFS_EIO;
        return status->error;
    }
    db_wipe(w,sizeof(*w));kfree(w);return r;
}
