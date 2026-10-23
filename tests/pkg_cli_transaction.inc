/* Actual CLI entry + private actual EXT4/JBD2 backend. Transport mocks only
 * userspace stdout/stderr; no install interface, device paths or trust options. */
#include "user/tools/common.h"
static block_dev_t *qt_device;
static pkg_transaction_status qt_snapshot;
static unsigned qt_status_calls,qt_recover_calls;
static int qt_backend_error;
static bool qt_fail_output;
static unsigned qt_bad_case;
static char qt_output[4096];
static size_t qt_output_n;
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){
 assert(nr==SYS_WRITE&&(a==1||a==2));
 if(qt_fail_output)return SYSCALL_EIO;
 size_t n=c>7?7:c;assert(n&&qt_output_n+n<sizeof qt_output);memcpy(qt_output+qt_output_n,(void *)b,n);qt_output_n+=n;qt_output[qt_output_n]=0;return (long)n;
}
static void qt_capture(void){
 qt_snapshot=(pkg_transaction_status){.phase=e4_u16(xi_current.g+22),.sequence=ad_u64(xi_current.g+24),
  .account_sequence=ad_u64(xi_current.a+24),.credential_generation=ad_u64(xi_current.a+144)};
 for(unsigned i=0;i<2;i++){qt_snapshot.remaining[i]=ad_u64(xi_current.g+312+8*i);qt_snapshot.charged[i]=ad_u64(xi_current.g+328+8*i);}
}
static int qt_status(void *context,pkg_transaction_status *out){
 assert(context==qt_device);qt_status_calls++;size_t events=ad_disk->events;int r;
 if(sv_fs){uint64_t irq=spin_lock_irqsave(&e4_lock);r=ad_validate(sv_fs->engine);if(!r)qt_capture();spin_unlock_irqrestore(&e4_lock,irq);}
 else{
  qt_observe=true;qt_observed=false;const char *where=NULL;r=ext4_prepare_root_diagnose(qt_device,true,&sv_fs,&where);qt_observe=false;
  if(r==-VFS_EAGAIN&&qt_observed)r=0;
  assert(!sv_fs&&!e4_engine_busy);
 }
 assert(ad_disk->events==events);
 if(!r)*out=qt_snapshot;
 return r;
}
static int qt_recover(void *context,pkg_transaction_status *out){
 assert(context==qt_device);qt_recover_calls++;int r=0;
 if(!sv_fs){const char *where=NULL;r=ext4_prepare_root_diagnose(qt_device,true,&sv_fs,&where);}
 if(!r)r=xi_finish(sv_fs->engine);
 if(!r)r=accounts_prepare(sv_fs);
 if(!r){uint64_t irq=spin_lock_irqsave(&e4_lock);r=ad_validate(sv_fs->engine);if(!r)qt_capture();spin_unlock_irqrestore(&e4_lock,irq);}
 qt_backend_error=r;if(!r)*out=qt_snapshot;return r;
}
static int qt_bad_snapshot(void *context,pkg_transaction_status *out){
 (void)context;qt_status_calls++;*out=(pkg_transaction_status){.phase=PKG_TX_IDLE,.sequence=1,.account_sequence=1,.credential_generation=1};
 if(qt_bad_case==0)out->phase=99;
 if(qt_bad_case==1)out->remaining[0]=1;
 if(qt_bad_case==2){out->phase=PKG_TX_RETIRING;out->remaining[0]=UINT64_MAX;out->charged[0]=1;}
 if(qt_bad_case==3)out->sequence=0;
 return qt_bad_case==4?-VFS_EIO:0;
}
static int qt_bad_recover(void *context,pkg_transaction_status *out){(void)context;qt_recover_calls++;*out=(pkg_transaction_status){.phase=PKG_TX_RETIRING,.sequence=1,.account_sequence=1,.credential_generation=1};return 0;}
int main(int argc,char **argv){
 if(argc==5||!strncmp(argv[1],"activate",8)||!strcmp(argv[1],"terminal-pause"))return qt_previous_main(argc,argv);
 ng_config();setvbuf(stdout,NULL,_IONBF,0);assert(argc==7);const char *mode=argv[1],*prefix=argv[6];size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=strtoul(argv[3],NULL,10);
 ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};ad_disk=&d;block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};qt_device=&dev;reset(&d,initial);ad_auth(argv[4],argv[5]);xr_arm(&d,mode);
 pkg_transaction_backend backend={&dev,qt_status,qt_recover};char *status[]={"pkg","status",NULL},*recover[]={"pkg","recover",NULL};int r;
 if(!strcmp(mode,"status")||!strcmp(mode,"status-refuse")){
  r=pkg_transaction_cli(2,status,NULL,&backend);assert(qt_status_calls==1&&!qt_recover_calls&&!d.events&&!sv_fs&&!memcmp(initial,d.stable,size));assert(r==(!strcmp(mode,"status-refuse")));if(!r)printf("PASS CLI status phase=%u seq=%llu writes=0 flushes=0\n",qt_snapshot.phase,(unsigned long long)qt_snapshot.sequence);
 }else if(!strcmp(mode,"syntax")){
  char huge[257];memset(huge,'x',256);huge[256]=0;char *args[]={"pkg","recover","--force",NULL};assert(pkg_transaction_cli(3,args,NULL,&backend));args[2]="/dev/sda";assert(pkg_transaction_cli(3,args,NULL,&backend));args[1]="install";args[2]="demo";assert(pkg_transaction_cli(3,args,NULL,&backend));args[1]=huge;assert(pkg_transaction_cli(2,args,NULL,&backend));assert(pkg_transaction_cli(0,NULL,NULL,&backend));assert(pkg_transaction_cli(2,recover,NULL,NULL));assert(pkg_main(2,recover));assert(pkg_main(2,status));char *help[]={"pkg","--help",NULL};assert(!pkg_transaction_cli(2,help,NULL,&backend));assert(!qt_status_calls&&!qt_recover_calls&&!d.events&&!sv_fs);r=0;puts("PASS CLI syntax/default backend refuses before storage");
 }else if(!strcmp(mode,"invalid-result")){
  backend.status=qt_bad_snapshot;
  for(qt_bad_case=0;qt_bad_case<5;qt_bad_case++)assert(pkg_transaction_cli(2,status,NULL,&backend));
  assert(qt_status_calls==5&&!strstr(qt_output,"state:")&&!d.events);backend.recover=qt_bad_recover;assert(pkg_transaction_cli(2,recover,NULL,&backend)&&qt_recover_calls==1&&!strstr(qt_output,"recovery complete:"));backend.status=NULL;assert(pkg_transaction_cli(2,status,NULL,&backend)&&qt_status_calls==5);backend.recover=NULL;assert(pkg_transaction_cli(2,recover,NULL,&backend)&&qt_recover_calls==1);r=0;puts("PASS invalid/failing/missing backend and non-IDLE recovery refuse success output");
 }else{
  qt_fail_output=!strcmp(mode,"output-fail");r=pkg_transaction_cli(2,recover,NULL,&backend);assert(qt_recover_calls==1&&!qt_status_calls);
  if(qt_backend_error){assert(r&&!strstr(qt_output,"recovery complete:"));if(!strcmp(mode,"refuse"))assert(!d.events&&!sv_fs&&!memcmp(initial,d.stable,size));else assert(xr_equals(mode));save(prefix,"interrupted",d.stable,size);printf("PASS CLI interrupted events=%zu commits=%u\n",d.events,xi_commits);}
  else{
   assert(r==qt_fail_output&&qt_snapshot.phase==PKG_TX_IDLE&&qt_snapshot.sequence==12);if(!qt_fail_output)assert(strstr(qt_output,"recovery complete: IDLE"));
   qt_fail_output=false;size_t events=d.events;assert(!pkg_transaction_cli(2,status,NULL,&backend)&&qt_status_calls==1&&d.events==events&&qt_recover_calls==1);printf("PASS CLI recover events=%zu commits=%u sequence=%llu\n",d.events,xi_commits,(unsigned long long)qt_snapshot.sequence);assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);
  }
 }
 printf("CLI-OUTPUT-BEGIN\n%sCLI-OUTPUT-END\n",qt_output);
 xr_active=ad_enabled=ng_required=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
