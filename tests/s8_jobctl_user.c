/* Helpers added only to the disposable real-shell job-control ISO. */
#include "syscall_abi.h"
#include "terminal.h"
static long call4(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    register uintptr_t r10 __asm__("r10")=d;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a),"S"(b),"d"(c),"r"(r10) : "rcx","r11","memory","cc");
    return n;
}
#define call(n,a,b,c) call4(n,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),0)
static bool same(const char *a,const char *b) { while (*a && *a==*b) { ++a; ++b; } return *a==*b; }
static void text(const char *s) { size_t n=0; while(s[n]) ++n; call(SYS_WRITE,2,s,n); }
static void require(bool ok) {
    if (!ok) { text("JOBCTL FAIL\n"); call(SYS_EXIT,1,0,0); __builtin_trap(); }
}
static void number(long n) {
    static char digits[24]; unsigned i=sizeof(digits);
    digits[--i]='\n'; do { digits[--i]='0'+n%10; n/=10; } while(n);
    call(SYS_WRITE,2,digits+i,sizeof(digits)-i);
}
static uint64_t ticks(void) {
    uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((uint64_t)hi<<32)|lo;
}
static void ignore_hup(void) {
    static signal_action_t action={.handler=SIG_IGN};
    require(!call(SYS_SIGACTION,SIGHUP,&action,0));
}
void shell_main(int argc,const char **argv) {
    require(argc>=2);
    const char *mode=argv[1];
    if (same(mode,"loop") || same(mode,"hold")) {
        bool hold=same(mode,"hold");
        if (hold) ignore_hup();
        text("JOBCTL READY\n");
        uint64_t last=ticks();
        long group=call(SYS_GETPGRP,0,0,0);
        for (;;) {
            if (ticks()-last>=1000000000ULL) {
                last=ticks();
                bool fg = call(SYS_TCGETPGRP,2,0,0)==group;
                /* Repeated acknowledgement is intentional: bg followed by fg
                 * can occur entirely between polls. An edge-only marker would
                 * then suppress the acknowledgement for the next fg cycle. */
                if (!hold && fg) {
                    static terminal_attrs_t attrs;
                    static signal_action_t action;
                    require(!call4(SYS_TERMATTR,2,TERM_GET,(uintptr_t)&attrs,sizeof(attrs)));
                    require((attrs.input_flags & TERM_ISIG) && attrs.vsusp==26);
                    require(!call(SYS_SIGACTION,SIGTSTP,0,&action));
                    require(action.handler==SIG_DFL);
                    text("JOBCTL FOREGROUND\n");
                }
            }
            __asm__ volatile("pause");
        }
    }
    if (same(mode,"writer")) {
        ignore_hup(); static char bytes[16384]; text("JOBCTL WRITER\n");
        size_t filled=0;
        while (filled<65536) {
            long n=call(SYS_WRITE,1,bytes,sizeof(bytes));
            require(n>0 || n==SYSCALL_EINTR);
            if (n>0) filled+=(size_t)n;
        }
        text("JOBCTL PIPE FULL\n");
        for (;;) { long n=call(SYS_WRITE,1,bytes,sizeof(bytes)); require(n>0 || n==SYSCALL_EINTR); }
    }
    if (same(mode,"read")) {
        static char byte; text("JOBCTL READ\n");
        long n;
        do { n=call(SYS_INPUT_READ,&byte,1,-1); } while(n==SYSCALL_EINTR || n==INPUT_LOST);
        require(n==1 && byte=='Q'); text("JOBCTL READ PASS\n"); return;
    }
    if (same(mode,"attrs")) {
        static terminal_attrs_t attrs;
        require(!call4(SYS_TERMATTR,0,TERM_GET,(uintptr_t)&attrs,sizeof(attrs)));
        require(attrs.input_flags==TERM_ISIG);
        attrs.input_flags=0;
        require(!call4(SYS_TERMATTR,0,TERM_SET,(uintptr_t)&attrs,sizeof(attrs)));
        require(!call(SYS_KILL,0,SIGSTOP,0));
        require(!call4(SYS_TERMATTR,0,TERM_GET,(uintptr_t)&attrs,sizeof(attrs)));
        require(attrs.input_flags==0);
        text("JOBCTL ATTRS PASS\n"); return;
    }
    if (same(mode,"shell-attrs")) {
        static terminal_attrs_t attrs;
        require(!call4(SYS_TERMATTR,0,TERM_GET,(uintptr_t)&attrs,sizeof(attrs)));
        require(attrs.input_flags==TERM_ISIG); text("JOBCTL SHELL ATTRS PASS\n"); return;
    }
    if (same(mode,"orphan-child")) {
        require(!call(SYS_KILL,0,SIGSTOP,0));
        for (;;) __asm__ volatile("pause");
    }
    if (same(mode,"orphan")) {
        static const char *args[]={"/bin/jobctl-probe","orphan-child",0};
        static spawn_opts_t opts={.size=64,.version=2,.flags=SPAWN_SETPGROUP};
        static uint64_t status;
        opts.argv=(uintptr_t)args;
        long pid=call(SYS_SPAWN_EXT,args[0],&opts,sizeof(opts)); require(pid>0);
        require(call(SYS_WAITPID,pid,&status,WUNTRACED)==pid && WIFSTOPPED(status));
        opts.flags|=SPAWN_STAGED;
        long staged=call(SYS_SPAWN_EXT,args[0],&opts,sizeof(opts)); require(staged>0);
        text("JOBCTL ORPHAN "); number(pid);
        text("JOBCTL STAGED "); number(staged);
        return; /* Bypass shell cleanup: kernel must handle both children. */
    }
    if (same(mode,"absent")) {
        require(argc==3); long pgid=0;
        for (const char *p=argv[2]; *p; ++p) {
            require(*p>='0' && *p<='9' && pgid<100000000);
            pgid=pgid*10+*p-'0';
        }
        require(pgid>1); uint64_t start=ticks(); long result;
        do { result=call(SYS_KILL,-pgid,0,0); }
        while (result!=SYSCALL_ESRCH && ticks()-start<12000000000ULL);
        require(result==SYSCALL_ESRCH); text("JOBCTL ABSENT PASS\n"); return;
    }
    require(false);
}
