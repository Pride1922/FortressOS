#include "syscall_abi.h"
#include "account_abi.h"
#include "src/fs/vfs.h"
#include "src/drivers/power.h"
#include "tests/pkg_backend_candidate.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){register uintptr_t fourth __asm__("r10")=0;__asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(fourth):"rcx","r11","memory","cc");return n;}
static unsigned length(const char *p){unsigned n=0;while(p[n])n++;return n;}
static bool equal(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void need(bool ok,unsigned line){if(ok)return;char b[4]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};const char s[]="PKGROOT RING3 FAIL ";raw(SYS_WRITE,2,(uintptr_t)s,sizeof s-1);raw(SYS_WRITE,2,(uintptr_t)b,4);raw(SYS_EXIT,1,0,0);__builtin_trap();}
#define check(x) need((x),__LINE__)
static char p[8192],g[8192],s[8192];
static void write_file(const char *path,const char *bytes){long fd=raw(SYS_OPEN,(uintptr_t)path,VFS_O_WRONLY|VFS_O_CREAT|VFS_O_TRUNC,0600);check(fd>=0);check(raw(SYS_WRITE,fd,(uintptr_t)bytes,length(bytes))==(long)length(bytes));check(!raw(SYS_CLOSE,fd,0,0));}
int pkg_call_main(int argc,char **argv){
    uint32_t uid,euid,suid;check(!raw(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid));
    if(argc>1 && equal(argv[1],"power")){check(!euid);raw(SYS_REBOOT,REBOOT_CMD_POWEROFF,0,0);check(false);}
    account_request_t q={.size=64,.version=1};check(!raw(SYS_ACCOUNTCTL,ACCOUNT_STATUS,(uintptr_t)&q,64)&&q.generation==1);
    long fd=raw(SYS_OPEN,(uintptr_t)"/installed-proof",VFS_O_RDONLY,0);check(fd>=0);char marker[32]={0};check(raw(SYS_READ,fd,(uintptr_t)marker,31)==20);check(equal(marker,"enrolled-root-proof\n"));check(!raw(SYS_CLOSE,fd,0,0));
    q=(account_request_t){.size=64,.version=1,.flags=ACCOUNT_WITH_SHADOW,.passwd=(uintptr_t)p,.group=(uintptr_t)g,.shadow=(uintptr_t)s,.passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=8192};
    if(euid){
        check(uid==1000&&euid==1000&&suid==1000);check(raw(SYS_ACCOUNTCTL,ACCOUNT_SNAPSHOT,(uintptr_t)&q,64)==SYSCALL_EACCES);
        char cwd[128];check(raw(SYS_GETCWD,(uintptr_t)cwd,sizeof cwd,0)>=0&&equal(cwd,"/home/operator"));
        write_file("/home/operator/proof","installed-user-write\n");write_file("/tmp/pkg-proof","volatile-tmp\n");
    }else{
        check(!uid&&!suid);check(!raw(SYS_ACCOUNTCTL,ACCOUNT_SNAPSHOT,(uintptr_t)&q,64)&&q.generation==1&&q.passwd_bytes>33&&q.shadow_bytes>14);
        write_file("/run/pkg-proof","volatile-run\n");
        q=(account_request_t){.size=64,.version=1};check(raw(SYS_ACCOUNTCTL,ACCOUNT_BEGIN,(uintptr_t)&q,64)==SYSCALL_EROFS);
        pb_request request={.size=32,.version=1,.operation=PB_ACQUIRE};pb_result result;
        check(!raw(200,(uintptr_t)&request,(uintptr_t)&result,32)&&result.phase==PKG_TX_IDLE&&result.sequence==7);uint64_t session=result.session;
        request.operation=PB_RECOVER;request.session=session;check(!raw(200,(uintptr_t)&request,(uintptr_t)&result,32)&&result.sequence==7);request.operation=PB_RELEASE;check(!raw(200,(uintptr_t)&request,(uintptr_t)&result,32));
        fd=raw(SYS_OPEN,(uintptr_t)"/bin/login",VFS_O_WRONLY,0);if(fd>=0){check(raw(SYS_WRITE,fd,(uintptr_t)"X",1)<0);check(!raw(SYS_CLOSE,fd,0,0));}
    }
    const char *pass=euid?"PKGROOT USER PASS\n":"PKGROOT ROOT PASS\n";raw(SYS_WRITE,1,(uintptr_t)pass,length(pass));return 0;
}
