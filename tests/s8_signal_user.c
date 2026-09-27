#include "syscall_abi.h"
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a),"S"(b),"d"(c) : "rcx","r11","memory","cc");
    return nr;
}
static void check(bool ok,unsigned line) {
    if(ok)return;
    const char msg[]="S8 SIGNAL FAIL ";
    char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    call(SYS_WRITE,2,(uintptr_t)msg,sizeof(msg)-1); call(SYS_WRITE,2,(uintptr_t)n,sizeof(n));
    call(SYS_EXIT,1,0,0); __builtin_trap();
}
#define require(x) check((x),__LINE__)
static char data[16384];
static long spawn(const char *mode,unsigned flags,uint64_t pgid) {
    const char *args[]={"/bin/shell",mode,0};
    spawn_opts_t opts={.size=64,.version=2,.flags=flags,.reserved2=pgid,.argv=(uintptr_t)args};
    return call(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
}
static void ready(void) { require(call(SYS_WRITE,4,(uintptr_t)"R",1)==1); }
static void wait_ready(void) { char c; require(call(SYS_READ,3,(uintptr_t)&c,1)==1 && c=='R'); }
static void pause_for_child(void) { char c; require(call(SYS_INPUT_READ,(uintptr_t)&c,1,100)==0); }
static void reap(long pid,unsigned sig) {
    uint64_t status=~0ULL;
    require(call(SYS_WAITPID,pid,(uintptr_t)&status,0)==pid);
    require(status==sig && WIFSIGNALED(status) && WTERMSIG(status)==sig);
    require(call(SYS_KILL,pid,0,0)==SYSCALL_ESRCH);
}
static void child(char mode) {
    uint64_t mask=SIGNAL_BIT(SIGTERM), old;
    signal_action_t act={.handler=SIG_IGN}, previous;
    long nested=0;
    if(mode=='h') {
        require(call(SYS_SIGPROCMASK,99,0,(uintptr_t)&old)==0 && old==mask);
        require(call(SYS_SIGACTION,SIGINT,0,(uintptr_t)&previous)==0 && previous.handler==SIG_IGN);
        require(call(SYS_SIGPROCMASK,SIG_UNBLOCK,(uintptr_t)&mask,0)==0);
        call(SYS_EXIT,137,0,0); /* Parent's pending TERM was not inherited. */
    }
    if(mode=='s') { call(SYS_KILL,0,SIGTERM,0); require(false); }
    if(mode=='i') require(call(SYS_SIGACTION,SIGTERM,(uintptr_t)&act,0)==0);
    if(mode=='b') require(call(SYS_SIGPROCMASK,SIG_BLOCK,(uintptr_t)&mask,0)==0);
    if(mode=='w') for(unsigned i=0;i<4;i++) require(call(SYS_WRITE,6,(uintptr_t)data,sizeof(data))==sizeof(data));
    if(mode=='v') { nested=spawn("l",SPAWN_SETPGROUP|SPAWN_STAGED,0); require(nested>0); }
    ready();
    if(mode=='l') for(;;) __asm__ volatile("pause"); /* No syscalls: user-IRQ hook. */
    if(mode=='t') call(SYS_INPUT_READ,(uintptr_t)data,1,-1);
    if(mode=='f') call(SYS_READ,0,(uintptr_t)data,1);
    if(mode=='r' || mode=='b' || mode=='i') {
        require(call(SYS_READ,5,(uintptr_t)data,1)==1);
        if(mode=='b') { call(SYS_SIGPROCMASK,SIG_UNBLOCK,(uintptr_t)&mask,0); require(false); }
        if(mode=='i') { call(SYS_EXIT,37,0,0); return; }
    }
    if(mode=='w') call(SYS_WRITE,6,(uintptr_t)data,1);
    if(mode=='v') call(SYS_WAITPID,nested,0,0);
    require(false); /* A fatal signal must not return into user code. */
}
static const signal_action_t ro_action={0};
static const uint64_t ro_mask=0;
void shell_main(int argc,const char **argv) {
    if(argc>1) { child(argv[1][0]); return; }
    int fds[2];
    require(call(SYS_PIPE,(uintptr_t)fds,0,0)==0 && fds[0]==3 && fds[1]==4);
    require(call(SYS_PIPE,(uintptr_t)fds,0,0)==0 && fds[0]==5 && fds[1]==6);
    long own=call(SYS_GETPGRP,0,0,0);
    require(call(SYS_KILL,own,0,0)==0);
    require(call(SYS_KILL,-1,0,0)==SYSCALL_EINVAL);
    require(call(SYS_KILL,INT64_MIN,SIGTERM,0)==SYSCALL_EINVAL);
    require(call(SYS_KILL,own,32,0)==SYSCALL_EINVAL);
    require(call(SYS_KILL,own,SIGQUIT,0)==SYSCALL_EINVAL); /* Reserved; STOP/CONT now supported. */
    require(call(SYS_KILL,own,SIGCONT,0)==0);
    signal_action_t act={.handler=SIG_IGN}, old;
    require(call(SYS_SIGACTION,SIGKILL,(uintptr_t)&act,0)==SYSCALL_EINVAL);
    act.handler=0x400000;
    require(call(SYS_SIGACTION,SIGTERM,(uintptr_t)&act,0)==0);
    act.handler=SIG_IGN;
    require(call(SYS_SIGACTION,SIGTERM,(uintptr_t)&act,(uintptr_t)&ro_action)==SYSCALL_EFAULT);
    require(call(SYS_SIGACTION,SIGTERM,0,(uintptr_t)&old)==0 && old.handler==0x400000);
    require(call(SYS_SIGACTION,SIGTERM,UINTPTR_MAX-7,0)==SYSCALL_EFAULT);
    uint64_t mask=SIGNAL_BIT(SIGTERM)|SIGNAL_BIT(SIGKILL), previous;
    require(call(SYS_SIGPROCMASK,SIG_SETMASK,(uintptr_t)&mask,(uintptr_t)&ro_mask)==SYSCALL_EFAULT);
    require(call(SYS_SIGPROCMASK,0,0,(uintptr_t)&previous)==0 && previous==0);
    require(call(SYS_SIGPROCMASK,SIG_SETMASK,(uintptr_t)&mask,0)==0);
    require(call(SYS_SIGPROCMASK,0,0,(uintptr_t)&previous)==0 && previous==SIGNAL_BIT(SIGTERM));
    require(call(SYS_SIGACTION,SIGINT,(uintptr_t)&act,0)==0);
    require(call(SYS_KILL,own,SIGTERM,0)==0); /* Pending + blocked in parent. */
    long pid=spawn("h",SPAWN_SETPGROUP,0); require(pid>0);
    uint64_t status;
    require(call(SYS_WAITPID,pid,(uintptr_t)&status,0)==pid && status==0x8900);
    require(call(SYS_SIGACTION,SIGTERM,(uintptr_t)&act,0)==0); /* Discards pending. */
    mask=0; require(call(SYS_SIGPROCMASK,SIG_SETMASK,(uintptr_t)&mask,0)==0);
    act.handler=SIG_DFL;
    require(call(SYS_SIGACTION,SIGTERM,(uintptr_t)&act,0)==0);
    require(call(SYS_SIGACTION,SIGINT,(uintptr_t)&act,0)==0);
    const char *modes[]={"l","t","f","r","w","v"};
    for(unsigned i=0;i<6;i++) {
        pid=spawn(modes[i],SPAWN_SETPGROUP,0); require(pid>0);
        wait_ready(); pause_for_child();
        require(call(SYS_KILL,pid,SIGKILL,0)==0); reap(pid,SIGKILL);
        if(i==4) for(unsigned j=0;j<4;j++) require(call(SYS_READ,5,(uintptr_t)data,sizeof(data))==sizeof(data));
    }
    for(unsigned i=0;i<2;i++) {
        pid=spawn(i ? "b":"i",SPAWN_SETPGROUP,0); require(pid>0);
        wait_ready(); require(call(SYS_KILL,pid,SIGTERM,0)==0); pause_for_child();
        require(call(SYS_WAITPID,pid,0,WNOHANG)==0);
        require(call(SYS_WRITE,6,(uintptr_t)"X",1)==1);
        if(i) reap(pid,SIGTERM);
        else require(call(SYS_WAITPID,pid,(uintptr_t)&status,0)==pid && status==(37<<8));
    }
    pid=spawn("s",SPAWN_SETPGROUP,0); require(pid>0); reap(pid,SIGTERM);
    long a=spawn("l",SPAWN_SETPGROUP,0); require(a>0); wait_ready();
    long b=spawn("l",SPAWN_SETPGROUP,a); require(b>0); wait_ready();
    require(call(SYS_KILL,-a,SIGINT,0)==0); reap(a,SIGINT); reap(b,SIGINT);
    for(unsigned i=0;i<80;i++) { /* Repeated teardown + legacy compatibility. */
        pid=spawn("l",SPAWN_SETPGROUP|SPAWN_STAGED,0); require(pid>0);
        require(call(SYS_KILL,pid,SIGTERM,0)==0);
        require(call(SYS_GROUP_RELEASE,pid,GROUP_RELEASE,0)==0);
        require(call(SYS_WAIT,pid,(uintptr_t)&status,0)==0 && status==128+SIGTERM);
    }
    const char pass[]="S8 SIGNAL PASS\n";
    call(SYS_WRITE,1,(uintptr_t)pass,sizeof(pass)-1);
}
