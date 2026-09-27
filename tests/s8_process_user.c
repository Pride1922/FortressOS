#include "syscall_abi.h"
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c) : "rcx","r11","memory","cc");
    return nr;
}
static void check(bool ok,unsigned line) {
    if(ok)return;
    const char msg[]="S8 USER FAIL ";
    char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    call(SYS_WRITE,2,(uintptr_t)msg,sizeof(msg)-1); call(SYS_WRITE,2,(uintptr_t)n,sizeof(n));
    call(SYS_EXIT,1,0,0); __builtin_trap();
}
#define require(x) check((x),__LINE__)
static const char *args[]={"/bin/shell","child",0};
static spawn_opts_t opts;
static long spawn(unsigned flags,uint64_t pgid) {
    opts=(spawn_opts_t){.size=64,.version=2,.flags=flags,.reserved2=pgid,.argv=(uintptr_t)args};
    return call(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
}
static const uint64_t ro_status=0;
void shell_main(int argc,const char **argv) {
    if(argc>1) {
        if(argv[1][0]=='o') { require(spawn(SPAWN_SETPGROUP|SPAWN_STAGED,0)>0); }
        call(SYS_EXIT,137,0,0); return;
    }
    uint64_t status=0xdead;
    long own=call(SYS_GETPGRP,0,0,0);
    require(own>0);
    require(call(SYS_SETPGID,0,0,0)==SYSCALL_EPERM);
    long a=spawn(SPAWN_SETPGROUP|SPAWN_STAGED,0); require(a>0);
    long b=spawn(SPAWN_SETPGROUP|SPAWN_STAGED,a); require(b>0);
    require(call(SYS_WAITPID,a,(uintptr_t)&status,WNOHANG)==0 && status==0xdead);
    require(call(SYS_WAITPID,a,(uintptr_t)&ro_status,WNOHANG)==SYSCALL_EFAULT);
    require(call(SYS_WAITPID,INT64_MIN,0,0)==SYSCALL_EINVAL);
    require(call(SYS_WAITPID,-1,0,16)==SYSCALL_EINVAL);
    require(call(SYS_SETPGID,b,b,0)==0);
    require(call(SYS_SETPGID,b,a,0)==0);
    require(call(SYS_GROUP_RELEASE,a,GROUP_RELEASE,0)==0);
    long first=call(SYS_WAITPID,-a,(uintptr_t)&status,0);
    require((first==a || first==b) && status==0x8900);
    long second=call(SYS_WAITPID,-a,(uintptr_t)&status,WUNTRACED|WCONTINUED);
    require(second>0 && second!=first && status==0x8900);
    require(call(SYS_WAITPID,-a,0,WNOHANG)==SYSCALL_ECHILD);
    a=spawn(0,0); require(a>0);
    require(call(SYS_WAIT,a,(uintptr_t)&status,~0ULL)==0 && status==137);
    a=spawn(SPAWN_SETPGROUP|SPAWN_STAGED,0); require(a>0);
    require(call(SYS_GROUP_RELEASE,a,GROUP_CANCEL,0)==0);
    require(call(SYS_WAITPID,a,0,0)==SYSCALL_ECHILD);
    opts.version=1; require(call(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64)==SYSCALL_EINVAL);
    for(unsigned i=0;i<80;i++) { /* Parent death must cancel its never-run child. */
        args[1]="orphan"; a=spawn(0,0); require(a>0);
        require(call(SYS_WAITPID,a,(uintptr_t)&status,0)==a && status==0x8900);
    }
    args[1]="child";
    /* Exhaust staged process/stack capacity, then recover it. */
    a=spawn(SPAWN_SETPGROUP|SPAWN_STAGED,0); require(a>0);
    unsigned count=1;
    while(count<64) { b=spawn(SPAWN_SETPGROUP|SPAWN_STAGED,a); if(b<0)break; count++; }
    require(b==SYSCALL_ENOMEM && count>1 && count<64);
    require(call(SYS_GROUP_RELEASE,a,GROUP_CANCEL,0)==0);
    a=spawn(0,0); require(a>0);
    require(call(SYS_WAITPID,0,(uintptr_t)&status,0)==a && status==0x8900);
    const char pass[]="S8 USER PASS\n";
    call(SYS_WRITE,1,(uintptr_t)pass,sizeof(pass)-1);
}
