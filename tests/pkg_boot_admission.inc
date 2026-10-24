/* Private copied-kernel only. No root/VFS/account publication. */
#include "boot_info.h"
#include "serial.h"
#include "nvme.h"
#include "gpt.h"
#include "smp.h"
#include "crypto.h"
#include "pkg_selector_candidate.h"
#include "pkg_backend_candidate.h"
#include "../kernel/time.h"
#include "../../user/tools/digest.h"
static uint8_t jb_bundle[32768],jb_witness[32+20*128];
static size_t jb_bundle_n;
static ps_policy jb_policy;
_Static_assert(sizeof(ps_policy)==1136,"private boot policy transport");
static ps_workspace jb_workspace;
static ps_result jb_result;
static uint8_t jb_selector[1536];
static size_t jb_selector_n;
static ext4_engine_t *jb_engine;
static block_dev_t jb_disk;
static unsigned jb_admitted;
static bool jb_authorized,jb_read_fault;
static uint64_t jb_writes,jb_flushes,jb_identity=1;
#include "pkg_boot_read_generated.inc"
static bool jb_read(block_dev_t *d,uint64_t l,void *p){
    (void)smp_tlb_service_local();if(l>=d->sector_count || (jb_read_fault && l==2050))return false;
    return nvme_read_sector(l,p);
}
static bool jb_write(block_dev_t *d,uint64_t l,const void *p){
    jb_writes++;if(!jb_authorized || l>=d->sector_count)return false;return nvme_write_sector(l,p);
}
static bool jb_flush(block_dev_t *d){(void)d;jb_flushes++;return jb_authorized && nvme_flush();}
static int jb_extra(ext4_mount_t *fs){
    static const char *paths[]={"/pkg/descriptor.v2","/etc/passwd","/etc/group","/etc/shadow"};
    for(unsigned i=0;i<4;i++){
        e4_inode_t in;int r=identity_path(fs,paths[i],&in);if(r)return r;
        if(in.mode!=VFS_S_IFREG || in.uid || in.gid || !in.generation ||
            (in.full_mode&07777)!=(i==1||i==2?0644:0600))return -VFS_EINVAL;
        r=e4_mapping(&in);if(r)return r;
        if(!in.map->count || in.map->count>4)return -VFS_EINVAL;
        uint8_t *p=jb_witness+32+128*(i+16);e4_p16(p,(uint16_t)(i+17));e4_p16(p+2,1);
        e4_p32(p+4,in.ino);e4_p32(p+8,in.generation);e4_p32(p+12,in.full_mode&07777);
        e4_p32(p+16,(uint32_t)in.size);e4_p32(p+20,(uint32_t)(in.size>>32));e4_p16(p+32,in.map->count);
        uint64_t blocks=0;
        for(unsigned j=0;j<in.map->count;j++){
            e4_extent_t x=in.map->entries[j];if(x.unwritten || x.logical!=blocks)return -VFS_EINVAL;
            uint8_t *q=p+40+16*j;e4_p32(q,x.logical);e4_p32(q+4,x.len);e4_p32(q+8,x.physical);blocks+=x.len;
        }
        e4_p32(p+24,(uint32_t)blocks);
    }
    return 0;
}
static int jb_namespace(ext4_mount_t *fs){
    e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));if(!w)return -VFS_ENOMEM;
    e4_inode_t root;int r=e4_inode(fs,2,&root);
    if(!r && (root.mode!=VFS_S_IFDIR || root.uid || root.gid || (root.full_mode&07777)!=0755))r=-VFS_EACCES;
    if(!r)r=e4_ns_child(fs,2,"etc",0755,&w->anchors[0]);
    if(!r)r=e4_ns_child(fs,2,"var",0755,&w->anchors[1]);
    if(!r)r=e4_ns_child(fs,w->anchors[1],"lib",0755,&w->anchors[2]);
    if(!r)r=e4_ns_child(fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
    if(!r)r=e4_ns_child(fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
    if(!r)r=e4_ns_directory(fs,w,0,2);
    if(!r)r=e4_ns_directory(fs,w,1,w->anchors[3]);
    if(!r && w->report.present_roles!=7)r=-VFS_EINVAL;
    kfree(w);return r;
}
static int jb_reserved_names(ext4_mount_t *fs){
    static const char *paths[]={"/pkg","/pkg/transaction.v1","/usr/bin","/usr/share/pkg"};
    static const char *parents[]={"/","/pkg","/usr","/usr/share"};
    static const char *names[]={"enrollment.v1","grant.v1","ownership.v1","transaction.v1",
        "origin.v1","account-generation.v1","state.v1","trust.v1","descriptor.v2"};
    for(unsigned directory=0;directory<4;directory++){
        e4_inode_t in,parent;int r=identity_path(fs,paths[directory],&in);
        if(!r)r=identity_path(fs,parents[directory],&parent);
        if(r)return r;
        if(parent.mode!=VFS_S_IFDIR || parent.uid || parent.gid ||
            (parent.full_mode&07777)!=(directory==1?0700:0755))return -VFS_EACCES;
        unsigned count=directory?0:9,seen=0;
        for(unsigned index=0;index<=count+2;index++){
            vfs_dirent_t entry;uint32_t found=0;r=e4_scan(&in,NULL,index,&entry,&found);
            if(r<0)return r;
            if(!found){if(seen!=(1u<<(count+2))-1)return -VFS_EINVAL;break;}
            unsigned bit;
            if(!strcmp(entry.name,".")){bit=0;if(found!=in.ino || entry.type!=VFS_DIRECTORY)return -VFS_EINVAL;}
            else if(!strcmp(entry.name,"..")){bit=1;if(found!=parent.ino || entry.type!=VFS_DIRECTORY)return -VFS_EINVAL;}
            else{
                unsigned child=0;while(child<count && strcmp(entry.name,names[child]))child++;
                if(child==count || entry.type!=(child==3?VFS_DIRECTORY:VFS_FILE))return -VFS_EINVAL;
                bit=child+2;
            }
            if((seen&(1u<<bit)) || index==count+2)return -VFS_EINVAL;
            seen|=1u<<bit;
        }
    }
    return 0;
}
static int jb_validate(ext4_engine_t *engine){
    if(engine->orphan_count || engine->orphan_recovery_required)return -VFS_EINVAL;
    ext4_mount_t *fs=engine->fs;int r=jb_namespace(fs);
    if(!r)r=jb_reserved_names(fs);
    if(r)return r;
    e4_inode_t descriptor;r=identity_path(fs,"/pkg/descriptor.v2",&descriptor);if(r)return r;
    if(descriptor.size!=2656)return -VFS_EINVAL;
    r=e4_mapping(&descriptor);if(r)return r;
    if(descriptor.map->count!=1 || descriptor.map->entries[0].unwritten || descriptor.map->entries[0].len!=1)return -VFS_EINVAL;
    if(!e4_bytes(fs,(uint64_t)descriptor.map->entries[0].physical*fs->bs,jb_bundle,2656))return -VFS_EIO;
    memset(jb_witness,0,sizeof jb_witness);memcpy(jb_witness,"FORT-PKG-MAP-v1\0",16);
    e4_p32(jb_witness+16,sizeof jb_witness);e4_p16(jb_witness+20,1);e4_p16(jb_witness+22,20);
    memcpy(jb_witness+32,jb_bundle+416,16*128);
    r=jb_extra(fs);if(!r)r=identities(fs,jb_witness,sizeof jb_witness);
    if(!r)r=export_bundle(fs,"",jb_witness);
    jb_policy.now=time_get_epoch_seconds();
    if(!r && ps_validate(jb_selector,jb_selector_n,jb_bundle,jb_bundle_n,&jb_policy,&jb_workspace,&jb_result))r=-VFS_EACCES;
    return r;
}
void jb_boot(const boot_info_t *b,bool selected,bool read_fault){
    const char *stage="module-admission";int r=-VFS_EINVAL;jbd2_plan_t *plan=NULL;ext4_engine_t *preview_engine=NULL;
    e4_journal_guard_t *guard=NULL;
    if(!selected || strcmp(b->cmdline,"boot_mode=pkg-test-v1") || !b->pkg_modules_valid)goto refused;
    memcpy(&jb_policy,(const void *)b->pkg_policy_vaddr,sizeof jb_policy);
    memcpy(jb_selector,(const void *)b->pkg_selector_vaddr,b->pkg_selector_size);jb_selector_n=b->pkg_selector_size;
    stage="measurements";uint8_t hash[32];
    if(!b->kernel_file_vaddr || !b->kernel_file_size || b->kernel_file_size>32u*1024*1024 ||
        !b->has_initramfs || b->initramfs_size>8u*1024*1024 ||
        crypto_sha256((void *)b->kernel_file_vaddr,b->kernel_file_size,hash) || memcmp(hash,jb_policy.kernel,32) ||
        crypto_sha256((void *)b->initramfs_vaddr,b->initramfs_size,hash) || memcmp(hash,jb_policy.bootstrap,32))goto refused;
    jb_policy.now=time_get_epoch_seconds();
    stage="device-admission";
    if(!nvme_init() || !nvme_has_unique_namespace() || nvme_get_sector_size()!=512 || nvme_get_sector_count()!=131072)goto refused;
    jb_read_fault=read_fault;jb_disk=(block_dev_t){.sector_size=512,.sector_count=131072,.read_sector=jb_read,.write_sector=jb_write,.flush=jb_flush};
    gpt_policy_result_t gp;stage="gpt";
    if(!gpt_parse_ex(&jb_disk,&gp) || gp!=GPT_POLICY_PRIMARY_CONSISTENT || gpt_get_partition_count()!=2)goto refused;
    gpt_partition_t *partition=gpt_get_partition(0);gpt_guid_t want;
    if(!gpt_str_to_guid("12345678-1234-4567-89ab-123456789abc",&want) || !gpt_guid_equal(&partition->unique_guid,&want) ||
       !gpt_guid_equal(&partition->type_guid,&GPT_GUID_LINUX_FS) || partition->starting_lba!=2048 || partition->sector_count!=65536)goto refused;
    static const unsigned order[16]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    for(unsigned i=0;i<16;i++)if(partition->unique_guid.bytes[order[i]]!=jb_policy.uuid[1][i])goto refused;
    stage="journal-analyze";
    ext4_mount_t *bootstrap=kcalloc(1,sizeof(*bootstrap));guard=kcalloc(1,sizeof(*guard));
    uint32_t *map=NULL;jbd2_source_t source;
    if(!bootstrap || !guard){kfree(bootstrap);r=-VFS_ENOMEM;goto refused;}
    guard->lifecycle=true;bootstrap->dev=&partition->block_dev;bootstrap->bs=512;bootstrap->blocks=65536;bootstrap->journal_bootstrap=true;
    r=e4_journal_source(bootstrap,guard,&map,&source);
    if(!r)r=jbd2_analyze(&partition->block_dev,&source,&plan,NULL);
    kfree(map);e4_discard(bootstrap);if(r)goto refused;
    static block_dev_t preview;
    preview=(block_dev_t){.sector_size=512,.sector_count=65536,.read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=plan};
    stage="projected-root";r=e4_open_journal_orphans_owned(&preview,true,&preview_engine,false,true);if(r)goto refused;
    r=jb_validate(preview_engine);if(r)goto refused;
    ext4_engine_close(preview_engine);preview_engine=NULL;
    if(jb_writes || jb_flushes){r=-VFS_EIO;goto refused;}
    serial_puts("PKGBOOT PROJECTED AUTHENTICATED before-writes=0\n");
    stage="authorized-replay";jb_authorized=true;r=jbd2_replay(plan,true);jb_authorized=false;if(r)goto refused;
    jbd2_release(plan);plan=NULL;kfree(guard);guard=NULL;
    stage="reopened-root";r=e4_open_journal_orphans_owned(&partition->block_dev,true,&jb_engine,false,true);
    if(!r)r=jb_validate(jb_engine);
    if(r)goto refused;
    jb_engine->fs->rw_map=kmalloc(sizeof(e4_map_t)+E4_MAP_MAX*sizeof(e4_extent_t));
    if(!jb_engine->fs->rw_map){r=-VFS_ENOMEM;goto refused;}
    jb_engine->fs->engine=jb_engine;
    __atomic_store_n(&jb_admitted,1,__ATOMIC_RELEASE);serial_puts("PKGBOOT ADMITTED IDLE=1 detached\n");return;
refused:
    jb_authorized=false;if(preview_engine)ext4_engine_close(preview_engine);
    if(plan)jbd2_release(plan);
    kfree(guard);
    if(jb_engine){ext4_engine_close(jb_engine);jb_engine=NULL;}
    serial_puts("PKGBOOT REFUSED stage=");serial_puts(stage);serial_puts(" error=");serial_print_dec((uint64_t)-r);
    serial_puts(" writes=");serial_print_dec(jb_writes);serial_puts(" flushes=");serial_print_dec(jb_flushes);serial_puts("\n");
}
static int jb_status(void *context,pkg_transaction_status *out){
    if(context!=&jb_identity || !__atomic_load_n(&jb_admitted,__ATOMIC_ACQUIRE))return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=jb_validate(jb_engine);
    if(!r)*out=(pkg_transaction_status){.phase=PKG_TX_IDLE,.sequence=1,.account_sequence=1,.credential_generation=1};
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
const pkg_transaction_backend *gk_backend(void){static const pkg_transaction_backend b={&jb_identity,jb_status,jb_status};return &b;}
bool gk_root_admitted(void){return __atomic_load_n(&jb_admitted,__ATOMIC_ACQUIRE)!=0;}
int64_t gk_storage_test(uint64_t op){if(op==13)return jb_writes;if(op==14)return jb_flushes;return SYSCALL_EINVAL;}
