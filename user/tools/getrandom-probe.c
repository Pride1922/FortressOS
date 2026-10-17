#include "common.h"

static int say(const char *s) { return tool_write("getrandom-probe",s,tool_length(s)); }
static long get(void *b,size_t n,uint64_t f) {
    return tool_syscall(SYS_GETRANDOM,(uintptr_t)b,n,f);
}
static void sample(const uint8_t b[32]) {
    static const char digits[]="0123456789abcdef";
    char line[128]; size_t n=0;
    const char *prefix="RANDOM sample cpu=";
    while (*prefix) line[n++]=*prefix++;
    uint32_t a,bx,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(bx),"=c"(c),"=d"(d):"a"(1),"c"(0));
    n+=tool_format_u64(line+n,bx>>24); line[n++]=' ';
    for(unsigned i=0;i<32;i++) {line[n++]=digits[b[i]>>4];line[n++]=digits[b[i]&15];}
    line[n++]='\n'; tool_write("getrandom-probe",line,n);
}
int getrandom_probe_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) {
        say("getrandom-probe [--worker|--workers|--unavailable]\nPublic random samples only; never use printed bytes as keys.\n"); return 0;
    }
    if (argc==2 && tool_equal(argv[1],"--workers")) {
        long pids[8]; int readers[8]; unsigned spawned=0; int result=0;
        const char *args[]={"getrandom-probe","--worker",NULL};
        for(unsigned i=0;i<8;i++) {
            int fds[2];
            if(tool_syscall(SYS_PIPE,(uintptr_t)fds,0,0)) {result=1;break;}
            spawn_fd_action_t actions[]={
                {.type=SPAWN_FD_ACTION_DUP2,.dst_fd=1,.src_fd=fds[1]},
                {.type=SPAWN_FD_ACTION_CLOSE,.dst_fd=fds[0]},
                {.type=SPAWN_FD_ACTION_CLOSE,.dst_fd=fds[1]}};
            spawn_opts_t opts={.size=sizeof(opts),.version=1,.argv=(uintptr_t)args,
                .fd_actions=(uintptr_t)actions,.action_count=3};
            pids[i]=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/getrandom-probe",(uintptr_t)&opts,sizeof(opts));
            tool_syscall(SYS_CLOSE,fds[1],0,0);
            if(pids[i]<0) {tool_syscall(SYS_CLOSE,fds[0],0,0);result=1;break;}
            readers[i]=fds[0]; spawned++;
        }
        /* Drain one retained pipe at a time to serialize terminal records.
         * Workers still generate concurrently on ordinary scheduler placement. */
        for(unsigned i=0;i<spawned;i++) {
            char data[512]; long n;
            while((n=tool_syscall(SYS_READ,readers[i],(uintptr_t)data,sizeof(data)))>0)
                if(tool_write("getrandom-probe",data,(size_t)n)) result=1;
            if(n<0) result=1;
            tool_syscall(SYS_CLOSE,readers[i],0,0);
            int64_t status=-1;
            if(tool_syscall(SYS_WAIT,pids[i],(uintptr_t)&status,0) || status) result=1;
        }
        if(result) return result;
        say("RANDOM workers PASS\n"); return 0;
    }
    uint8_t bytes[257];
    if (get(NULL,0,0)!=SYSCALL_EINVAL || get(NULL,257,0)!=SYSCALL_EINVAL ||
        get(NULL,32,1)!=SYSCALL_EINVAL || get(NULL,32,1ULL<<32)!=SYSCALL_EINVAL ||
        get(NULL,32,0)!=SYSCALL_EFAULT ||
        get((void *)0xffffffff80000000ULL,32,0)!=SYSCALL_EFAULT ||
        get((void *)(UINT64_MAX-8),32,0)!=SYSCALL_EFAULT ||
        get((void *)getrandom_probe_main,32,0)!=SYSCALL_EFAULT) return 1;
    if (argc==2 && tool_equal(argv[1],"--unavailable")) {
        for(unsigned i=0;i<257;i++) bytes[i]=0xa5;
        if(get(bytes,256,0)!=SYSCALL_EIO) return 1;
        for(unsigned i=0;i<257;i++) if(bytes[i]!=0xa5) return 1;
        say("RANDOM unavailable PASS\n"); return 0;
    }
    if(get(bytes,256,0)!=256) return 1;
    unsigned rounds=argc==2 && tool_equal(argv[1],"--worker") ? 32 : 2;
    for(unsigned i=0;i<rounds;i++) {
        if(get(bytes,32,0)!=32) return 1;
        sample(bytes);
        /* Give ordinary unpinned children time to run/steal on other CPUs. */
        for(volatile unsigned j=0;j<1000000;j++) {}
    }
    say("RANDOM probe PASS\n"); return 0;
}
