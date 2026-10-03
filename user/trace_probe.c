/* Ring 3 fixture, included only in disposable traceroute test ISOs. */
#include "tools/common.h"
#include "trace_abi.h"
static const net_trace_v1_t readonly_request={.version=1};
int trace_probe_main(int argc,char **argv) {
    (void)argc; (void)argv;
    sysinfo_t info;
    if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)<0 || !info.tick_hz) return 1;
    net_trace_v1_t req={.version=1,.destination=__builtin_bswap32(0xc0000209),.ttl=1,
        .timeout_seconds=5,.sequence=99,.deadline_ticks=info.uptime_ticks+info.tick_hz};
    if (tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,0,64)!=SYSCALL_EFAULT ||
        tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&readonly_request,64)!=SYSCALL_EFAULT ||
        tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&req,63)!=SYSCALL_EINVAL) goto fail;
    req.reserved0=1;
    if (tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&req,64)!=SYSCALL_EINVAL || req.reserved0!=1) goto fail;
    req.reserved0=0; req.reserved1=1;
    if (tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&req,64)!=SYSCALL_EINVAL) goto fail;
    req.reserved1=0; req.deadline_ticks=0;
    if (tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&req,64)!=SYSCALL_ETIMEDOUT) goto fail;
    if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)<0) goto fail;
    req.deadline_ticks=info.uptime_ticks+(info.tick_hz/10 ? info.tick_hz/10:1);
    if (tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&req,64) || req.outcome!=NETTRACE_PROBE_TIMEOUT) goto fail;
    if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)<0 || info.uptime_ticks<req.deadline_ticks) goto fail;
    const char *pass="TRACE ABI deadline/range/reserved PASS\n";
    return tool_write("trace-probe",pass,tool_length(pass));
fail:
    return tool_error("trace-probe","ABI failure",NULL);
}
