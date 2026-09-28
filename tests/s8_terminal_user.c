/* Disposable Ring 3 Phase 3 fixture. Runner injects real IRQ1/IRQ4 input. */
#include "syscall_abi.h"
#include "terminal.h"
static long call4(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    register uintptr_t r10 __asm__("r10")=d;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a),"S"(b),"d"(c),"r"(r10) : "rcx","r11","memory","cc");
    return n;
}
#define call(n,a,b,c) call4(n,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),0)
static void check(bool ok,unsigned line) {
    if (ok) return;
    const char msg[]="S8 TERMINAL FAIL ";
    char num[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    call(SYS_WRITE,2,msg,sizeof(msg)-1); call(SYS_WRITE,2,num,sizeof(num));
    call(SYS_EXIT,1,0,0); __builtin_trap();
}
#define require(x) check((x),__LINE__)
static void text(const char *s) {
    size_t n=0; while (s[n]) ++n;
    require(call(SYS_WRITE,1,s,n)==(long)n);
}
static long waitfor(long pid,uint64_t *status,unsigned options) {
    long r; do { r=call(SYS_WAITPID,pid,status,options); } while (r==SYSCALL_EINTR); return r;
}
static void reap(long pid,unsigned sig) {
    uint64_t status=~0ULL;
    require(waitfor(pid,&status,0)==pid);
    require(sig ? WIFSIGNALED(status) && WTERMSIG(status)==sig : WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void disposition(unsigned sig,uintptr_t handler) {
    signal_action_t a={.handler=handler}; require(!call(SYS_SIGACTION,sig,&a,0));
}
static void attributes(unsigned op,terminal_attrs_t *a) {
    require(!call4(SYS_TERMATTR,31,op,(uintptr_t)a,sizeof(*a)));
}
static long spawn(const char *mode,unsigned flags,uint64_t pgid,bool redirected) {
    const char *args[]={"/bin/shell",mode,0};
    spawn_fd_action_t action={.type=SPAWN_FD_ACTION_DUP2,.src_fd=5,.dst_fd=0};
    spawn_opts_t opts={.size=64,.version=2,.flags=flags,.reserved2=pgid,.argv=(uintptr_t)args,
                      .fd_actions=redirected ? (uintptr_t)&action : 0,.action_count=redirected ? 1 : 0};
    long pid=call(SYS_SPAWN_EXT,args[0],&opts,sizeof(opts)); require(pid>0); return pid;
}
static void child(char mode) {
    /* The retained parent handle must be CLOEXEC. Recreate it only for explicit
     * checks that fd 31 conveys no foreground privilege. */
    require(call(SYS_TCGETPGRP,31,0,0)==SYSCALL_EBADF);
    disposition(SIGTTOU,SIG_DFL); disposition(SIGTSTP,SIG_DFL);
    if (mode=='l') {
        require(call(SYS_WRITE,4,"R",1)==1);
        for (;;) __asm__ volatile("pause");
    }
    char c;
    if (mode=='f') { require(call(SYS_READ,0,&c,1)==1 && c=='F'); return; }
    require(call(SYS_DUP2,0,31,0)==31);
    if (mode=='i' || mode=='b') {
        if (mode=='i') disposition(SIGTTIN,SIG_IGN);
        else { uint64_t mask=SIGNAL_BIT(SIGTTIN); require(!call(SYS_SIGPROCMASK,SIG_BLOCK,&mask,0)); }
        require(call(SYS_READ,31,&c,1)==SYSCALL_EIO);
        require(call(SYS_INPUT_READ,&c,1,0)==SYSCALL_EIO);
        return;
    }
    if (mode=='t') {
        terminal_info_t info={.version=1};
        require(!call(SYS_TERMCTL,TERM_SET,&info,sizeof(info))); return;
    }
    if (mode=='a') {
        terminal_attrs_t attrs; attributes(TERM_GET,&attrs);
        attrs.input_flags=0; attributes(TERM_SET,&attrs); return;
    }
    if (mode=='c') { require(!call(SYS_TCSETPGRP,31,call(SYS_GETPGRP,0,0,0),0)); return; }
    require(call(SYS_READ,31,&c,1)==1 && c=='R');
}
void shell_main(int argc,const char **argv) {
    if (argc>1) { child(argv[1][0]); return; }
    long own=call(SYS_GETPGRP,0,0,0); require(own>0);
    require(call(SYS_FCNTL,0,F_DUPFD_CLOEXEC,31)==31);
    require(call(SYS_FCNTL,31,F_GETFD,0)==FD_CLOEXEC);
    require(call(SYS_TCGETPGRP,31,0,0)==own);
    disposition(SIGTTOU,SIG_IGN);
    require(!call(SYS_KBD_LAYOUT,0,0,0)); /* US QMP key names */
    int fds[2]; require(!call(SYS_PIPE,fds,0,0) && fds[0]==3 && fds[1]==4);
    require(!call(SYS_PIPE,fds,0,0) && fds[0]==5 && fds[1]==6);
    require(call(SYS_TCGETPGRP,3,0,0)==SYSCALL_ENOTTY);
    require(call(SYS_TCGETPGRP,UINT64_MAX,0,0)==SYSCALL_EBADF);
    require(call(SYS_TCSETPGRP,31,0,0)==SYSCALL_EINVAL);
    require(call(SYS_TCSETPGRP,31,INT64_MAX,0)==SYSCALL_ESRCH);
    terminal_attrs_t attrs;
    attributes(TERM_GET,&attrs); require(attrs.version==1 && attrs.input_flags==TERM_ISIG);
    require(call4(SYS_TERMATTR,31,TERM_GET,0,32)==SYSCALL_EFAULT);
    require(call4(SYS_TERMATTR,31,TERM_GET,(uintptr_t)"readonly",32)==SYSCALL_EFAULT);
    require(call4(SYS_TERMATTR,31,TERM_GET,UINT64_MAX-15,32)==SYSCALL_EFAULT);
    require(call4(SYS_TERMATTR,31,TERM_GET,(uintptr_t)&attrs,31)==SYSCALL_EINVAL);
    attrs.version=2; require(call4(SYS_TERMATTR,31,TERM_SET,(uintptr_t)&attrs,32)==SYSCALL_EINVAL);
    attrs.version=1; attrs.reserved[0]=1;
    require(call4(SYS_TERMATTR,31,TERM_SET,(uintptr_t)&attrs,32)==SYSCALL_EINVAL); attrs.reserved[0]=0;
    /* Acknowledged ordinary byte remains untouched by ignored/blocked readers. */
    text("S8 TERMINAL PRESERVE\n");
    /* Runner waits for this marker; delayed arrival is harmless: background
     * reads fail without waiting even when the queue is empty. */
    reap(spawn("i",SPAWN_SETPGROUP,0,false),0);
    reap(spawn("b",SPAWN_SETPGROUP,0,false),0);
    char c; require(call(SYS_READ,31,&c,1)==1 && c=='P');
    require(call(SYS_WRITE,6,"F",1)==1);
    reap(spawn("f",SPAWN_SETPGROUP,0,true),0);
    const char *modes[]={"r","t","a","c"};
    for (unsigned i=0;i<4;++i) {
        long pid=spawn(modes[i],SPAWN_SETPGROUP,0,false);
        uint64_t status; require(waitfor(pid,&status,WUNTRACED)==pid);
        require(WIFSTOPPED(status) && WSTOPSIG(status)==(i ? SIGTTOU : SIGTTIN));
        require(!call(SYS_TCSETPGRP,31,pid,0));
        require(!call(SYS_KILL,pid,SIGCONT,0));
        if (!i) text("S8 TERMINAL READER\n");
        reap(pid,0); require(!call(SYS_TCSETPGRP,31,own,0));
        attrs.input_flags=TERM_ISIG; attributes(TERM_SET,&attrs);
    }
    /* Redirect stdin in the supervisor: every handoff/reclaim below uses 31. */
    require(call(SYS_DUP2,5,0,0)==0);
    require(call(SYS_TCGETPGRP,0,0,0)==SYSCALL_ENOTTY);
    const char *markers[]={"S8 TERMINAL UART INT\n","S8 TERMINAL KEY INT\n","S8 TERMINAL KEY STOP\n"};
    for (unsigned round=0;round<3;++round) {
        long first=spawn("l",SPAWN_SETPGROUP|SPAWN_STAGED,0,false);
        long second=spawn("l",SPAWN_SETPGROUP|SPAWN_STAGED,first,false);
        require(!call(SYS_TCSETPGRP,31,first,0));
        require(!call(SYS_GROUP_RELEASE,first,GROUP_RELEASE,0));
        for (unsigned i=0;i<2;++i) require(call(SYS_READ,3,&c,1)==1 && c=='R');
        text(markers[round]);
        if (round==2) {
            uint64_t status;
            require(waitfor(first,&status,WUNTRACED)==first && WIFSTOPPED(status) && WSTOPSIG(status)==SIGTSTP);
            require(waitfor(second,&status,WUNTRACED)==second && WIFSTOPPED(status) && WSTOPSIG(status)==SIGTSTP);
            require(!call(SYS_KILL,-first,SIGKILL,0));
        }
        reap(first,round==2 ? SIGKILL : SIGINT); reap(second,round==2 ? SIGKILL : SIGINT);
        require(!call(SYS_TCSETPGRP,31,own,0));
    }
    attrs.input_flags=0; attributes(TERM_SET,&attrs);
    text("S8 TERMINAL UART LITERAL\n"); require(call(SYS_READ,31,&c,1)==1 && c==3);
    text("S8 TERMINAL KEY LITERAL\n"); require(call(SYS_INPUT_READ,&c,1,-1)==1 && c==26);
    attrs.input_flags=TERM_ISIG; attributes(TERM_SET,&attrs);
    for (unsigned i=3;i<=6;++i) require(!call(SYS_CLOSE,i,0,0));
    text("S8 TERMINAL PASS\n");
    for (;;) __asm__ volatile("pause"); /* runner owns bounded teardown */
}
