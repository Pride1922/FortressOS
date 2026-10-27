#include "installed_root.h"
#include "ext4.h"
#include "storage.h"
#include "pci.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "elf.h"
#include "accounts.h"
#include "vmm.h"
#include "../../user/tools/userdb.h"

static root_recovery_t recovery;
static bool recovery_valid;
static bool published;
static const char *reason="root admission";
static userdb_t database;
static char file_bytes[DB_FILE_MAX+1];
static bool inventory_read(block_dev_t *disk,uint64_t lba,void *bytes) {
    (void)disk;return storage_read_sector(lba,bytes);
}
/* Scan read-only, reject duplicate identifiers across drives, then pin the
 * matching controller before publishing any writable block/filesystem state. */
static bool select_root_controller(const gpt_guid_t *uuid) {
    size_t count=storage_inventory_boot();unsigned matches=0,chosen=0;bool coherent=false;
    if (!count) return false;
    for (unsigned i=0;i<count;i++) {
        if (!storage_inventory_select(i) || !storage_has_unique_namespace() || !storage_root_geometry_supported()) return false;
        block_dev_t disk={.sector_size=storage_get_sector_size(),.sector_count=storage_get_sector_count(),.read_sector=inventory_read};
        memcpy(disk.name,"root-probe",11);gpt_policy_result_t policy;
        bool parsed=gpt_parse_ex(&disk,&policy);
        if (parsed) for (size_t j=0;j<gpt_get_partition_count();j++) {
            gpt_partition_t *p=gpt_get_partition(j);
            if (gpt_guid_equal(&p->unique_guid,uuid)) {
                matches++;chosen=i;coherent=policy==GPT_POLICY_PRIMARY_CONSISTENT && gpt_guid_equal(&p->type_guid,&GPT_GUID_LINUX_FS);
            }
        }
        gpt_reset();
    }
    return matches==1 && coherent && storage_inventory_select(chosen);
}
#ifdef TEST_ROOT_PHASE2
static bool fail_flush,fail_replay_write;
void installed_root_test_faults(bool f,bool w) {fail_flush=f;fail_replay_write=w;}
static bool rejected_flush(block_dev_t *d) {(void)d;return false;}
static bool rejected_write(block_dev_t *d,uint64_t l,const void *p) {(void)d;(void)l;(void)p;return false;}
#endif

bool installed_root_requested(const boot_info_t *info) {
    root_boot_config_t c=root_boot_parse(info->cmdline,sizeof(info->cmdline),info->cmdline_truncated);
    return !c.valid || (c.mode!=ROOT_BOOT_LIVE && c.mode!=ROOT_BOOT_INSTALLER) || c.has_root;
}
static bool read_node(vfs_node_t *node,void *bytes,size_t capacity,size_t *length) {
    vfs_metadata_t m;
    if (!node || vfs_metadata(node,&m) || m.type!=VFS_FILE || m.size>capacity) return false;
    if (!node->read) {
        if (!(m.mnt_flags & VFS_MNT_RDONLY) || (!node->data && m.size)) return false;
        memcpy(bytes,node->data,m.size);*length=m.size;return true;
    }
    size_t at=0;
    while (at<m.size) {
        int64_t n=node->read(node,at,(uint8_t *)bytes+at,m.size-at);
        if (n<=0 || (uint64_t)n>m.size-at) return false;
        at+=(size_t)n;
    }
    *length=at;return true;
}
static bool read_at(vfs_node_t *root,const char *path,void *bytes,size_t capacity,size_t *length) {
    creds_t actor;creds_init_root(&actor);int error;
    vfs_node_t *node=root ? vfs_lookup_at_creds(root,path,&actor,&error) : vfs_bootstrap_lookup_creds(path,&actor,&error);
    bool ok=read_node(node,bytes,capacity,length);vfs_node_put(node);return ok;
}
static bool executable(vfs_node_t *root,const char *path,bool setid) {
    creds_t actor;creds_init_root(&actor);int error;bool ok=false;
    vfs_node_t *node=vfs_lookup_at_creds(root,path,&actor,&error);void *bytes=NULL;vfs_metadata_t m;
    if (node) {
        /* Detached validation owns the node, and the image callback performs the
         * complete authoritative copy under filesystem exclusion. */
        file_t f={.node=node,.flags=VFS_O_RDONLY,.ref_count=1};
        if (!vfs_exec_image(&f,&actor,&m,&bytes) && !m.uid && !m.gid &&
            (m.mode & 07777)==(setid ? 04755u : 0755u)) {
            elf_loaded_process_t loaded;
            if (elf_load_executable(bytes,m.size,&loaded)==ELF_OK)
                ok=vmm_destroy_pml4(loaded.pml4_phys,true)==VMM_OK;
        }
    }
    kfree(bytes);vfs_node_put(node);return ok;
}
static bool validate_tree(ext4_mount_t *mount) {
    vfs_node_t *root=ext4_prepared_root(mount);creds_t actor;creds_init_root(&actor);int error;
    const char *dirs[]={"/","/bin","/etc","/home","/var","/root","/mnt","/dev","/run","/tmp"};
    for (unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
        vfs_node_t *n=vfs_lookup_at_creds(root,dirs[i],&actor,&error);vfs_metadata_t m;
        bool ok=n && !vfs_metadata(n,&m) && m.type==VFS_DIRECTORY && !m.uid && !m.gid &&
            /* Keep earlier installed roots bootable; new installer roots make
             * /var private. Both forms remain root-owned and non-user-writable. */
            (i==4 ? ((m.mode & 07777)==0700u || (m.mode & 07777)==0755u) :
             (m.mode & 07777)==(i==5 ? 0700u : i==9 ? 01777u : 0755u));
        vfs_node_put(n);if (!ok) return false;
    }
    size_t len;
    if (!read_at(root,"/etc/fortress-install.v1",file_bytes,512,&len) || !root_manifest_validate(file_bytes,len,&recovery)) return false;
    /* Resolve redo against the detached root before reading canonical names.
     * No installed authentication or process publication has occurred. */
    if (accounts_prepare(mount)) return false;
    memset(&database,0,sizeof(database));
    const char *names[]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};
    vfs_node_t *nodes[4]={0};bool ok=true;
    for (unsigned i=0;i<4 && ok;i++) {
        nodes[i]=vfs_lookup_at_creds(root,names[i],&actor,&error);
        vfs_metadata_t m;
        ok=nodes[i] && !vfs_metadata(nodes[i],&m) && !m.uid && !m.gid;
        if (!i || !ok) continue;
        ok=m.type==VFS_FILE && (m.mode & 07777)==(i==3 ? 0600u : 0644u) &&
            read_node(nodes[i],file_bytes,DB_FILE_MAX,&len);
        if (ok) ok=i==1 ? db_passwd(&database,file_bytes,len) : i==2 ? db_group(&database,file_bytes,len) : db_shadow(&database,file_bytes,len);
        db_wipe(file_bytes,sizeof(file_bytes));
    }
    if (ok) ok=db_validate(&database);
    const db_shadow_t *admin=ok ? db_shadow_name(&database,"root") : NULL;
    if (!admin || (admin->hash[0]!='!' && admin->hash[0]!='*')) ok=false;
    for (unsigned i=0;i<database.nusers && ok;i++) {
        const db_user_t *u=&database.users[i];const db_shadow_t *s=db_shadow_name(&database,u->name);
        if (!s || (!s->restricted && !*s->hash)) {ok=false;break;}
        if (!u->uid) continue;
        char home[128]="/home/";size_t name=strlen(u->name);
        memcpy(home+6,u->name,name+1);
        if (strcmp(home,u->home) || strcmp(u->shell,"/bin/shell")) {ok=false;break;}
        vfs_node_t *n=vfs_lookup_at_creds(root,home,&actor,&error);vfs_metadata_t m;
        ok=n && !vfs_metadata(n,&m) && m.type==VFS_DIRECTORY && m.uid==u->uid && m.gid==u->gid && (m.mode & 07777)==0700;
        vfs_node_put(n);
    }
    if (ok) ok=!ext4_root_lock_accounts(mount,nodes);
    for (unsigned i=0;i<4;i++) vfs_node_put(nodes[i]);
    db_wipe(&database,sizeof(database));
    return ok && executable(root,"/bin/login",false) && executable(root,"/bin/shell",false) && executable(root,"/bin/sudo",true);
}
bool installed_root_boot(const boot_info_t *info) {
    root_boot_config_t c=root_boot_parse(info->cmdline,sizeof(info->cmdline),info->cmdline_truncated);
    size_t n;char record[512];
    creds_t actor;creds_init_root(&actor);int error;
    vfs_node_t *credential=vfs_bootstrap_lookup_creds("/bootstrap/recovery.v1",&actor,&error);
    vfs_metadata_t meta;
    recovery_valid=credential && !vfs_metadata(credential,&meta) && !meta.uid && !meta.gid &&
        meta.mode==(VFS_S_IFREG|0600) && (meta.mnt_flags & VFS_MNT_RDONLY) &&
        read_node(credential,record,sizeof(record),&n) && root_recovery_parse(record,n,&recovery);
    if (recovery_valid) recovery_valid=credential->parent && !vfs_metadata(credential->parent,&meta) &&
        meta.mode==(VFS_S_IFDIR|0700) && !meta.uid && !meta.gid && (meta.mnt_flags & VFS_MNT_RDONLY);
    vfs_node_put(credential);
    db_wipe(record,sizeof(record));
    reason="invalid boot selection";
    if (!c.valid) return false;
    reason="invalid independent recovery credential";
    if (!recovery_valid) return false;
    gpt_guid_t expected;
    if (!gpt_str_to_guid(recovery.partuuid,&expected) || !gpt_guid_equal(&expected,&c.partuuid)) {recovery_valid=false;return false;}
    if (c.mode==ROOT_BOOT_RECOVERY) {reason="explicit recovery mode (no root writes)";return false;}
    reason="invalid or incomplete boot readiness";
    if (!info->installed_modules_valid || !root_readiness_validate((void *)info->readiness_vaddr,info->readiness_size,&recovery,
        (void *)info->kernel_file_vaddr,info->kernel_file_size,(void *)info->initramfs_vaddr,info->initramfs_size)) return false;
    reason="ambiguous storage inventory";
    if (!select_root_controller(&c.partuuid)) return false;
    reason="unsupported storage device/geometry";
    if (!storage_init() || !storage_has_unique_namespace() || !storage_root_geometry_supported() || storage_get_sector_size()!=512) return false;
    block_init();if (!storage_register_selected()) return false;
    block_dev_t *disk=storage_selected_disk();gpt_policy_result_t policy;
    reason="invalid, degraded or conflicting GPT";
    if (!gpt_parse_ex(disk,&policy) || policy!=GPT_POLICY_PRIMARY_CONSISTENT) return false;
    block_dev_t *selected=NULL;unsigned matches=0;
    for (size_t i=0;i<gpt_get_partition_count();i++) {
        gpt_partition_t *p=gpt_get_partition(i);
        if (gpt_guid_equal(&p->unique_guid,&c.partuuid)) {
            matches++;if (gpt_guid_equal(&p->type_guid,&GPT_GUID_LINUX_FS)) selected=&p->block_dev;
        }
    }
    reason="missing or duplicate root PARTUUID";
    if (matches!=1 || !selected) return false;
    reason="unsupported installed root capacity";
    /* Fixed legacy roots and new whole-disk installations share the same
     * PARTUUID/readiness/filesystem admission. Keep explicit geometry bounds. */
    if (selected->sector_count<524288 || selected->sector_count>2147483655ULL) return false;
    reason="filesystem identity mismatch";uint8_t sb[512];gpt_guid_t uuid;
    if (!gpt_str_to_guid(recovery.filesystem_uuid,&uuid) || !block_read_sector(selected,2,sb)) return false;
    uint8_t canonical[16];memcpy(canonical,uuid.bytes,16);
    for (unsigned i=0;i<4;i++) canonical[i]=uuid.bytes[3-i];
    canonical[4]=uuid.bytes[5];canonical[5]=uuid.bytes[4];canonical[6]=uuid.bytes[7];canonical[7]=uuid.bytes[6];
    if (memcmp(sb+104,canonical,16)) return false;
#ifdef TEST_ROOT_PHASE2
    if (fail_flush) selected->flush=rejected_flush;
    if (fail_replay_write) {selected->write_sector=rejected_write;selected->write_sectors=NULL;}
#endif
    reason="Storage flush preflight failed";if (!block_flush(selected)) return false;
    reason="filesystem admission/replay failed";ext4_mount_t *mount=NULL;const char *stage="root preparation";
    if (ext4_prepare_root_diagnose(selected,true,&mount,&stage)) {
        const char *after[]={"journal-replay","reopened-state","map-allocation","journal-activation","orphan-recovery","final-barrier","root-inode"};
        bool may_write=false;for (unsigned i=0;i<sizeof(after)/sizeof(after[0]);i++) may_write|=!strcmp(stage,after[i]);
        serial_puts(may_write ? "[ROOT] Recovery writes may have occurred; failed stage: " : "[ROOT] Rejected before recovery writes; failed stage: ");
        serial_puts(stage);serial_puts("\n");return false;
    }
    reason="mandatory installed-system validation failed (recovery may have written)";
    if (!validate_tree(mount)) {
        accounts_abandon();
        /* Detached nodes have no surviving validation handles. A failed freeze
         * is never reported clean, and tainted state is never reopened in place. */
        (void)ext4_freeze_and_sync(mount);(void)ext4_discard_prepared_root(mount);return false;
    }
    reason="root publication failed";
    if (ext4_publish_root(mount)) {accounts_abandon();(void)ext4_freeze_and_sync(mount);(void)ext4_discard_prepared_root(mount);return false;}
    published=true;
    serial_puts("[ROOT] Installed root admitted by PARTUUID; volatile namespaces attached\n");return true;
}
void installed_root_login_failed(void) {accounts_disable();reason="installed login failed; installed authentication disabled";}
bool installed_root_quiesce(void) {return !published || ext4_quiesce_installed_root(storage_quiesce)==0;}
const char *installed_root_recovery_hash(void) {return recovery_valid ? recovery.hash : NULL;}
void installed_root_recovery_notice(void) {
    console_puts("FortressOS recovery: ");console_puts(reason);console_puts("\n");
    serial_puts("[ROOT RECOVERY] ");serial_puts(reason);serial_puts("\n");
    if (!recovery_valid) console_puts("No valid independent recovery credential. Use separate live rescue media.\n");
}
