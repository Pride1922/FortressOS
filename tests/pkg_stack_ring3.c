/* Private fixture only. No default image integration or production policy. */
#include "user/tools/common.h"
#include "src/kernel/elf.h"
extern int pkg_test_main(int, char **);
#ifndef STACK_SELF
#define STACK_SELF "/bin/pkg-stack"
#endif
static volatile uintptr_t low, signal_low;
static volatile unsigned armed, signals, failed;
static long own_pid;
static char strings[32][257];
static char *args[34], *env[34];
static char *verify_args[]={"pkg","verify","/docs/catalog","demo","/docs/fixture.fpk"};
static char *bad_args[]={"pkg","verify","/docs/bad-catalog","demo","/docs/fixture.fpk"};
static inline __attribute__((always_inline)) long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c) {
    __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");
    return n;
}
static void caught(int sig) {
    volatile unsigned char scratch[512];
    uintptr_t sp; __asm__ volatile("mov %%rsp,%0":"=r"(sp));
    if(sp<signal_low) signal_low=sp;
    for(unsigned i=0;i<sizeof(scratch);i++) scratch[i]=(unsigned char)i;
    for(unsigned i=0;i<sizeof(scratch);i++) if(scratch[i]!=(unsigned char)i) failed=1;
    if(sig!=SIGINT) failed=1;
    signals++;
}
/* Every new low samples the allocated frame, including vendor crypto frames.
 * Hook calls deliberately add overhead; results describe this instrumented ELF.
 * Signal handler/hook are compiled separately without instrumentation. */
void __cyg_profile_func_enter(void *fn,void *caller) {
    (void)fn;(void)caller;
    uintptr_t sp; __asm__ volatile("mov %%rsp,%0":"=r"(sp));
    if(armed && sp<low) {
        low=sp;
        if(armed==2 && raw(SYS_KILL,(uintptr_t)own_pid,SIGINT,0)) failed=1;
    }
}
void __cyg_profile_func_exit(void *fn,void *caller) {(void)fn;(void)caller;}
static void number(const char *label,uint64_t n) {
    char b[32];size_t k=tool_format_u64(b,n);
    tool_write("stack",label,tool_length(label));tool_write("stack",b,k);tool_write("stack","\n",1);
}
int pkg_stack_main(int argc,char **argv,char **envp) {
    (void)argv;
    if(argc!=32) {
        for(unsigned i=0;i<32;i++) {
            unsigned n=i<3?255:(i==31?31:7);
            for(unsigned j=0;j<n;j++) strings[i][j]='a';
            strings[i][n]=0;args[i]=strings[i];env[i]=strings[i];
        }
        spawn_opts_t opts={.size=sizeof(opts),.version=1,.argv=(uintptr_t)args,.envp=(uintptr_t)env};
        long pid=raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts));
        int64_t status=-1;
        if(pid<0 || raw(SYS_WAIT,pid,(uintptr_t)&status,0) || status) return 1;
        /* Check independent aggregate, single-string and vector-count limits. */
        strings[31][31]='a';strings[31][32]=0;
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        args[31]="";
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        strings[31][31]=0;args[31]=strings[31];
        strings[0][255]='a';strings[0][256]=0;
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        args[0]="";
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        strings[0][255]=0;args[0]=strings[0];
        args[32]="";
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        args[32]=0;env[32]="";
        if(raw(SYS_SPAWN_EXT,(uintptr_t)STACK_SELF,(uintptr_t)&opts,sizeof(opts))!=SYSCALL_E2BIG) return 1;
        tool_write("stack","STACK PASS\n",11);return 0;
    }
    size_t bytes=0,ebytes=0;unsigned ec=0;
    for(unsigned i=0;i<32;i++) bytes+=tool_length(argv[i])+1;
    while(envp[ec]) {ebytes+=tool_length(envp[ec])+1;ec++;}
    if(bytes!=1024 || ebytes!=1024 || ec!=32) return 1;
    proc_info_t info;
    if(raw(SYS_PROCINFO,PROC_INFO_SELF,(uintptr_t)&info,0)!=1) return 1;
    own_pid=info.pid;
    signal_action_t action={.handler=(uintptr_t)caught};
    if(raw(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0)) return 1;
    number("STACK argv_bytes=",bytes);number("STACK env_bytes=",ebytes);
    for(unsigned mode=1;mode<=
#ifdef STACK_PLAIN
        1
#else
        2
#endif
        ;mode++) {
        uintptr_t sp;__asm__ volatile("mov %%rsp,%0":"=r"(sp));
        /* Fill only below the active frame. No calls between RSP and fill. */
        volatile unsigned char *p=(void *)USER_STACK_BOTTOM_VIRT;
        while((uintptr_t)p<sp-16) *p++=0xa5;
        low=sp;signal_low=USER_STACK_TOP_VIRT;signals=0;armed=mode;
        int ok=pkg_test_main(5,verify_args);
        static char catalogs[3][40]={"/docs/stack-dynamic.cat","/docs/stack-fixed.cat","/docs/stack-stored.cat"};
        static char artifacts[3][40]={"/docs/stack-dynamic.fpk","/docs/stack-fixed.fpk","/docs/stack-stored.fpk"};
        for(unsigned i=0;i<3;i++) {
            verify_args[2]=catalogs[i];verify_args[4]=artifacts[i];
            if(pkg_test_main(5,verify_args)) ok=1;
        }
        verify_args[2]="/docs/catalog";verify_args[4]="/docs/fixture.fpk";
        int bad=pkg_test_main(5,bad_args);
        armed=0;
        p=(void *)USER_STACK_BOTTOM_VIRT;
        while((uintptr_t)p<sp-16 && *p==0xa5) p++;
        uintptr_t margin=(uintptr_t)p-USER_STACK_BOTTOM_VIRT;
        number(mode==1?"STACK nested_margin=":"STACK signal_watermark_margin=",margin);
        number("STACK sampled_margin=",low-USER_STACK_BOTTOM_VIRT);
        number("STACK signals=",signals);
        if(mode==2) number("STACK handler_margin=",signal_low-USER_STACK_BOTTOM_VIRT);
        if(ok || bad!=1 || failed || margin<4096 || low<USER_STACK_BOTTOM_VIRT+4096 ||
           (mode==2 && (!signals || signal_low>=low))) return 1;
    }
    return 0;
}
