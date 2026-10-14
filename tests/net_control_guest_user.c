/* Installed fixture only; never packaged in the release initramfs. */
#include "types.h"
#include "syscall_abi.h"
#include "netctl_abi.h"
#include "socket_abi.h"
static long call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a),"S"(b),"d"(c) : "rcx","r11","memory","cc");
    return nr;
}
static uint32_t ip(unsigned last) { return (last<<24)|0x0002000a; }
int net_cpu_probe_main(int argc,char **argv) {
    char mode=argc>1 ? argv[argc-1][0] : 'a';
    long rc=0;
    netctl_ifget_t get={.struct_version=1};
    if (mode=='a' || mode=='c') {
        if (call(SYS_NETCTL,NETCTL_IFGET,0,1)!=SYSCALL_EINVAL ||
            call(SYS_NETCTL,NETCTL_IFGET,0,sizeof(get))!=SYSCALL_EFAULT) return 1;
        static const netctl_ifget_t readonly={.struct_version=1};
        if (call(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&readonly,sizeof(readonly))!=SYSCALL_EFAULT ||
            call(SYS_NETCTL,NETCTL_IFGET,0x7fffffffffe0ULL,sizeof(get))!=SYSCALL_EFAULT) return 14;
        get.reserved=1;
        if (call(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&get,sizeof(get))!=SYSCALL_EINVAL) return 2;
        get.reserved=0;
        rc=call(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&get,sizeof(get));
        if (rc || get.mtu!=1500 || get.netmask_ipv4!=0x00ffffff) return 3;
        if (mode=='a') {
            if (call(SYS_NETCTL,NETCTL_IFSET,0,0)!=SYSCALL_EPERM) return 4;
        } else {
            if (call(SYS_NETCTL,NETCTL_IFSET,0,0)!=SYSCALL_EINVAL ||
                call(SYS_NETCTL,NETCTL_IFSET,0,sizeof(netctl_ifset_t))!=SYSCALL_EFAULT) return 5;
            netctl_ifset_t set={.struct_version=1,.local_ipv4=ip(50),
                .netmask_ipv4=0x00ffffff,.gateway_ipv4=ip(2)};
            for (unsigned i=0;i<32;++i) {
                set.local_ipv4=ip(50+i%2);
                if (call(SYS_NETCTL,NETCTL_IFSET,(uintptr_t)&set,sizeof(set))) return 6;
            }
        }
    } else if (mode=='p' || mode=='e') {
        net_ping_v1_t ping={.version=1,.destination=ip(mode=='p' ? 2 : 123),
            .timeout_seconds=5,.start_delay_ms=mode=='e' ? 1000 : 0};
        rc=call(SYS_NETCTL,NETCTL_PING,(uintptr_t)&ping,sizeof(ping));
        if (mode=='p' && (!rc && ping.outcome!=NETPING_REPLY)) return 7;
        if (rc!=0 && rc!=SYSCALL_EAGAIN && rc!=SYSCALL_EINTR) return 8;
    } else if (mode=='t') {
        sysinfo_t info={0};
        if (call(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)) return 13;
        uint64_t ticks=info.uptime_ticks;
        net_trace_v1_t trace={.version=1,.destination=ip(2),.ttl=1,
            .timeout_seconds=1,.deadline_ticks=ticks+3000};
        rc=call(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&trace,sizeof(trace));
        if (rc) return 9;
    } else if (mode=='g' || mode=='m') rc=call(0x7ff2,mode=='g' ? 0 : 1,0,0);
    else if (mode=='r') {
        for (unsigned i=0;i<64;++i) {
            get=(netctl_ifget_t){.struct_version=1};
            if (call(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&get,sizeof(get))) return 10;
        }
    } else return 11;
    return rc ? 12 : 0;
}
