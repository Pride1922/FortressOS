#include "syscall_abi.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){
    __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");return n;
}
static void check(bool ok,unsigned line){
    if(ok)return;
    const char text[]="CALL KERNEL FAIL ";char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    raw(SYS_WRITE,2,(uintptr_t)text,sizeof text-1);raw(SYS_WRITE,2,(uintptr_t)n,sizeof n);raw(SYS_EXIT,1,0,0);__builtin_trap();
}
#define require(x) check((x),__LINE__)
static volatile unsigned handled;
static void handler(int sig){if(sig==SIGINT)handled++;}
static long spawn(const char *mode){
    const char *args[]={"/bin/pkg-call-kernel",mode,0};spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};
    return raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
}
static void wait_count(unsigned n){
    for(unsigned i=0;i<100000;i++){if(raw(200,0,0,0)==n)return;raw(200,8,0,0);}require(false);
}
static void reap(long pid,unsigned sig){
    uint64_t status=~0ULL;require(raw(SYS_WAITPID,pid,(uintptr_t)&status,0)==pid);
    require(sig?(WIFSIGNALED(status)&&WTERMSIG(status)==sig):(WIFEXITED(status)&&WEXITSTATUS(status)==0));
}
int pkg_call_main(int argc,char **argv){
    if(argc>1){
        char mode=argv[1][0];
        if(mode=='n'||mode=='q'){require(!raw(200,mode=='q'?3:1,0,0));raw(SYS_EXIT,0,0,0);}
        if(mode=='c'){raw(200,5,0,0);require(false);}
        if(mode=='h'){
            signal_action_t action={.handler=(uintptr_t)handler};require(!raw(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0));
            require(raw(200,2,1,0)==SYSCALL_EINTR&&handled==1);raw(SYS_EXIT,0,0,0);
        }
        raw(200,2,0,0);require(false);return 1;
    }
    unsigned reaped=0,exits=0;
    for(unsigned round=0;round<3;round++){
        long p=spawn("n");require(p>0);reap(p,0);reaped++;exits++;
        p=spawn("q");require(p>0);reap(p,0);reaped++;exits++;
        p=spawn("c");require(p>0);reap(p,SIGKILL);reaped++;exits++;
        p=spawn("h");require(p>0);wait_count(1);require(!raw(SYS_KILL,p,SIGINT,0));reap(p,0);reaped++;
        for(unsigned i=0;i<2;i++){p=spawn("w");require(p>0);wait_count(1);require(!raw(SYS_KILL,p,i?SIGTERM:SIGKILL,0));reap(p,i?SIGTERM:SIGKILL);reaped++;exits++;}
        p=spawn("w");require(p>0);wait_count(1);require(!raw(SYS_KILL,p,SIGSTOP,0));uint64_t status=0;
        require(raw(SYS_WAITPID,p,(uintptr_t)&status,WUNTRACED)==p&&WIFSTOPPED(status));require(raw(200,0,0,0)==1);
        require(!raw(SYS_KILL,p,SIGCONT,0)&&!raw(SYS_KILL,p,SIGTERM,0));reap(p,SIGTERM);reaped++;exits++;
        long batch[4];for(unsigned i=0;i<4;i++){batch[i]=spawn("w");require(batch[i]>0);}wait_count(4);
        for(unsigned i=0;i<4;i++)require(!raw(SYS_KILL,batch[i],SIGKILL,0));
        for(unsigned i=0;i<4;i++){reap(batch[i],SIGKILL);reaped++;exits++;}
        wait_count(0);
        for(unsigned i=0;i<100000&&raw(200,4,0,0)<reaped;i++)raw(200,8,0,0);
        require(raw(200,4,0,0)==reaped&&raw(200,7,0,0)==exits);
    }
    const char pass[]="CALL KERNEL PASS children=33 exits=30 reaped=33 rounds=3\n";
    raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);
    long mask=raw(200,6,0,0);char line[]="CALL KERNEL CPU_MASK=000\n";
    line[21]='0'+mask/100%10;line[22]='0'+mask/10%10;line[23]='0'+mask%10;
    raw(SYS_WRITE,1,(uintptr_t)line,sizeof line-1);return 0;
}
