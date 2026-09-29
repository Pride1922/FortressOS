/* Test-only Ring 3 helper. All large buffers/ABI objects stay in BSS. */
#include "syscall_abi.h"
#include "vfs.h"
static long call(long n, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return n;
}
static void text(const char *s) {
    size_t n=0; while (s[n]) ++n;
    call(SYS_WRITE,2,(uintptr_t)s,n);
}
static void check(bool ok, unsigned line) {
    if (ok) return;
    char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    text("S8 SIGPIPE FAIL "); call(SYS_WRITE,2,(uintptr_t)n,sizeof(n));
    call(SYS_EXIT,1,0,0); __builtin_trap();
}
#define require(x) check((x),__LINE__)
static char data[16384];
static int fds[2], ready_fds[2];
static signal_action_t action;
static uint64_t mask, status;
static volatile unsigned caught;
static void handler(unsigned sig) { if (sig==SIGPIPE) ++caught; }
static void disposition(uint64_t h) {
    action=(signal_action_t){.handler=h};
    require(!call(SYS_SIGACTION,SIGPIPE,(uintptr_t)&action,0));
}
static void close_fd(int fd) { require(!call(SYS_CLOSE,fd,0,0)); }
static void new_pipe(int *out) { require(!call(SYS_PIPE,(uintptr_t)out,VFS_O_CLOEXEC,0)); }
static long launch(const char *path, const char *arg, const char *extra,
                   int input, int output, int ready) {
    static const char *args[5];
    static spawn_fd_action_t actions[3];
    static spawn_opts_t opts;
    unsigned count=0;
    args[0]=path; args[1]=arg; args[2]=extra; args[3]=NULL;
    if (input>=0) actions[count++]=(spawn_fd_action_t){.type=SPAWN_FD_ACTION_DUP2,.src_fd=input,.dst_fd=0};
    if (output>=0) actions[count++]=(spawn_fd_action_t){.type=SPAWN_FD_ACTION_DUP2,.src_fd=output,.dst_fd=1};
    if (ready>=0) actions[count++]=(spawn_fd_action_t){.type=SPAWN_FD_ACTION_DUP2,.src_fd=ready,.dst_fd=3};
    opts=(spawn_opts_t){.size=sizeof(opts),.version=2,.argv=(uintptr_t)args,
                       .fd_actions=count ? (uintptr_t)actions : 0,.action_count=count};
    long pid=call(SYS_SPAWN_EXT,(uintptr_t)path,(uintptr_t)&opts,sizeof(opts));
    require(pid>0); return pid; /* Same group as parent: catches group misdelivery. */
}
static void reap(long pid, unsigned sig, unsigned code) {
    require(call(SYS_WAITPID,pid,(uintptr_t)&status,0)==pid);
    if (sig) require(WIFSIGNALED(status) && WTERMSIG(status)==sig);
    else require(WIFEXITED(status) && WEXITSTATUS(status)==code);
}
static void child(char mode) {
    if (mode=='p') { /* Infinite producer, suitable for the real shell/head. */
        for (unsigned i=0;i<sizeof(data);++i) data[i]=(i&1)?'\n':'x';
        for (;;) require(call(SYS_WRITE,1,(uintptr_t)data,sizeof(data))>0);
    }
    if (mode=='w' || mode=='s') {
        for (unsigned i=0;i<4;++i) require(call(SYS_WRITE,1,(uintptr_t)data,sizeof(data))==sizeof(data));
        require(call(SYS_WRITE,3,(uintptr_t)"R",1)==1);
        call(SYS_WRITE,1,(uintptr_t)data,1);
        require(false); /* Default PIPE must terminate after reader close. */
    }
    new_pipe(fds);
    if (mode=='i') disposition(SIG_IGN);
    if (mode=='c' || mode=='b' || mode=='t') disposition((uintptr_t)handler);
    if (mode=='b' || mode=='u') {
        mask=SIGNAL_BIT(SIGPIPE);
        require(!call(SYS_SIGPROCMASK,SIG_BLOCK,(uintptr_t)&mask,0));
    }
    if (mode=='t') {
        for (unsigned i=0;i<3;++i) require(call(SYS_WRITE,fds[1],(uintptr_t)data,sizeof(data))==sizeof(data));
        require(call(SYS_WRITE,fds[1],(uintptr_t)data,sizeof(data)-17)==sizeof(data)-17);
        require(call(SYS_WRITE,fds[1],(uintptr_t)data,sizeof(data))==17);
        require(caught==0);
    }
    close_fd(fds[0]);
    require(call(SYS_WRITE,fds[1],0,0)==0 && caught==0);
    require(call(SYS_WRITE,fds[1],0,1)==SYSCALL_EFAULT && caught==0);
    long result=call(SYS_WRITE,fds[1],(uintptr_t)data,1);
    require(mode!='d' && result==SYSCALL_EPIPE);
    if (mode=='c' || mode=='t') require(caught==1); /* Restored RAX after sigreturn. */
    else require(caught==0);
    if (mode=='b' || mode=='u') {
        /* Repeated writes coalesce while blocked, and remain pending. */
        require(call(SYS_WRITE,fds[1],(uintptr_t)data,1)==SYSCALL_EPIPE && caught==0);
        require(!call(SYS_SIGPROCMASK,SIG_UNBLOCK,(uintptr_t)&mask,0));
        require(mode=='b' && caught==1); /* 'u' must terminate at unblock. */
    }
    close_fd(fds[1]);
}
void shell_main(int argc, const char **argv) {
    if (argc>1) { child(argv[1][0]); return; }
    disposition(SIG_DFL);
    const char *modes[]={"d","i","c","b","t","u"};
    for (unsigned i=0;i<6;++i) {
        long pid=launch("/bin/sigpipe-probe",modes[i],NULL,-1,-1,-1);
        reap(pid,(i==0 || i==5)?SIGPIPE:0,0);
    }
    text("S8 SIGPIPE dispositions and partial PASS\n");
    for (unsigned stop=0;stop<2;++stop) {
        new_pipe(fds); new_pipe(ready_fds);
        long pid=launch("/bin/sigpipe-probe",stop?"s":"w",NULL,-1,fds[1],ready_fds[1]);
        close_fd(fds[1]); close_fd(ready_fds[1]);
        require(call(SYS_READ,ready_fds[0],(uintptr_t)data,1)==1 && data[0]=='R');
        close_fd(ready_fds[0]);
        if (stop) {
            require(!call(SYS_KILL,pid,SIGSTOP,0));
            require(call(SYS_WAITPID,pid,(uintptr_t)&status,WUNTRACED)==pid);
            require(WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
        }
        close_fd(fds[0]);
        if (stop) require(!call(SYS_KILL,pid,SIGCONT,0));
        reap(pid,SIGPIPE,0);
    }
    text("S8 SIGPIPE early close and stopped producer PASS\n");
    new_pipe(fds);
    long writer=launch("/bin/sigpipe-probe","p",NULL,-1,fds[1],-1);
    long reader=launch("/bin/head","-n","1",fds[0],-1,-1);
    close_fd(fds[0]); close_fd(fds[1]);
    reap(reader,0,0); reap(writer,SIGPIPE,0);
    text("S8 SIGPIPE head upstream signal metadata PASS\n");
    for (unsigned variant=0;variant<4;++variant) {
        bool builtin=(variant&1)!=0, ignored=variant>=2;
        new_pipe(fds); close_fd(fds[0]);
        disposition(ignored?SIG_IGN:SIG_DFL); /* Exec preserves ignored disposition. */
        writer=launch(builtin?"/bin/sh-builtin":"/bin/cat",builtin?"echo":"--help",
                      builtin?"hello":NULL,-1,fds[1],-1);
        disposition(SIG_DFL); close_fd(fds[1]);
        reap(writer,ignored?0:SIGPIPE,1); /* Real tools return 1 only when EPIPE returns. */
    }
    text("S8 SIGPIPE PASS\n");
}
