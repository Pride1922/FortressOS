#include "syscall_abi.h"
#include "tests/pkg_backend_candidate.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){__asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");return n;}
static void need(bool ok){if(!ok){const char s[]="PKGBOOT RING3 FAIL\n";raw(SYS_WRITE,2,(uintptr_t)s,sizeof s-1);raw(SYS_EXIT,1,0,0);__builtin_trap();}}
static long call(unsigned op,uint64_t session,pb_result *r){pb_request q={.size=32,.version=1,.operation=op,.session=session};return raw(200,(uintptr_t)&q,(uintptr_t)r,sizeof q);}
static void number(uint64_t n){char b[24];unsigned at=sizeof b;do{b[--at]='0'+n%10;n/=10;}while(n);raw(SYS_WRITE,1,(uintptr_t)(b+at),sizeof b-at);}
int pkg_call_main(int argc,char **argv){
    (void)argv;pb_result r;
    if(argc>1){need(call(PB_STATUS,0,&r)==SYSCALL_EOPNOTSUPP);need(call(PB_ACQUIRE,0,&r)==SYSCALL_EOPNOTSUPP);need(!raw(201,13,0,0)&&!raw(201,14,0,0));}
    else for(unsigned i=0;i<2;i++){
        need(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==1);
        need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;
        need(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==1);
        need(!call(PB_RELEASE,session,&r));
    }
    const char counter[]="PKGBOOT COUNTERS writes=",flush[]=" flushes=";
    raw(SYS_WRITE,1,(uintptr_t)counter,sizeof counter-1);number(raw(201,13,0,0));
    raw(SYS_WRITE,1,(uintptr_t)flush,sizeof flush-1);number(raw(201,14,0,0));raw(SYS_WRITE,1,(uintptr_t)"\n",1);
    const char pass[]="PKGBOOT RING3 PASS\n";raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);return 0;
}
