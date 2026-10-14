/* Replaces only the probe in disposable installed-root preparation. */
#include "types.h"
#include "syscall_abi.h"
static long call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a),"S"(b),"d"(c) : "rcx","r11","memory","cc");
    return nr;
}
int net_cpu_probe_main(int argc,char **argv) {
    unsigned mode=0;
    if (argc>1) {
        const char *p=argv[argc-1];
        if (*p>='a' && *p<='o') mode=(unsigned)(*p-'a');
    }
    const char *start="HANDOFF CALL START\n";
    if (mode!=4) call(SYS_WRITE,1,(uintptr_t)start,19);
    long rc=call(0x7ff0,mode,0,0);
    const char *msg=rc ? "HANDOFF CALL FAIL\n" : "HANDOFF CALL PASS\n";
    if (mode!=4 || rc) call(SYS_WRITE,1,(uintptr_t)msg,18);
    return rc ? 1 : 0;
}
