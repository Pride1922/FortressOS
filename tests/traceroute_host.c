#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "trace_abi.h"
int traceroute_main(int argc,char **argv);
static unsigned calls, clock_calls, mode;
static uint64_t now, deadline;
static char out[8192], err[4096];
static size_t out_n, err_n;
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_SYSINFO) {
        assert(b==sizeof(sysinfo_t)); clock_calls++;
        *(sysinfo_t *)a=(sysinfo_t){.uptime_ticks=now,.tick_hz=100}; return 0;
    }
    if (nr==SYS_NETCTL) {
        assert(a==NETCTL_TRACE_PROBE && c==64);
        net_trace_v1_t *r=(void *)b; calls++;
        assert(r->sequence==calls && r->ttl==(calls-1)/3+1);
        assert(!r->reserved0 && !r->reserved1 && !r->outcome && !r->responder && !r->rtt_ticks && !r->tick_hz);
        if (!deadline) deadline=r->deadline_ticks;
        assert(deadline==12000 && r->deadline_ticks==deadline);
        if (mode==1) return SYSCALL_EAGAIN;
        if (mode==2) { now=deadline; r->outcome=NETTRACE_PROBE_TIMEOUT; return 0; }
        if (mode==3) return SYSCALL_EINTR;
        if (mode==4) { r->outcome=NETTRACE_UNREACHABLE; r->icmp_code=1; }
        else r->outcome=calls==7 ? NETTRACE_REPLY:NETTRACE_HOP_EXPIRED;
        r->responder=__builtin_bswap32(0xc0000201u); r->tick_hz=100; r->rtt_ticks=2; now+=2;
        return 0;
    }
    if (nr==SYS_WRITE) {
        assert(a==1 || a==2);
        size_t n=c>5 ? 5:c;
        char *p=a==1 ? out:err;
        size_t *used=a==1 ? &out_n:&err_n;
        assert(*used+n+1<(a==1 ? sizeof(out):sizeof(err)));
        memcpy(p+*used,(void *)b,n); *used+=n; p[*used]=0; return (long)n;
    }
    assert(!"unexpected syscall"); return -1;
}
static void reset(unsigned m) { mode=m; calls=clock_calls=0; now=deadline=0; out_n=err_n=0; out[0]=err[0]=0; }
int main(void) {
    char *args[]={"traceroute","192.0.2.1"};
    reset(0); assert(!traceroute_main(2,args) && calls==7);
    assert(strstr(out,"1  192.0.2.1  20 ms\n") && strstr(out,"3  192.0.2.1  20 ms\n"));
    reset(1); assert(traceroute_main(2,args)==1 && calls==1 && strstr(err,"busy; retry manually"));
    reset(2); assert(traceroute_main(2,args)==1 && calls==1 && strstr(out,"1  *\n") && strstr(err,"deadline reached"));
    reset(3); assert(traceroute_main(2,args)==1 && calls==1);
    reset(4); assert(traceroute_main(2,args)==1 && calls==1 && strstr(out," !H\n"));
    char *bad[]={"traceroute","-q","4","192.0.2.1"};
    reset(0); assert(traceroute_main(4,bad)==2 && !calls && !clock_calls);
    char *hostname[]={"traceroute","example.com"};
    reset(0); assert(traceroute_main(2,hostname)==2 && !calls);
    char *help[]={"traceroute","--help"};
    reset(0); assert(!traceroute_main(2,help) && !calls && strstr(out,"Total budget 120"));
    puts("Traceroute CLI host PASS: distinct labels, one line/probe, fixed deadline, mid-probe cutoff, no EAGAIN retry and short writes");
}
