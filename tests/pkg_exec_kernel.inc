/* Private copied kernel only. Boot admission precedes replay and publication. */
#include "boot_info.h"
#include "serial.h"
#include "nvme.h"
#include "gpt.h"
#include "smp.h"
#include "thread.h"
#include "process_table.h"
#include "syscall.h"
#include "vmm.h"
#include "accounts.h"
#include "pkg_exec_wire.h"
static bool mx_published,mx_boot_authority,mx_storage_authority,mx_cut;
static uint64_t mx_writes,mx_flushes,mx_identity=1;
static block_dev_t mx_disk;
static unsigned mx_pause,mx_waiting;
static unsigned mx_backend_busy;
static uint64_t mx_wait_pid;
static const char *mx_base_paths[]={"/bin/pkg-boot","/bin/shell","/bin/sudo","/bin/login","/etc/passwd","/etc/group","/etc/shadow"};
static uint32_t mx_base_inodes[8];
static uint8_t mx_io[4096];
static px_policy mx_policy;
static int px_core_commit(ext4_engine_t *e){
    if(__atomic_load_n(&mx_published,__ATOMIC_ACQUIRE)&&px&&px->fs==e->fs)for(unsigned i=0;i<8;i++){int r=px_protect(e,mx_base_inodes[i]);if(r)return r;}
    return mx_raw_commit(e);
}
static void mx_outw(uint16_t port,uint16_t value){__asm__ volatile("outw %0,%1"::"a"(value),"Nd"(port):"memory");}
static bool mx_fw_key(uint8_t key[32]){
    mx_outw(0x510,0);char sig[4];for(unsigned i=0;i<4;i++)sig[i]=inb(0x511);if(memcmp(sig,"QEMU",4))return false;
    mx_outw(0x510,0x19);uint32_t count=0;for(unsigned i=0;i<4;i++)count=count*256+inb(0x511);if(count>256)return false;
    for(unsigned i=0;i<count;i++){
        uint32_t size=0;uint16_t index=0;for(unsigned j=0;j<4;j++)size=size*256+inb(0x511);for(unsigned j=0;j<2;j++)index=index*256+inb(0x511);
        (void)inb(0x511);(void)inb(0x511);char name[56];for(unsigned j=0;j<56;j++)name[j]=inb(0x511);name[55]=0;
        if(!strcmp(name,"opt/fortress/pkg_exec_key")){if(size!=32)return false;mx_outw(0x510,index);for(unsigned j=0;j<32;j++)key[j]=inb(0x511);return true;}
    }return false;
}
static bool mx_read(block_dev_t *d,uint64_t l,void *p){(void)smp_tlb_service_local();return l<d->sector_count&&nvme_read_sector(l,p);}
static bool mx_write(block_dev_t *d,uint64_t l,const void *p){__atomic_add_fetch(&mx_writes,1,__ATOMIC_RELAXED);return mx_storage_authority&&l<d->sector_count&&nvme_write_sector(l,p);}
static bool mx_flush(block_dev_t *d){(void)d;__atomic_add_fetch(&mx_flushes,1,__ATOMIC_RELAXED);return mx_storage_authority&&nvme_flush();}
static uintptr_t mx_actor(void){return (uintptr_t)thread_current();}
static bool mx_released(void *p){(void)p;return !__atomic_load_n(&mx_pause,__ATOMIC_ACQUIRE);}
static bool mx_authorized(void){
    if(mx_boot_authority)return true;
    tcb_t *t=thread_current();if(!t)return false;
    if(mx_cut&&px->state.phase==PX_COMMIT){
        mx_cut=false;__atomic_store_n(&mx_pause,PX_COMMIT,__ATOMIC_RELEASE);serial_puts("PKGEXEC CUT durable-COMMIT\n");
    }
    if(__atomic_load_n(&mx_pause,__ATOMIC_ACQUIRE)==px->state.phase&&px->state.phase){
        __atomic_store_n(&mx_wait_pid,t->tid,__ATOMIC_RELEASE);__atomic_store_n(&mx_waiting,1,__ATOMIC_RELEASE);
        sched_wait_until(&mx_pause,mx_released,NULL);
    }
    return !process_signal_interrupt()&&pw_recovery_step((pw_key){t->pkg_owner.generation,t->pkg_owner.slot});
}
static int mx_measure_base(ext4_mount_t *fs,const uint8_t *expected){
    for(unsigned i=0;i<8;i++){
        e4_inode_t in;int r=px_lookup(fs,i==7?"/bin":mx_base_paths[i],&in);if(r)return r;
        if(in.uid||in.gid||(in.full_mode&07777)!=(i==2?04755:i==6?0600:i==4||i==5?0644:0755)||in.mode!=(i==7?VFS_S_IFDIR:VFS_S_IFREG))return -VFS_EACCES;
        mx_base_inodes[i]=in.ino;if(i==7)continue;
        r=e4_mapping(&in);if(r)return r;crypto_sha256_ctx hash;crypto_sha256_init(&hash);
        for(uint64_t at=0;at<in.size;){size_t n=in.size-at;if(n>sizeof mx_io)n=sizeof mx_io;
            uint32_t physical=0;r=e4_dir_block(&in,(uint32_t)(at/4096),&physical);if(r)return r;
            if(!e4_bytes(fs,(uint64_t)physical*4096,mx_io,n))return -VFS_EIO;
            crypto_sha256_update(&hash,mx_io,n);at+=n;}
        uint8_t sum[32];crypto_sha256_final(&hash,sum);if(memcmp(sum,expected+32*i,32))return -VFS_EACCES;
    }return 0;
}
static int mx_exact(ext4_mount_t *fs,const char *path,uint32_t parent,const char *const *names,const uint32_t *ids,const uint32_t *types,unsigned count){
    e4_inode_t dir;int r=px_lookup(fs,path,&dir);if(r)return r;unsigned seen=0;
    for(unsigned at=0;at<=count+2;at++){
        uint32_t found=0;vfs_dirent_t entry;r=e4_scan(&dir,NULL,at,&entry,&found);if(r<0)return r;
        if(!found)return seen==(1u<<(count+2))-1?0:-VFS_EACCES;
        unsigned bit;
        if(!strcmp(entry.name,".")){bit=0;if(found!=dir.ino||entry.type!=VFS_DIRECTORY)return -VFS_EACCES;}
        else if(!strcmp(entry.name,"..")){bit=1;if(found!=parent||entry.type!=VFS_DIRECTORY)return -VFS_EACCES;}
        else{unsigned i=0;while(i<count&&strcmp(entry.name,names[i]))i++;if(i==count||entry.type!=types[i]||(ids[i]&&found!=ids[i]))return -VFS_EACCES;bit=i+2;}
        if((seen&(1u<<bit))||at==count+2)return -VFS_EACCES;
        seen|=1u<<bit;
    }return -VFS_EACCES;
}
static int mx_namespace(ext4_mount_t *fs){
    static const char *const bin[]={"pkg-boot","shell","sudo","login"};static const uint32_t files[]={VFS_FILE,VFS_FILE,VFS_FILE,VFS_FILE};
    int r=mx_exact(fs,"/bin",2,bin,mx_base_inodes,files,4);if(r)return r;
    char command[33],package[33];memcpy(command,px->shape.command,px->shape.command_length);command[px->shape.command_length]=0;memcpy(package,px->shape.package,px->shape.package_length);package[px->shape.package_length]=0;
    const char *name=command;unsigned executable=px_member(16);uint32_t id=px->state.files[executable].inode,type=VFS_FILE;
    r=mx_exact(fs,"/usr/bin",px->policy.anchors[6].inode,&name,&id,&type,(px->state.published&(1u<<executable))?1:0);if(r)return r;
    unsigned present=0;id=px->state.directories[0].inode;
    if(px->directory_count&&id&&!px_dir_gone(0)){uint8_t raw[256];r=px_inode_bytes(fs->engine,id,raw,false);if(r)return r;present=e4_u16(raw+26)!=0;}
    name=package;type=VFS_DIRECTORY;r=mx_exact(fs,"/usr/share/pkg",px->policy.anchors[7].inode,&name,&id,&type,present);if(r)return r;
    static const char *const pkg[]={"enrollment.v1","grant.v1","ownership.v1","transaction.v1","origin.v1","account-generation.v1","state.v1","trust.v1","descriptor.v2","install.v2","work-v2"};
    static const uint32_t kinds[]={VFS_FILE,VFS_FILE,VFS_FILE,VFS_DIRECTORY,VFS_FILE,VFS_FILE,VFS_FILE,VFS_FILE,VFS_FILE,VFS_FILE,VFS_DIRECTORY};
    uint32_t ids[11]={0};ids[9]=px->policy.anchors[0].inode;ids[10]=px->policy.anchors[1].inode;
    r=mx_exact(fs,"/pkg",2,pkg,ids,kinds,11);if(r)return r;
    return mx_exact(fs,"/pkg/transaction.v1",px->policy.anchors[5].inode,NULL,NULL,NULL,0);
}
static int mx_admit_engine(ext4_engine_t *e,const uint8_t *base){
    e->fs->rw_map=kmalloc(sizeof(e4_map_t)+E4_MAP_MAX*sizeof(e4_extent_t));if(!e->fs->rw_map)return -VFS_ENOMEM;
    e->fs->engine=e;px->fs=e->fs;int r=px_validate(e->fs);if(!r&&px->state.phase)r=px_rederive(px->state.operation,true);
    /* This fixture's account lineage is immutable. Foreign redo must not be
     * admitted and then consumed by accounts_prepare after base measurement. */
    if(!r){
        e4_ns_workspace_t *w=kcalloc(1,sizeof *w);if(!w)return -VFS_ENOMEM;
        r=e4_ns_child(e->fs,2,"etc",0755,&w->anchors[0]);
        if(!r)r=e4_ns_child(e->fs,2,"var",0755,&w->anchors[1]);
        if(!r)r=e4_ns_child(e->fs,w->anchors[1],"lib",0755,&w->anchors[2]);
        if(!r)r=e4_ns_child(e->fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
        if(!r)r=e4_ns_child(e->fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
        if(!r)r=e4_ns_directory(e->fs,w,0,2);
        if(!r)r=e4_ns_directory(e->fs,w,1,w->anchors[3]);
        if(!r&&w->report.present_roles!=7)r=-VFS_EACCES;
        kfree(w);
    }
    if(!r)r=mx_measure_base(e->fs,base);
    if(!r)r=mx_namespace(e->fs);
    return r;
}
bool mx_ready(void){return __atomic_load_n(&mx_published,__ATOMIC_ACQUIRE);}
void jb_boot(const boot_info_t *b,bool selected,bool read_fault){
    mx_cut=read_fault;int r=-VFS_EACCES;const char *stage="modules";jbd2_plan_t *journal=NULL;ext4_engine_t *e=NULL;
    if(!selected||strcmp(b->cmdline,"boot_mode=pkg-exec-v1")||!b->pkg_modules_valid||b->pkg_selector_size!=768||b->pkg_policy_size<960)goto fail;
    if(!b->kernel_file_vaddr||!b->kernel_file_size||b->kernel_file_size>32u*1024*1024||!b->has_initramfs||!b->initramfs_vaddr||b->initramfs_size>8u*1024*1024)goto fail;
    const uint8_t *policy=(void *)b->pkg_policy_vaddr,*selector=(void *)b->pkg_selector_vaddr;size_t policy_n=b->pkg_policy_size;uint8_t sum[32];
    stage="measurements";if(memcmp(selector,"FORT-PKG-EXEC-B1",16))goto fail;
    crypto_sha256(policy,policy_n,sum);if(memcmp(sum,selector+80,32))goto fail;
    crypto_sha256((void *)b->kernel_file_vaddr,b->kernel_file_size,sum);if(memcmp(sum,selector+16,32))goto fail;
    crypto_sha256((void *)b->initramfs_vaddr,b->initramfs_size,sum);if(memcmp(sum,selector+48,32))goto fail;
    stage="selector-signatures";for(unsigned i=0;i<2;i++){
        const uint8_t *signature=selector+576+96*i;bool valid=false;
        for(unsigned j=0;j<3;j++){const uint8_t *key=policy+632+32*j;crypto_sha256(key,32,sum);if(!memcmp(sum,signature,32)&&!crypto_ed25519_verify(signature+32,key,selector,576))valid=true;}
        if(!valid||(i&&memcmp(selector+576,signature,32)>=0))goto fail;
    }
    stage="local-key";if(!mx_fw_key(mx_policy.record_key))goto fail;
    crypto_sha256(mx_policy.record_key,32,sum);if(memcmp(sum,selector+112,32))goto fail;
    memcpy(&mx_policy.catalog,policy,336);mx_policy.catalog.now=time_get_epoch_seconds();const uint8_t *cfg=policy+336;
    memcpy(mx_policy.filesystem,cfg,16);memcpy(mx_policy.layout,cfg+184,32);memcpy(mx_policy.minimum_hash,cfg+152,32);mx_policy.minimum_sequence=px_get(cfg+144,8);
    for(unsigned i=0;i<8;i++)mx_policy.anchors[i]=(px_anchor){px_get(cfg+16+16*i,4),px_get(cfg+20+16*i,4),px_get(cfg+24+16*i,8)};
    static fpk_span names[]={{(void *)"shell",5},{(void *)"sudo",4},{(void *)"pkg",3},{(void *)"pkg-boot",8},{(void *)"login",5}};
    mx_policy.resources=(pi_resources){.block_size=4096,.journal_images=64,.orphan_slots=16,.memory_bytes=32*1024*1024,.base=2,.base_names=names,.base_count=5,.capacity={px_get(cfg+216,8),px_get(cfg+224,8)}};
    for(unsigned i=0;i<32;i++)mx_policy.resources.platform[i]=i;
    mx_policy.current_actor=mx_actor;mx_policy.authorized=mx_authorized;
    size_t an=px_get(policy+952,4),cn=px_get(policy+956,4);if(an>policy_n-960||cn!=policy_n-960-an)goto fail;
    stage="signed-source";r=px_prepare(&mx_policy,policy+960,an,policy+960+an,cn,(void *)"toolbox",7,true);crypto_wipe(mx_policy.record_key,32);if(r)goto fail;
    stage="device";if(!nvme_init()||!nvme_has_unique_namespace()||nvme_get_sector_size()!=512||nvme_get_sector_count()!=131072)goto fail;
    mx_disk=(block_dev_t){.sector_size=512,.sector_count=131072,.read_sector=mx_read,.write_sector=mx_write,.flush=mx_flush};
    gpt_policy_result_t gp;if(!gpt_parse_ex(&mx_disk,&gp)||gp!=GPT_POLICY_PRIMARY_CONSISTENT||gpt_get_partition_count()!=2)goto fail;
    gpt_partition_t *part=gpt_get_partition(0);gpt_guid_t want;if(!gpt_str_to_guid("12345678-1234-4567-89ab-123456789abc",&want)||!gpt_guid_equal(&part->unique_guid,&want)||!gpt_guid_equal(&part->type_guid,&GPT_GUID_LINUX_FS)||part->starting_lba!=2048||part->sector_count!=65536)goto fail;
    stage="journal";ext4_mount_t *bootstrap=kcalloc(1,sizeof *bootstrap);e4_journal_guard_t *guard=kcalloc(1,sizeof *guard);uint32_t *map=NULL;jbd2_source_t src;
    if(!bootstrap||!guard){kfree(bootstrap);kfree(guard);goto fail;}bootstrap->dev=&part->block_dev;bootstrap->bs=512;bootstrap->blocks=65536;bootstrap->journal_bootstrap=true;guard->lifecycle=true;
    r=e4_journal_source(bootstrap,guard,&map,&src);if(!r)r=jbd2_analyze(&part->block_dev,&src,&journal,NULL);kfree(map);e4_discard(bootstrap);kfree(guard);if(r)goto fail;
    block_dev_t preview={.sector_size=512,.sector_count=65536,.read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=journal};
    stage="projected";r=e4_open_journal_orphans_owned(&preview,true,&e,false,true);if(!r)r=mx_admit_engine(e,policy+728);if(r)goto fail;
    ext4_engine_close(e);e=NULL;px->fs=NULL;serial_puts("PKGEXEC PROJECTED before-writes=0\n");
    stage="replay";mx_storage_authority=true;r=jbd2_replay(journal,true);if(r)goto fail;jbd2_release(journal);journal=NULL;
    r=e4_open_journal_orphans_owned(&part->block_dev,true,&e,false,true);if(!r)r=mx_admit_engine(e,policy+728);if(r)goto fail;
    stage="root";r=vfs_boot_begin_root_preparation();if(r)goto fail;e4_node_t *root=kcalloc(1,sizeof *root);if(!root){r=-VFS_ENOMEM;goto fail;}
    e4_inode_t in;r=e4_inode(e->fs,2,&in);if(r){kfree(root);goto fail;}px_copy(root->node.name,"/");px_copy(root->node.path,"/");e4_setup(root,&in);e->fs->cached[e->fs->nodes++]=root;e->fs->journal_mounted=true;e->fs->detached=true;e->fs->trusted_root=true;
    mx_boot_authority=true;r=px_control(0);mx_boot_authority=false;if(r)goto fail;
    {uint64_t flags=spin_lock_irqsave(&e4_lock);r=e->dirty?0:e4_journal_mark(e,false);spin_unlock_irqrestore(&e4_lock,flags);}if(r)goto fail;
    ext4_account_namespace_report_t report;r=ext4_root_account_namespace_admit(e->fs,&report);if(!r)r=accounts_prepare(e->fs);if(!r)r=ext4_publish_root(e->fs);if(r)goto fail;
    __atomic_store_n(&mx_published,true,__ATOMIC_RELEASE);serial_puts("PKGEXEC PUBLISHED installed-root real-account-service\n");return;
fail:
    mx_boot_authority=mx_storage_authority=false;crypto_wipe(mx_policy.record_key,32);if(e){ext4_engine_close(e);if(px)px->fs=NULL;}if(journal)jbd2_release(journal);px_destroy();
    serial_puts("PKGEXEC REFUSED stage=");serial_puts(stage);serial_puts(" error=");serial_print_dec(-r);serial_puts(" writes=");serial_print_dec(mx_writes);serial_puts(" flushes=");serial_print_dec(mx_flushes);serial_puts("\n");
}
static bool mx_enter(void){unsigned zero=0;return __atomic_compare_exchange_n(&mx_backend_busy,&zero,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
static void mx_leave(void){__atomic_store_n(&mx_backend_busy,0,__ATOMIC_RELEASE);}
static int mx_snapshot(void *context,pkg_transaction_status *out){
    if(context!=&mx_identity||!mx_ready())return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=px->fs->engine->tainted||px->fs->namespace_uncertain?-VFS_EIO:px_validate(px->fs);
    if(!r)*out=(pkg_transaction_status){.phase=!px->state.phase?PKG_TX_IDLE:px->state.phase==PX_BUILD?PKG_TX_RESERVED:px->state.phase==PX_COMMIT?PKG_TX_PENDING:PKG_TX_RETIRING,.sequence=px->state.sequence,.account_sequence=1,.credential_generation=1,.remaining={px->state.remaining[0],px->state.remaining[1]},.charged={px->state.charged[0],px->state.charged[1]}};
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int mx_status(void *context,pkg_transaction_status *out){if(!mx_enter())return -VFS_EAGAIN;int r=mx_snapshot(context,out);mx_leave();return r;}
static int mx_recover(void *context,pkg_transaction_status *out){if(context!=&mx_identity||!mx_ready())return -VFS_EROFS;if(!mx_enter())return -VFS_EAGAIN;int r=px_control(0);if(!r)r=mx_snapshot(context,out);mx_leave();return r;}
const pkg_transaction_backend *gk_backend(void){static const pkg_transaction_backend backend={&mx_identity,mx_status,mx_recover};return &backend;}
bool gk_root_admitted(void){return mx_ready();}
void mx_exec_owner(tcb_t *);
int64_t gk_storage_test(uint64_t op){
    if(op==30){__atomic_store_n(&mx_waiting,0,__ATOMIC_RELEASE);__atomic_store_n(&mx_pause,PX_COMMIT,__ATOMIC_RELEASE);return 0;}
    if(op==31)return __atomic_load_n(&mx_waiting,__ATOMIC_ACQUIRE);
    if(op==32){__atomic_store_n(&mx_pause,0,__ATOMIC_RELEASE);sched_wake_all(&mx_pause);return 0;}
    if(op==33)return __atomic_load_n(&mx_wait_pid,__ATOMIC_ACQUIRE);
    if(op==34){
        uint64_t pid=__atomic_load_n(&mx_wait_pid,__ATOMIC_ACQUIRE);creds_t old,next;
        if(!pid||!process_record_creds(pid,&old))return -VFS_EINVAL;
        next=old;next.cap_effective&=~CAP_SYS_ADMIN;if(!process_record_publish_creds(pid,&old,&next))return -VFS_EAGAIN;
        __atomic_store_n(&mx_pause,0,__ATOMIC_RELEASE);sched_wake_all(&mx_pause);return 0;
    }
    if(op==35){mx_exec_owner(NULL);return 0;}
    return op==13?(int64_t)__atomic_load_n(&mx_writes,__ATOMIC_RELAXED):op==14?(int64_t)__atomic_load_n(&mx_flushes,__ATOMIC_RELAXED):-VFS_EINVAL;
}
bool mx_copy_fault(void);
int64_t mx_transport(uintptr_t input,uintptr_t output,uint64_t size){
    if(size!=64)return SYSCALL_EINVAL;
    tcb_t *t=thread_current();uint64_t *pml4=vmm_get_active_pml4_virt();
    if(!t||!t->is_user||!vmm_validate_user_range(pml4,input,64,false)||!vmm_validate_user_range(pml4,output,80,true))return SYSCALL_EFAULT;
    mx_request q;memcpy(&q,(void *)input,64);if(q.size!=64||q.version!=1||q.reserved||q.reserved64||(q.operation!=PI_INSTALL&&q.operation!=PI_REMOVE)||memcmp(q.package,"toolbox",8)||!px_zero(q.package+8,24))return SYSCALL_EINVAL;
    if(!mx_ready()||!t->pkg_owner.generation)return SYSCALL_EACCES;
    pw_key operation;
    if(!pw_enter((pw_key){t->pkg_owner.generation,t->pkg_owner.slot},q.session,&operation))return SYSCALL_EACCES;
    /* The immutable catalog was authenticated at boot. Do not reuse that boot
     * clock snapshot to authorize a new install/remove after its interval. */
    bool entered=mx_enter();uint64_t now=time_get_epoch_seconds();int r=!entered?-VFS_EAGAIN:now<px_get(px->catalog+48,8)||now>=px_get(px->catalog+56,8)?-VFS_EACCES:0;
    if(!r)r=px_control(q.operation);
    pkg_transaction_status status;if(!r)r=mx_snapshot(&mx_identity,&status);if(!pw_complete(operation))r=-VFS_EIO;if(entered)mx_leave();if(r)return syscall_from_vfs_error(r);
    pb_result result={.size=80,.version=1,.phase=status.phase,.session=q.session,.sequence=status.sequence,.account_sequence=status.account_sequence,.credential_generation=status.credential_generation};
    if(mx_copy_fault())return SYSCALL_EFAULT;
    memcpy((void *)output,&result,80);return 0;
}
