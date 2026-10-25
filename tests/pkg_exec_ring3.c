#include "syscall_abi.h"
#include "src/fs/vfs.h"
#include "src/drivers/power.h"
#include "tests/pkg_exec_wire.h"
#include "user/pkg/mutation_cli.h"
#include "tests/pkg_exec_cli.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){__asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");return n;}
static void need(bool ok,unsigned line){if(ok)return;const char s[]="PKGEXEC RING3 FAIL ";char b[4]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};raw(SYS_WRITE,2,(uintptr_t)s,sizeof s-1);raw(SYS_WRITE,2,(uintptr_t)b,4);raw(SYS_EXIT,1,0,0);__builtin_trap();}
#define check(x) need((x),__LINE__)
static long call(unsigned op,uint64_t session,pb_result *r){pb_request q={.size=32,.version=1,.operation=op,.session=session};return raw(200,(uintptr_t)&q,(uintptr_t)r,32);}
static long mutate(unsigned op,uint64_t session,pb_result *r){mx_request q={.size=64,.version=1,.operation=op,.session=session,.package="field-kit"};return raw(203,(uintptr_t)&q,(uintptr_t)r,64);}
static int cli_mutate(void *context,bool remove,const char *name,size_t n){
    mx_request q={.size=64,.version=1,.operation=remove?2:1,.session=*(uint64_t *)context};pb_result r;
    if(n>32)return -1;
    for(size_t i=0;i<n;i++)q.package[i]=name[i];
    return (int)raw(203,(uintptr_t)&q,(uintptr_t)&r,64);
}
static int cli_run(char *verb,uint64_t session){pkg_mutation_backend b={.transactions={.context=&session},.mutate=cli_mutate};char *args[]={"pkg",verb,"field-kit"};return pkg_mutation_cli(3,args,NULL,&b);}
static void cli_command(const char *verb,const char *name,uint64_t expected){
    const char *args[]={"/bin/pkg",verb,name,0};
    spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};
    long child=raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);check(child>0);
    uint64_t status;check(raw(SYS_WAITPID,child,(uintptr_t)&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==expected);
}
static bool equal(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void delay(void){timespec_t t={.tv_nsec=10000000};check(!raw(SYS_NANOSLEEP,(uintptr_t)&t,0,0));}
static long spawn(const char *mode){const char *args[]={"/bin/pkg-boot",mode,0};spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};return raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);}
static void wait_ready(void){for(unsigned i=0;i<3000;i++){if(raw(201,31,0,0)==1)return;delay();}check(false);}
static void mark(const char *s){unsigned n=0;while(s[n])n++;check(raw(SYS_WRITE,1,(uintptr_t)s,n)==n);}
int pkg_call_main(int argc,char **argv){
    pb_result r;
    if(argc>1&&equal(argv[1],"--hostkey")){
        static char key[1024];long n=raw(SYS_HOSTKEY,0,(uintptr_t)key,sizeof key);
        for(unsigned i=0;i<sizeof key;i++)((volatile char *)key)[i]=0;
        return n>0?0:n<0?(int)-n:255;
    }
    if(argc>1&&equal(argv[1],"--poweroff")){raw(SYS_REBOOT,REBOOT_CMD_POWEROFF,0,0);return 1;}
    if(argc>1&&equal(argv[1],"--pause"))return raw(201,30,0,0)?1:0;
    if(argc>1&&equal(argv[1],"--release"))return raw(201,32,0,0)?1:0;
    if(argc>1&&equal(argv[1],"--waiting"))return raw(201,31,0,0)==1?0:1;
    if(argc>1&&equal(argv[1],"--pkg"))return pkg_private_cli(argc-1,argv+1);
    if(argc>1&&!equal(argv[1],"--installed")){
        if(equal(argv[1],"misuse")){check(mutate(1,1,&r)==SYSCALL_EACCES);check(call(PB_ACQUIRE,0,&r)<0);return 0;}
        check(!call(PB_ACQUIRE,0,&r));uint64_t s=r.session;bool recovering=equal(argv[1],"recover-kill")||equal(argv[1],"recover-drop");long result=recovering?call(PB_RECOVER,s,&r):mutate(1,s,&r);
        check((equal(argv[1],"drop")||equal(argv[1],"recover-drop"))&&result==SYSCALL_EACCES);check(!call(PB_RELEASE,s,&r));return 0;
    }
    check(raw(203,0,0,63)==SYSCALL_EINVAL);check(raw(203,0,0,64)==SYSCALL_EFAULT);
    check(!raw(201,35,0,0));
    check(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;check(session&&r.phase==PKG_TX_IDLE);
    timespec_t saved,expired={.tv_sec=1793102400};check(!raw(SYS_CLOCK_GETTIME,CLOCK_REALTIME,(uintptr_t)&saved,0));
    long before_writes=raw(201,13,0,0);check(!raw(SYS_CLOCK_SETTIME,CLOCK_REALTIME,(uintptr_t)&expired,0));check(mutate(1,session,&r)==SYSCALL_EACCES&&raw(201,13,0,0)==before_writes);check(!raw(SYS_CLOCK_SETTIME,CLOCK_REALTIME,(uintptr_t)&saved,0));
    mark("PKGEXEC phase acquired\n");
    long child=spawn("misuse");check(child>0);uint64_t child_status;check(raw(SYS_WAITPID,child,(uintptr_t)&child_status,0)==child&&WIFEXITED(child_status)&&!WEXITSTATUS(child_status));
    mx_request absent={.size=64,.version=1,.operation=1,.session=session,.package="unknown-package"};
    long source_writes=raw(201,13,0,0);
    check(raw(203,(uintptr_t)&absent,(uintptr_t)&r,64)==SYSCALL_EACCES&&raw(201,13,0,0)==source_writes);
    absent.package[0]='/';check(raw(203,(uintptr_t)&absent,(uintptr_t)&r,64)==SYSCALL_EINVAL&&raw(201,13,0,0)==source_writes);
    check(mutate(1,session+1,&r)==SYSCALL_EACCES);check(!raw(201,4,0,0));check(mutate(1,session,&r)==SYSCALL_EFAULT);check(!call(PB_STATUS,0,&r));uint64_t sequence=r.sequence;
    check(!raw(201,4,0,0));check(mutate(1,session,&r)==SYSCALL_EFAULT);check(!call(PB_STATUS,0,&r)&&r.sequence==sequence);
    long fd=raw(SYS_OPEN,(uintptr_t)"/usr/bin/field-tool",VFS_O_RDONLY,0);check(fd>=0);check(mutate(2,session,&r)==SYSCALL_EAGAIN);
    char elf[4];check(raw(SYS_READ,fd,(uintptr_t)elf,4)==4&&elf[0]==0x7f&&elf[1]=='E'&&elf[2]=='L'&&elf[3]=='F');check(!raw(SYS_CLOSE,fd,0,0));
    check(!raw(201,4,0,0));check(call(PB_RECOVER,session,&r)==SYSCALL_EFAULT);check(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE);sequence=r.sequence;
    long recovery_writes=raw(201,13,0,0);check(!call(PB_RECOVER,session,&r)&&r.sequence==sequence&&raw(201,13,0,0)==recovery_writes);
    check(!raw(201,4,0,0));check(cli_run("install",session)!=0);check(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE&&r.sequence>sequence);
    sequence=r.sequence;long writes=raw(201,13,0,0);check(!cli_run("install",session));check(!call(PB_STATUS,0,&r)&&r.sequence==sequence&&raw(201,13,0,0)==writes);check(!call(PB_RELEASE,session,&r));
    mark("PKGEXEC phase ordinary lifecycle complete\n");
    for(unsigned loss=0;loss<2;loss++){
        check(!call(PB_ACQUIRE,0,&r));session=r.session;check(!cli_run("remove",session));check(!call(PB_RELEASE,session,&r));
        check(!raw(201,30,0,0));child=spawn(loss?"drop":"kill");check(child>0);wait_ready();check(raw(201,33,0,0)==child);check(call(PB_STATUS,0,&r)==SYSCALL_EAGAIN);check(call(PB_ACQUIRE,0,&r)<0);
        if(loss)check(!raw(201,34,0,0));else{check(!raw(SYS_KILL,child,SIGKILL,0));check(!raw(201,32,0,0));}
        check(raw(SYS_WAITPID,child,(uintptr_t)&child_status,0)==child);
        check(loss?(WIFEXITED(child_status)&&!WEXITSTATUS(child_status)):(WIFSIGNALED(child_status)&&WTERMSIG(child_status)==SIGKILL));
        if(!loss)for(unsigned recovering_loss=0;recovering_loss<2;recovering_loss++){
            check(!raw(201,30,0,0));child=spawn(recovering_loss?"recover-drop":"recover-kill");check(child>0);wait_ready();check(raw(201,33,0,0)==child);check(call(PB_STATUS,0,&r)==SYSCALL_EAGAIN);
            if(recovering_loss)check(!raw(201,34,0,0));else{check(!raw(SYS_KILL,child,SIGKILL,0));check(!raw(201,32,0,0));}
            check(raw(SYS_WAITPID,child,(uintptr_t)&child_status,0)==child);check(recovering_loss?(WIFEXITED(child_status)&&!WEXITSTATUS(child_status)):(WIFSIGNALED(child_status)&&WTERMSIG(child_status)==SIGKILL));
        }
        check(!call(PB_ACQUIRE,0,&r));session=r.session;check(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE);check(!call(PB_RELEASE,session,&r));
        mark(loss?"PKGEXEC phase credential-loss recovered\n":"PKGEXEC phase signal/reaper recovered\n");
    }
    cli_command("--help",NULL,0);cli_command("status",NULL,0);cli_command("recover",NULL,0);
    cli_command("install","unknown-package",1);cli_command("install","../field-kit",1);
    cli_command("install","field-kit",0);cli_command("remove","field-kit",0);cli_command("install","field-kit",0);
    mark("PKGEXEC private command sessions PASS\n");
    for(unsigned i=0;i<3000&&raw(201,2,0,0)<5;i++)delay();
    check(raw(201,1,0,0)>=5&&raw(201,2,0,0)>=5);
    const char pass[]="PKGEXEC RING3 PASS install remove recover copyout open-executable\n";raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);
    raw(SYS_REBOOT,REBOOT_CMD_POWEROFF,0,0);check(false);return 1;
}
