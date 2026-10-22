#include "syscall_abi.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){
    __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");return n;
}
static void check(bool ok,unsigned line){
    if(ok)return;
    const char text[]="KERNEL FS FAIL ";char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    raw(SYS_WRITE,2,(uintptr_t)text,sizeof text-1);raw(SYS_WRITE,2,(uintptr_t)n,sizeof n);raw(SYS_EXIT,1,0,0);__builtin_trap();
}
#define require(x) check((x),__LINE__)
static volatile unsigned handled;
static void handler(int sig){if(sig==SIGINT)handled++;}
static long spawn(const char *mode){
    const char *args[]={"/bin/pkg-call-kernel",mode,0};spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};
    return raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
}
static void ready(unsigned n){
    for(unsigned i=0;i<100000;i++){if(raw(200,22,0,0)==n)return;raw(200,8,0,0);}require(false);
}
static void reap(long p,unsigned sig){
    uint64_t s=~0ULL;require(raw(SYS_WAITPID,p,(uintptr_t)&s,0)==p);
    require(sig?(WIFSIGNALED(s)&&WTERMSIG(s)==sig):(WIFEXITED(s)&&WEXITSTATUS(s)==0));
}
int pkg_call_main(int argc,char **argv){
    if(argc>1){
        if(argv[1][0]=='h'){
            signal_action_t action={.handler=(uintptr_t)handler};require(!raw(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0));
            require(raw(200,25,0,0)==SYSCALL_EINTR&&handled==1);return 0;
        }
        raw(200,21,0,0);require(false);return 1;
    }
    require(!raw(200,20,0,0));unsigned n=0;
    for(unsigned i=0;i<3;i++){
        long p=spawn("w");require(p>0);ready(++n);
        if(i==2){uint64_t s;require(!raw(SYS_KILL,p,SIGSTOP,0));
            require(raw(SYS_WAITPID,p,(uintptr_t)&s,WUNTRACED)==p&&WIFSTOPPED(s));
            require(raw(200,0,0,0)==1);require(!raw(SYS_KILL,p,SIGCONT,0));}
        unsigned sig=i?SIGTERM:SIGKILL;require(!raw(SYS_KILL,p,sig,0));reap(p,sig);
    }
    long p=spawn("h");require(p>0);ready(++n);require(!raw(SYS_KILL,p,SIGINT,0));reap(p,0);
    long batch[4];for(unsigned i=0;i<4;i++){batch[i]=spawn("w");require(batch[i]>0);}ready(n+4);
    for(unsigned i=0;i<4;i++)require(!raw(SYS_KILL,batch[i],SIGKILL,0));
    for(unsigned i=0;i<4;i++)reap(batch[i],SIGKILL);
    for(unsigned i=0;i<100000&&raw(200,4,0,0)<8;i++)raw(200,8,0,0);
    require(raw(200,4,0,0)==8&&raw(200,7,0,0)==7);require(!raw(200,24,0,0));
    const char pass[]="KERNEL FS PASS children=8 exits=7 reaped=8 cancelled=8 Q=8,1 G=0,0\n";
    raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);return 0;
}
