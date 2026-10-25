/* Actual filesystem execution; process/credential callbacks below are host
 * controls, not scheduler, syscall or physical-device acceptance. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
static int px_reject_diagnostic(unsigned line,int code){fprintf(stderr,"executor refusal line=%u error=%d\n",line,code);return code;}
#define PX_REJECT(code) px_reject_diagnostic(__LINE__,code)
#define PX_ANCHOR_DIAGNOSTIC(i,in) fprintf(stderr,"anchor %u inode %u/%u generation %u/%u mode %o uid %u gid %u\n",i,in.ino,px->policy.anchors[i].inode,in.generation,px->policy.anchors[i].generation,in.full_mode,in.uid,in.gid)
#define main retained_mount_main
#define teardown retained_teardown
#include "ext4_mount_host.c"
#undef teardown
#undef main
#include "pkg_install_adapter.h"
static bool permitted=true,output_failure;static unsigned mutations;static int deny_phase=-1;static long cancel_at=-1;static ext4_fault_disk_t *disk_observed;
static uintptr_t current_actor(void){return (uintptr_t)pthread_self();}
static bool authorized(void){return permitted&&(deny_phase<0||px->state.phase!=(unsigned)deny_phase)&&(cancel_at<0||disk_observed->events<(size_t)cancel_at);}
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)b;assert(n==SYS_WRITE&&(a==1||a==2));return output_failure?-32:(long)c;}
#include "user/pkg/mutation_cli.h"
static int cli_mutate(void *unused,bool remove,const char *name,size_t length){
    (void)unused;mutations++;if(length!=px->shape.package_length||memcmp(name,px->shape.package,length))return -VFS_ENOENT;
    return px_control(remove?PI_REMOVE:PI_INSTALL);
}
static int cli_run(const char *verb,const char *name){
    pkg_mutation_backend backend={.mutate=cli_mutate};char *argv[]={"pkg",(char *)verb,(char *)name};
    return pkg_mutation_cli(3,argv,&px->policy.catalog,&backend);
}
static void *log_writer(void *opaque){
    file_t *f=opaque;uint8_t bytes[4096];memset(bytes,0x5a,sizeof bytes);
    for(unsigned i=0;i<32;i++)assert(vfs_write(f,bytes,sizeof bytes)==sizeof bytes);
    return NULL;
}
static int executor_open(block_dev_t *dev){
    ext4_mount_t *bootstrap=kcalloc(1,sizeof *bootstrap);e4_journal_guard_t *guard=kcalloc(1,sizeof *guard);assert(bootstrap&&guard);
    bootstrap->dev=dev;bootstrap->bs=512;bootstrap->blocks=dev->sector_count;bootstrap->journal_bootstrap=true;guard->lifecycle=true;
    uint32_t *map=NULL;jbd2_source_t source;jbd2_plan_t *plan=NULL;int r=e4_journal_source(bootstrap,guard,&map,&source);
    if(!r)r=jbd2_analyze(dev,&source,&plan,NULL);
    kfree(map);e4_discard(bootstrap);kfree(guard);
    ext4_engine_t *preview=NULL;block_dev_t projected={.sector_size=512,.sector_count=dev->sector_count,.read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=plan};
    if(!r)r=e4_open_journal_orphans_owned(&projected,true,&preview,false,true);
    if(!r){preview->fs->rw_map=kmalloc(sizeof(e4_map_t)+E4_MAP_MAX*sizeof(e4_extent_t));assert(preview->fs->rw_map);preview->fs->engine=preview;px->fs=preview->fs;r=px_validate(preview->fs);if(!r&&px->state.phase)r=px_rederive(px->state.operation,true);}
    if(r)fprintf(stderr,"executor projected admission error=%d\n",r);
    if(preview)ext4_engine_close(preview);
    px->fs=NULL;
    if(!r)r=jbd2_replay(plan,true);
    jbd2_release(plan);if(r)return r;
    ext4_engine_t *e=NULL;r=e4_open_journal_orphans_owned(dev,true,&e,false,true);if(r)return r;
    e->fs->rw_map=kmalloc(sizeof(e4_map_t)+E4_MAP_MAX*sizeof(e4_extent_t));assert(e->fs->rw_map);e->fs->engine=e;px->fs=e->fs;
    r=px_validate(e->fs);if(r){ext4_engine_close(e);px->fs=NULL;return r;}
    e4_node_t *root=kcalloc(1,sizeof *root);assert(root);e4_inode_t in;assert(!e4_inode(e->fs,2,&in));
    px_copy(root->node.name,"mnt");px_copy(root->node.path,"/mnt");root->node.parent=g_vfs_root;e4_setup(root,&in);root->node.next=g_vfs_root->children;g_vfs_root->children=&root->node;
    e->fs->cached[e->fs->nodes++]=root;e->fs->journal_mounted=true;e->fs->detached=true;e4_active=e->fs;e4_engine_busy=false;
    uint64_t flags=spin_lock_irqsave(&e4_lock);r=e->dirty?0:e4_journal_mark(e,false);spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static void executor_close(void){
    if(px&&px->fs){ext4_engine_t *e=px->fs->engine;px->fs->journal_mounted=false;e4_active=NULL;g_vfs_root->children=NULL;ext4_engine_close(e);px->fs=NULL;}
    px_destroy();if(g_vfs_root){g_vfs_root->children=NULL;kfree(g_vfs_root);g_vfs_root=NULL;}assert(!live&&!e4_engine_busy);
}
static int begin_profile(px_policy *p,const uint8_t *artifact,size_t artifact_n,const uint8_t *catalog,size_t catalog_n,block_dev_t *dev){
    int r=px_prepare(p,artifact,artifact_n,catalog,catalog_n,catalog+73,catalog[72],true);if(r){fprintf(stderr,"executor source preparation error=%d\n",r);return r;}vfs_init();return executor_open(dev);
}
static void dump(const char *path,const uint8_t *bytes,size_t n){FILE *f=fopen(path,"wbx");assert(f&&fwrite(bytes,1,n,f)==n&&!fclose(f));}
int main(int argc,char **argv){
    assert(argc==9);size_t n,artifact_n,catalog_n,policy_n,config_n,key_n;uint8_t *initial=load(argv[1],&n),*artifact=load(argv[2],&artifact_n),*catalog=load(argv[3],&catalog_n),*policy_bytes=load(argv[4],&policy_n),*config=load(argv[5],&config_n),*key=load(argv[8],&key_n);assert(key_n==32);
    assert(n==32*1024*1024&&policy_n==sizeof(fpk_catalog_policy)&&config_n==296);
    px_policy p={0};memcpy(&p.catalog,policy_bytes,policy_n);memcpy(p.filesystem,config,16);
    for(unsigned i=0;i<8;i++){p.anchors[i].inode=px_get(config+16+16*i,4);p.anchors[i].generation=px_get(config+20+16*i,4);p.anchors[i].physical=px_get(config+24+16*i,8);}
    p.minimum_sequence=px_get(config+144,8);memcpy(p.minimum_hash,config+152,32);memcpy(p.layout,config+184,32);
    p.resources.capacity[0]=px_get(config+216,8);p.resources.capacity[1]=px_get(config+224,8); /* final 64 bytes reserved */
    fpk_span names[]={{(const uint8_t *)"shell",5},{(const uint8_t *)"sudo",4},{(const uint8_t *)"pkg",3},{(const uint8_t *)"echo",4}};
    p.resources.block_size=4096;p.resources.journal_images=64;p.resources.orphan_slots=16;p.resources.memory_bytes=32*1024*1024;p.resources.base=2;p.resources.base_names=names;p.resources.base_count=4;for(unsigned i=0;i<32;i++)p.resources.platform[i]=i;
    p.current_actor=current_actor;p.authorized=authorized;memcpy(p.record_key,key,32);memset(key,0,32);free(key);
    ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable&&d.volatile_bytes);memcpy(d.stable,initial,n);memcpy(d.volatile_bytes,initial,n);disk_observed=&d;
    block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
    if(!strcmp(argv[7],"-4")){
        assert(begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev)==-VFS_EACCES&&!d.events);executor_close();
        crypto_wipe(p.record_key,32);free(d.stable);free(d.volatile_bytes);free(initial);free(artifact);free(catalog);free(policy_bytes);free(config);puts("PASS foreign owned-directory entry refused before replay/writes");return 0;
    }
    p.record_key[0]^=1;assert(begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev)==-VFS_EACCES&&!d.events);executor_close();p.record_key[0]^=1;
    p.minimum_sequence++;assert(begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev)==-VFS_EACCES&&!d.events);executor_close();p.minimum_sequence--;
    size_t tamper=(size_t)p.anchors[0].physical*4096+24;d.stable[tamper]^=1;d.volatile_bytes[tamper]^=1;
    assert(begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev)==-VFS_EACCES&&!d.events);executor_close();d.stable[tamper]^=1;d.volatile_bytes[tamper]^=1;
    assert(!memcmp(d.stable,initial,n));puts("PASS wrong key, external rollback floor and ledger tamper refused before writes");
    int r=begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev);assert(!r);
    if(!strcmp(argv[7],"-3")){
        size_t before=d.events;assert(px_control(PI_INSTALL)&&d.events==before&&!px->state.phase);executor_close();
        crypto_wipe(p.record_key,32);free(d.stable);free(d.volatile_bytes);free(initial);free(artifact);free(catalog);free(policy_bytes);free(config);puts("PASS insufficient capacity rejected before reservation");return 0;
    }
    file_t *log=vfs_open("/mnt/root/ordinary.log",VFS_O_CREAT|VFS_O_RDWR);assert(log);pthread_t writer;assert(!pthread_create(&writer,NULL,log_writer,log));
    assert(cli_run("install","../toolbox")&&mutations==0);
    assert(!cli_run("install","toolbox")&&mutations==1&&px->state.installed&&!px->state.phase);assert(!pthread_join(writer,NULL));uint8_t logged[4096];log->offset=0;
    for(unsigned i=0;i<32;i++){assert(vfs_read(log,logged,sizeof logged)==sizeof logged);for(unsigned j=0;j<sizeof logged;j++)assert(logged[j]==0x5a);}assert(!vfs_close(log));
    uint64_t floor=px->state.floor;assert(floor==73);size_t events=d.events;
    output_failure=true;assert(cli_run("install","toolbox")&&mutations==2&&d.events==events&&px->state.installed);output_failure=false;
    size_t install_events=events;uint8_t *installed=malloc(n);assert(installed);memcpy(installed,d.stable,n);
    assert(!px_control(PI_INSTALL)&&events==d.events);assert(!px_control(0)&&events==d.events);
    char executable[256];snprintf(executable,sizeof executable,"/mnt%s",px->files[0]);file_t *pin=vfs_open(executable,VFS_O_RDONLY);assert(pin);
    assert(cli_run("remove","toolbox")&&px->state.phase==PX_DELETE);uint8_t b[128];long got=vfs_read(pin,b,sizeof b);assert(got>0&&!memcmp(b,px->payload+px->offsets[px_member(16)],(size_t)got));assert(!vfs_close(pin));assert(!px_control(0)&&!px->state.installed&&!px->state.phase&&px->state.floor==floor);
    assert(!px_control(PI_INSTALL)&&px->state.installed);assert(!ext4_freeze_and_sync(px->fs));dump(argv[6],d.stable,n);printf("PASS actual install/remove/reinstall files=%u sequence=%llu events=%zu\n",px->shape.file_count,(unsigned long long)px->state.sequence,d.events);
    executor_close();
    unsigned phases[]={PX_BUILD,PX_COMMIT,PX_RETIRE,PX_DELETE};
    for(unsigned i=0;i<sizeof phases/sizeof *phases;i++){
        memcpy(d.stable,phases[i]==PX_DELETE?installed:initial,n);ext4_fault_restart(&d);assert(!begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev));
        deny_phase=(int)phases[i];r=px_control(phases[i]==PX_DELETE?PI_REMOVE:PI_INSTALL);assert(r==-VFS_EACCES&&px->state.phase==phases[i]);
        events=d.events;permitted=false;deny_phase=-1;assert(px_control(0)==-VFS_EACCES&&d.events==events);executor_close();permitted=true;ext4_fault_restart(&d);
        assert(!begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev));assert(!px_control(0)&&!px->state.phase);
        assert(px->state.installed==(phases[i]==PX_COMMIT||phases[i]==PX_RETIRE));assert(!ext4_freeze_and_sync(px->fs));executor_close();
    }
    puts("PASS authority loss at four durable phases with fresh-owner recovery");
    long cut=strtol(argv[7],NULL,10);
    if(cut>=0){
        size_t cuts[]={0,1,2,20,(size_t)cut,100,install_events/8,install_events/3,install_events/2,install_events*3/4,install_events-2,install_events-1,install_events};
        for(unsigned operation=PI_INSTALL;operation<=PI_REMOVE;operation++)for(unsigned c=0;c<sizeof cuts/sizeof *cuts;c++){
            memcpy(d.stable,operation==PI_INSTALL?initial:installed,n);ext4_fault_restart(&d);assert(!begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev));size_t base=d.events;d.cut=(long)base+(long)cuts[c];
            r=px_control(operation);assert(r||d.events<=base+cuts[c]);executor_close();ext4_fault_restart(&d);
            assert(!begin_profile(&p,artifact,artifact_n,catalog,catalog_n,&dev));assert(!px_control(0));events=d.events;assert(!px_control(0)&&events==d.events);
            assert(!px->state.phase);assert(!ext4_freeze_and_sync(px->fs));executor_close();
        }
        puts("PASS 26 fresh-owner interruption recoveries and repeat");
    }
    crypto_wipe(p.record_key,32);free(installed);free(d.stable);free(d.volatile_bytes);free(initial);free(artifact);free(catalog);free(policy_bytes);free(config);return 0;
}
