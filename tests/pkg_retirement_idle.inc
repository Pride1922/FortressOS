#include "crypto.h"
/* Private finite completion profile. Signed hashes authorize exact record sets;
 * actual filesystem identities, ownership and refunds are checked independently.
 * No production caller, signer, trust provisioning or general package executor. */
#define XI_CERT_BODY 1536u
static const char *xi_cert_path="/mnt/pkg/completion.cert";
static uint8_t xi_cert[1728];
static xr_bundle xi_current,xi_next;
static bool xi_manual,xi_terminal_pause;
static unsigned xi_commits,xi_max;
static const char *xi_names[]={"/mnt/pkg/transaction.v1/artifact.fpk","/mnt/usr/bin/demo","/mnt/pkg/transaction.v1/next.v1","/mnt/pkg/transaction.v1/retired.v1"};
static void xi_digest(const xr_bundle *b,uint8_t h[32]){
 digest_t d;digest_init(&d,DIGEST_SHA256);
 assert(digest_update(&d,b->g,1024)&&digest_update(&d,b->t,8192)&&digest_update(&d,b->l,1024)&&digest_update(&d,b->a,1024)&&digest_update(&d,b->o,1024));digest_final(&d,h);
}
static void xi_export(ext4_mount_t *fs,const char *prefix){
 uint32_t ino;assert(!ad_path(fs,xi_cert_path,&ino));e4_inode_t in;assert(!e4_inode(fs,ino,&in));fs->rw_ino=0;assert(!e4_mapping(&in)&&in.map->count<=4);
 uint8_t *p=xi_locator;memset(p,0,128);e4_p16(p,5);e4_p16(p+2,1);e4_p32(p+4,ino);e4_p32(p+8,in.generation);e4_p32(p+12,0600);ad_p64(p+16,in.size);e4_p16(p+32,in.map->count);
 uint64_t blocks=0;for(unsigned i=0;i<in.map->count;i++){e4_extent_t x=in.map->entries[i];e4_p32(p+40+16*i,x.logical);e4_p32(p+44+16*i,x.len);ad_p64(p+48+16*i,x.physical);blocks+=x.len;}ad_p64(p+24,blocks);save(prefix,"completion-locator",p,128);
}
#include "pkg_private_completion.inc"
static void xi_pressure(ext4_engine_t *e){
 size_t events=ad_disk->events;uint64_t irq=spin_lock_irqsave(&e4_lock);vfs_create_attrs_t attrs={.mode=0600};uint32_t ino=0;int r=ext4_engine_namespace_create_attrs(e,2,"idle-competing",false,&attrs,&ino);spin_unlock_irqrestore(&e4_lock,irq);assert(r==-VFS_ENOSPC&&ad_disk->events==events&&!e->tainted);
 int error;vfs_node_t *n=e4_ac_lookup(sv_fs,"/pkg-competing",&error);assert(n);uint64_t off=sv_fs->bs;uint8_t byte='X';assert(n->write(n,&off,false,&byte,1)==-VFS_ENOSPC&&ad_disk->events==events&&!e->tainted);vfs_node_put(n);puts("PASS competing block/inode allocations refused before I/O");
}
static void xi_ordinary(void){
 uint64_t irq=spin_lock_irqsave(&e4_lock);vfs_create_attrs_t attrs={.mode=0600};uint32_t ino;assert(!ext4_engine_namespace_create_attrs(sv_fs->engine,2,"after-idle",false,&attrs,&ino));spin_unlock_irqrestore(&e4_lock,irq);
 int error;vfs_node_t *n=e4_ac_lookup(sv_fs,"/after-idle",&error);assert(n);uint64_t off=0;const uint8_t bytes[]="fresh baseline allocation";assert(n->write(n,&off,false,bytes,sizeof bytes)==sizeof bytes);vfs_node_put(n);uint64_t lock=spin_lock_irqsave(&e4_lock);e4_inode_t in;assert(!e4_inode(sv_fs,ino,&in));bool reused=false;for(unsigned i=0;i<4;i++)if(i!=1&&ino==e4_u32(xi_cert+772+64*i)){assert(in.generation>e4_u32(xi_cert+776+64*i));reused=true;}spin_unlock_irqrestore(&e4_lock,lock);assert(reused);puts("PASS ordinary inode and block allocation after IDLE; retired inode reused with newer generation");
}
int main(int argc,char **argv){
 if(argc==5||!strncmp(argv[1],"activate",8))return xi_previous_main(argc,argv);
 ng_config();setvbuf(stdout,NULL,_IONBF,0);assert(argc==7);const char *mode=argv[1],*prefix=argv[6];size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=strtoul(argv[3],NULL,10);
 ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};ad_disk=&d;block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};reset(&d,initial);ad_auth(argv[4],argv[5]);
 bool cut=xr_equals(mode)!=NULL||!strcmp(mode,"terminal-pause");xr_arm(&d,mode);xi_terminal_pause=!strcmp(mode,"terminal-pause");if(xi_terminal_pause)ng_pause=false;const char *where=NULL;int r=ext4_prepare_root_diagnose(&dev,true,&sv_fs,&where);
 if(!strcmp(mode,"reject")){assert(r&&!sv_fs&&!e4_engine_busy&&!d.events&&!memcmp(initial,d.stable,size));printf("PASS reject %s writes=0 flushes=0\n",where);goto done;}
 if(!r&&!strcmp(mode,"pressure"))xi_pressure(sv_fs->engine);
 if(!r)r=xi_finish(sv_fs->engine);
 if(!r)r=accounts_prepare(sv_fs);
 if(r&&cut){assert((!enabled&&!writer)||(sv_fs&&sv_fs->engine->tainted)||!sv_fs);}
 if(!r&&!strcmp(mode,"ordinary"))xi_ordinary();
 if(!r&&strncmp(mode,"recover",7)==0){assert(xi_stage>=11);}
 if(!r&&!strcmp(mode,"fresh")){
  assert(enabled);creds_t root;creds_init_root(&root);account_request_t req={.size=64,.version=1};for(unsigned lifetime=0;lifetime<3;lifetime++){req=(account_request_t){.size=64,.version=1};assert(!accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&root,77)&&writer==77&&req.token);
  size_t events=d.events;account_request_t second={.size=64,.version=1};assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&second,sizeof second,&root,88)==SYSCALL_EAGAIN&&d.events==events);
  assert(accounts_control(ACCOUNT_FINISH,(uintptr_t)&req,sizeof req,&root,88)!=0&&d.events==events&&writer==77);assert(!accounts_control(ACCOUNT_FINISH,(uintptr_t)&req,sizeof req,&root,77)&&!writer);}puts("PASS fresh account writer; competing owner refused; three owner finishes durable");
 }
 if(!r&&!strcmp(mode,"death")){
  creds_t root;creds_init_root(&root);account_request_t req={.size=64,.version=1};assert(!accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&root,77));size_t events=d.events;accounts_process_exit(88);assert(writer==77&&d.events==events);accounts_process_exit(77);assert(!writer&&!enabled&&d.events==events);account_request_t retry={.size=64,.version=1};assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&retry,sizeof retry,&root,88)==SYSCALL_EAGAIN&&d.events==events);puts("PASS process-owner exit fences admission until durable recovery");
 }
 if(r){if(!cut)fprintf(stderr,"XI failure r=%d where=%s stage=%u events=%zu\n",r,where?where:"operation",xi_stage,d.events);assert(cut&&r==-VFS_EIO);save(prefix,"interrupted",d.stable,size);printf("PASS interrupted events=%zu stage=%u\n",d.events,xi_stage);}
 else{uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_validate(sv_fs->engine));spin_unlock_irqrestore(&e4_lock,irq);assert(xi_stage>=11);printf("PASS IDLE events=%zu stage=%u commits=%u images=%u\n",d.events,xi_stage,xi_commits,xi_max);assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);}
 done:xr_active=ad_enabled=ng_required=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
