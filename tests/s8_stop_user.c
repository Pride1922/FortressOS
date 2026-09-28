/* Disposable Ring 3 S8 Phase 2C fixture; no disk writes. */
#include "syscall_abi.h"
static volatile uint64_t child_notice;
static void chld(unsigned sig) { (void)sig; child_notice=1; }
static long call(long n, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return n;
}
static void check(bool ok, unsigned line) {
    if (ok) return;
    const char msg[]="S8 STOP FAIL ";
    char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    call(SYS_WRITE,2,(uintptr_t)msg,sizeof(msg)-1); call(SYS_WRITE,2,(uintptr_t)n,sizeof(n));
    call(SYS_EXIT,1,0,0); __builtin_trap();
}
#define require(x) check((x),__LINE__)
static long io(long nr, long fd, void *p, size_t n) {
    long r; do { r=call(nr,fd,(uintptr_t)p,n); } while (r==SYSCALL_EINTR); return r;
}
static long wait_for(long pid, uint64_t *status, unsigned options) {
    long r; do { r=call(SYS_WAITPID,pid,(uintptr_t)status,options); } while (r==SYSCALL_EINTR); return r;
}
static void delay(void) {
    char c; long r=call(SYS_INPUT_READ,(uintptr_t)&c,1,100);
    require(r==0 || r==SYSCALL_EINTR);
}
static void send(long pid, unsigned sig) { require(call(SYS_KILL,pid,sig,0)==0); }
static char data[16384];
static void child(char mode) {
    require(call(SYS_CLOSE,7,0,0)==0);
    signal_action_t action={.handler=SIG_IGN};
    if (mode=='i') require(call(SYS_SIGACTION,SIGCONT,(uintptr_t)&action,0)==0);
    if (mode=='c') {
        uint64_t mask=SIGNAL_BIT(SIGCONT);
        require(call(SYS_SIGPROCMASK,SIG_BLOCK,(uintptr_t)&mask,0)==0);
    }
    if (mode=='w') for (unsigned i=0;i<4;i++) require(io(SYS_WRITE,6,data,sizeof(data))==sizeof(data));
    require(io(SYS_WRITE,4,(void *)"R",1)==1);
    if (mode=='l') for (;;) __asm__ volatile("pause");
    if (mode=='b' || mode=='q') { char c; require(io(SYS_READ,5,&c,1)==1 && c=='D'); }
    else if (mode=='w') require(io(SYS_WRITE,6,(void *)"W",1)==1);
    else if (mode=='t') { char c; call(SYS_INPUT_READ,(uintptr_t)&c,1,-1); require(false); }
    else send(0,SIGSTOP);
    /* A killed stopped task must never reach this marker. */
    require(io(SYS_WRITE,8,(void *)"X",1)==1);
    for (;;) __asm__ volatile("pause");
}
void shell_main(int argc, const char **argv) {
    if (argc>1) { child(argv[1][0]); return; }
    int fds[2];
    require(call(SYS_PIPE,(uintptr_t)fds,0,0)==0 && fds[0]==3 && fds[1]==4);
    require(call(SYS_PIPE,(uintptr_t)fds,0,0)==0 && fds[0]==5 && fds[1]==6);
    signal_action_t action={.handler=SIG_IGN};
    require(call(SYS_SIGACTION,SIGCHLD,(uintptr_t)&action,0)==SYSCALL_EINVAL);
    action=(signal_action_t){.flags=2};
    require(call(SYS_SIGACTION,SIGCHLD,(uintptr_t)&action,0)==SYSCALL_EINVAL);
    const char *modes[]={"s","i","c","l","b","q","w","t","k"};
    for (unsigned cycle=0;cycle<81;cycle++) {
        const char *mode=modes[cycle%9];
        /* First round default CHLD, later rounds caught CHLD (flag only). */
        action=(signal_action_t){.handler=cycle<9 ? SIG_DFL : (uintptr_t)chld};
        require(call(SYS_SIGACTION,SIGCHLD,(uintptr_t)&action,0)==0);
        child_notice=0;
        require(call(SYS_PIPE,(uintptr_t)fds,0,0)==0 && fds[0]==7 && fds[1]==8);
        const char *args[]={"/bin/shell",mode,0};
        /* Keep the terminal-blocked case foreground with its parent. Phase 3
         * tests SIGTTIN separately; this fixture must still reach blocked input. */
        spawn_opts_t opts={.size=64,.version=2,.flags=*mode=='t' ? 0 : SPAWN_SETPGROUP,.argv=(uintptr_t)args};
        long pid=call(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
        require(pid>0);
        require(call(SYS_CLOSE,8,0,0)==0);
        char c;
        require(io(SYS_READ,3,&c,1)==1 && c=='R');
        if (*mode=='b' || *mode=='q' || *mode=='w' || *mode=='t' || *mode=='l') {
            delay(); send(pid,SIGSTOP);
        }
        uint64_t status=~0ULL;
        require(wait_for(pid,&status,WUNTRACED)==pid && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
        if (cycle>=9) require(child_notice==1);
        /* Repeated STOP must not manufacture a new report. */
        send(pid,SIGSTOP); delay();
        require(wait_for(pid,NULL,WNOHANG|WUNTRACED|WCONTINUED)==0);
        if (*mode=='b' || *mode=='q') {
            require(io(SYS_WRITE,6,(void *)"D",1)==1); /* Ordinary channel wake while STOPPED. */
            delay(); require(wait_for(pid,NULL,WNOHANG|WCONTINUED)==0);
        }
        if (*mode=='w') {
            require(io(SYS_READ,5,&c,1)==1);
            delay(); require(wait_for(pid,NULL,WNOHANG|WCONTINUED)==0);
        }
        bool kill_only=*mode=='k' || *mode=='q' || *mode=='t';
        if (!kill_only) {
            send(pid,SIGCONT);
            require(wait_for(pid,&status,WCONTINUED)==pid && WIFCONTINUED(status));
            require(wait_for(pid,NULL,WNOHANG|WCONTINUED)==0);
            if (*mode!='l') require(io(SYS_READ,7,&c,1)==1 && c=='X');
            /* A second stop must become reportable after consuming the first. */
            send(pid,SIGTSTP);
            require(wait_for(pid,&status,WUNTRACED)==pid && WSTOPSIG(status)==SIGTSTP);
        }
        send(pid,SIGKILL); /* No CONT: saved kernel continuation must clean up. */
        require(wait_for(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
        require(wait_for(pid,NULL,WNOHANG)==SYSCALL_ECHILD);
        require(io(SYS_READ,7,&c,1)==0); /* EOF proves endpoint cleanup, no post-kill marker. */
        require(call(SYS_CLOSE,7,0,0)==0);
        if (*mode=='q') require(io(SYS_READ,5,&c,1)==1 && c=='D'); /* Stopped read never consumed it. */
        if (*mode=='w') for (unsigned i=0;i<4;i++) require(io(SYS_READ,5,data,sizeof(data))==sizeof(data));
    }
    const char pass[]="S8 STOP PASS\n";
    call(SYS_WRITE,1,(uintptr_t)pass,sizeof(pass)-1);
}
