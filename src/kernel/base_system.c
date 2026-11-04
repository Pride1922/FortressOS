#include "base_system.h"
#include "base_update.h"
#include "storage.h"
#include "ext4.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "crypto.h"
#include "time.h"
#include "thread.h"
#include "pkg_owner.h"
#include "../../user/pkg/repository.h"

static bool enabled,live,fenced;
static uint8_t running_ready[512];
static char recovery_record[512];static size_t recovery_bytes;
static base_state states[2],accepted_floor;static unsigned running,selected,invalid;static bool mixed;
static uint64_t current_build,current_abi;static uint8_t current_platform[32];
static int read_io(void *ctx,uint64_t lba,void *p,unsigned n){
    (void)ctx;
    if(live){tcb_t *t=thread_current();if(!t||!pw_recovery_step(t->pkg_owner))return -1;return ext4_base_io(0,lba,n,p);}
    return storage_read_sectors(lba,n,p)?0:-1;
}
static int write_io(void *ctx,uint64_t lba,const void *p,unsigned n){
    (void)ctx;tcb_t *t=thread_current();
    if(!live||!t||!pw_recovery_step(t->pkg_owner))return -1;
    return ext4_base_io(1,lba,n,(void *)p);
}
static int flush_io(void *ctx){(void)ctx;return live?ext4_base_io(2,0,0,NULL):(storage_flush()?0:-1);}
static kernel_update_io_t io_value(void){return (kernel_update_io_t){.sectors=storage_get_sector_count(),.read=read_io,.write=write_io,.flush=flush_io};}
static void capture(base_update_work *w){memcpy(states,w->states,sizeof states);running=w->running;selected=w->disk.active;invalid=w->invalid;mixed=w->mixed;accepted_floor=w->floors[w->floor];}
static int config_file(const char *path,uint8_t *out,size_t cap,size_t *n,bool bootstrap){
    creds_t root;creds_init_root(&root);int error;
    vfs_node_t *node=bootstrap?vfs_bootstrap_lookup_creds(path,&root,&error):vfs_lookup_creds(path,&root,&error);
    if(!node)return error?error:-VFS_ENOENT;
    vfs_metadata_t m;int r=vfs_metadata(node,&m);size_t at=0;
    if(!r&&(m.type!=VFS_FILE||m.uid||m.gid||(m.mode&0022)||!m.size||m.size>cap))r=-VFS_EACCES;
    while(!r&&at<m.size){int64_t got=node->read?node->read(node,at,out+at,m.size-at):(memcpy(out+at,(uint8_t *)node->data+at,m.size-at),(int64_t)(m.size-at));
        if(got<=0||(uint64_t)got>m.size-at)r=-VFS_EIO;else at+=(size_t)got;}
    vfs_node_put(node);if(!r)*n=at;return r;
}
bool base_system_boot(const boot_info_t *info,const root_readiness_digests_t *loaded){
    if(enabled||live||!info||!loaded||!info->installed_modules_valid||info->readiness_size!=512)return false;
    uint8_t marker[BASE_RELEASE_BYTES];size_t n=0;
    if(config_file("/base/release.v1",marker,sizeof marker,&n,true)||base_release_parse(marker,n,&current_build,&current_abi,current_platform)||
       config_file("/bootstrap/recovery.v1",(uint8_t *)recovery_record,sizeof recovery_record,&recovery_bytes,true))return false;
    memcpy(running_ready,(void *)info->readiness_vaddr,512);
    boot_timing_t timing;boot_timing_begin(&timing,"workspace");
    base_update_work *w=kmalloc(sizeof *w);void *bytes=kmalloc_large(BASE_ESP_BYTES);bool ok=false;
    if(w&&bytes){kernel_update_io_t io=io_value();
        boot_timing_mark(&timing,"disk-layout");
        if(!base_update_prepare_timed(&io,bytes,w,running_ready,&timing)){
            boot_timing_mark(&timing,"loaded-modules");
            base_state *s=w->states+w->running;
            ok=s->build==current_build&&s->abi==current_abi&&!memcmp(s->platform,current_platform,32)&&
                !memcmp(loaded->kernel,s->kernel_hash,32)&&!memcmp(loaded->bootstrap,s->bootstrap_hash,32);
            if(ok)capture(w);
        }
    }
    boot_timing_mark(&timing,"cleanup");
    kfree(bytes);kfree(w);
    boot_timing_mark(&timing,NULL);boot_timing_emit(&timing,"BASE-BOOT");
    enabled=ok;return ok;
}
void base_system_published(void){if(enabled)live=true;}
bool base_system_version(uint64_t *abi,uint8_t platform[32]){
    if(!enabled)return false;
    *abi=current_abi;memcpy(platform,current_platform,32);return true;
}
int base_system_catalog_check(uint64_t catalog,const uint8_t hash[32],uint64_t root,const uint8_t root_hash[32]){
    if(!enabled)return 0;
    if(fenced)return -VFS_EIO;
    if(accepted_floor.catalog>catalog||
        (accepted_floor.catalog&&accepted_floor.catalog==catalog&&memcmp(accepted_floor.catalog_hash,hash,32))||
        accepted_floor.root>root||(accepted_floor.root&&accepted_floor.root==root&&memcmp(accepted_floor.root_hash,root_hash,32)))return -VFS_EACCES;
    return 0;
}
static void result(pkg_base_result_v1 *out){
    *out=(pkg_base_result_v1){.size=sizeof *out,.version=1,.running_build=current_build,.selected_build=states[selected].build,.abi=current_abi};
    if(selected!=running)out->flags|=PKG_BASE_REBOOT;
    if(states[running].phase!=BASE_STATE_CONFIRMED)out->flags|=PKG_BASE_UNCONFIRMED;
    if(mixed||invalid)out->flags|=PKG_BASE_RECOVERY;
    if(states[running^1].phase==BASE_STATE_PREPARING)out->flags|=PKG_BASE_RETRY;
    if(fenced)out->flags|=PKG_BASE_FENCED;
    out->floor_build=accepted_floor.build;out->catalog=accepted_floor.catalog;memcpy(out->artifact,accepted_floor.artifact,32);
    memcpy(out->platform,current_platform,32);
}
struct base_operation {base_update_work work;pkg_repository_trust trust;base_artifact artifact;fpk_catalog catalog;base_state release;uint8_t wire[1024],root[288];};
int base_system_operation(unsigned op,const uint8_t *catalog,size_t cn,const uint8_t *artifact,size_t an,pkg_base_result_v1 *out){
    if(!enabled||!live)return -VFS_EOPNOTSUPP;
    if(op==PKG_BASE_STATUS){result(out);return 0;}
    if(op==PKG_BASE_CONFIRM && !fenced&&!invalid&&!mixed&&running==selected&&states[running].phase==BASE_STATE_CONFIRMED){result(out);return 0;}
    if(fenced)return -VFS_EIO;
    struct base_operation *st=kcalloc(1,sizeof *st);void *bytes=kmalloc_large(BASE_ESP_BYTES);
    if(!st||!bytes){kfree(st);kfree(bytes);return -VFS_ENOMEM;}
    kernel_update_io_t io=io_value();int r=-VFS_EACCES;
    if(base_update_prepare(&io,bytes,&st->work,running_ready))goto done;
    capture(&st->work);
    if(op==PKG_BASE_STAGE){
        timespec_t now={0};time_get_timespec(CLOCK_REALTIME,&now);size_t n=0;
        if(config_file("/etc/pkg/trust.conf",st->wire,sizeof st->wire,&n,false)||
           pkg_repository_trust_decode(st->wire,n,(uint64_t)now.tv_sec,&st->trust)||
           base_artifact_verify(artifact,an,catalog,cn,&st->trust.catalog,&st->artifact)||
           fpk_catalog_verify(catalog,cn,&st->trust.catalog,&st->catalog)||
           memcmp(st->artifact.platform,current_platform,32)||memcmp(st->trust.platform,current_platform,32)||st->artifact.abi!=current_abi)goto done;
        base_state *s=&st->release;s->build=st->artifact.build;s->abi=st->artifact.abi;s->catalog=st->catalog.revision;s->root=st->trust.catalog.root_version;
        memcpy(s->platform,current_platform,32);memcpy(s->catalog_hash,st->catalog.sha256,32);(void)crypto_sha256(artifact,an,s->artifact);
        memcpy(st->root,st->trust.catalog.repository,16);memcpy(st->root+16,st->trust.catalog.keys,256);
        for(unsigned i=0;i<4;i++){st->root[272+i]=(uint8_t)(st->trust.catalog.key_count>>(8*i));st->root[276+i]=(uint8_t)(st->trust.catalog.threshold>>(8*i));}
        for(unsigned i=0;i<8;i++)st->root[280+i]=(uint8_t)(s->root>>(8*i));
        (void)crypto_sha256(st->root,sizeof st->root,s->root_hash);
        r=base_system_catalog_check(s->catalog,s->catalog_hash,s->root,s->root_hash);if(r)goto done;
        if(s->build<accepted_floor.build || (s->build==accepted_floor.build&&accepted_floor.catalog&&memcmp(s->artifact,accepted_floor.artifact,32))){r=-VFS_EACCES;goto done;}
        r=ext4_pkg_check_base_catalog(s->catalog,s->catalog_hash,s->root,s->root_hash,st->artifact.bootstrap,st->artifact.bootstrap_bytes);if(r)goto done;
        r=-VFS_EACCES;
        if(s->build==current_build && !memcmp(s->artifact,states[running].artifact,32)){r=0;goto done;}
        if(st->work.invalid||st->work.mixed||selected!=running||states[running].phase!=BASE_STATE_CONFIRMED){r=-VFS_EAGAIN;goto done;}
        /* Admission is complete. A failure from here fences this boot even when
         * the device cannot distinguish a completed write from an error. */
        if(base_update_apply(&io,bytes,&st->work,&st->artifact,s,recovery_record,recovery_bytes)){if(st->work.wrote)fenced=true;r=st->work.wrote?-VFS_EIO:-VFS_EACCES;goto done;}
    }else if(op==PKG_BASE_CONFIRM||op==PKG_BASE_RECOVER){
        if(base_update_select(&io,bytes,&st->work,op==PKG_BASE_RECOVER)){if(st->work.wrote)fenced=true;r=st->work.wrote?-VFS_EIO:-VFS_EACCES;goto done;}
    }else {r=-VFS_EINVAL;goto done;}
    if(base_update_prepare(&io,bytes,&st->work,running_ready)){if(st->work.wrote)fenced=true;r=st->work.wrote?-VFS_EIO:-VFS_EACCES;goto done;}
    capture(&st->work);r=0;
done:
    if(!r)result(out);
    kfree(bytes);kfree(st);return r;
}
