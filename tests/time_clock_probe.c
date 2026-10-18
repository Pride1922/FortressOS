/* Acceptance-only Ring 3 syscall probe; never packaged on release media. */
#include "../user/udp_common.h"
static bool get(unsigned id,timespec_t *t) {return udp_call(SYS_CLOCK_GETTIME,id,(uintptr_t)t,0,0,0,0)==0;}
static long set(uint64_t id,const timespec_t *t) {return udp_call(SYS_CLOCK_SETTIME,id,(uintptr_t)t,0,0,0,0);}
int time_clock_probe_main(void) {
    timespec_t original,before,after,target={1893456000,123456789};
    if(!get(CLOCK_REALTIME,&original)||!get(CLOCK_MONOTONIC,&before))goto fail;
    if(set(CLOCK_MONOTONIC,&target)!=SYSCALL_EINVAL || set(1ULL<<32,&target)!=SYSCALL_EINVAL)goto fail;
    if(set(CLOCK_REALTIME,NULL)!=SYSCALL_EFAULT)goto fail;
    if(set(CLOCK_REALTIME,(const timespec_t *)0x00007fffffffffffULL)!=SYSCALL_EFAULT)goto fail;
    if(set(CLOCK_REALTIME,(const timespec_t *)0xffffffff80000000ULL)!=SYSCALL_EFAULT)goto fail;
    target.tv_nsec=1000000000;
    if(set(CLOCK_REALTIME,&target)!=SYSCALL_EINVAL)goto fail;
    target.tv_nsec=-1;
    if(set(CLOCK_REALTIME,&target)!=SYSCALL_EINVAL)goto fail;
    target=(timespec_t){-1,0};if(set(CLOCK_REALTIME,&target)!=SYSCALL_EINVAL)goto fail;
    target=(timespec_t){4133980800LL,0};if(set(CLOCK_REALTIME,&target)!=SYSCALL_EINVAL)goto fail;
    if(!get(CLOCK_REALTIME,&after)||after.tv_sec<original.tv_sec||after.tv_sec>original.tv_sec+2)goto fail;
    target=(timespec_t){1893456000,123456789};if(set(CLOCK_REALTIME,&target))goto fail;
    if(!get(CLOCK_REALTIME,&after)||after.tv_sec<target.tv_sec||after.tv_sec>target.tv_sec+1)goto fail;
    target=(timespec_t){1672531200,0};if(set(CLOCK_REALTIME,&target))goto fail;
    if(!get(CLOCK_MONOTONIC,&after)||after.tv_sec<before.tv_sec||after.tv_sec>before.tv_sec+3)goto fail;
    if(set(CLOCK_REALTIME,&original))goto fail;
    if(udp_call(SYS_CAPSET,0,0,0,0,0,0)||set(CLOCK_REALTIME,&target)!=SYSCALL_EPERM)goto fail;
    udp_write("CLOCK_PROBE_PASS\n");return 0;
fail:udp_write("CLOCK_PROBE_FAIL\n");return 1;
}
