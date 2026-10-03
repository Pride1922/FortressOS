#include "tools/common.h"
#include "trace_abi.h"

static char text[256];
static size_t used;
static void str(const char *p) { while (*p && used<sizeof(text)) text[used++]=*p++; }
static void number(uint64_t n) {
    if (n>=10) number(n/10);
    if (used<sizeof(text)) text[used++]=(char)('0'+n%10);
}
static void ip_text(uint32_t ip) {
    uint32_t h=__builtin_bswap32(ip);
    for (unsigned i=0;i<4;i++) { if (i) str("."); number((h>>(24-i*8))&255); }
}
static bool decimal(const char *s, unsigned limit, unsigned *value) {
    unsigned n=0;
    if (!*s) return false;
    for (;*s;s++) {
        if (*s<'0' || *s>'9') return false;
        unsigned digit=(unsigned)(*s-'0');
        if (n>limit/10 || (n==limit/10 && digit>limit%10)) return false;
        n=n*10+digit;
    }
    if (!n) return false;
    *value=n; return true;
}
static bool address(const char *s, uint32_t *ip) {
    uint32_t h=0;
    for (unsigned i=0;i<4;i++) {
        unsigned n=0, digits=0;
        while (*s>='0' && *s<='9') {
            if (++digits>3) return false;
            n=n*10+(unsigned)(*s++-'0'); if (n>255) return false;
        }
        if (!digits || (i<3 && *s++!='.')) return false;
        h=(h<<8)|n;
    }
    if (*s || !(h>>24) || (h>>24)==127 || (h>>24)>=224) return false;
    *ip=__builtin_bswap32(h); return true;
}
static int flush(void) {
    int ret=tool_write("traceroute",text,used); used=0; return ret;
}
int traceroute_main(int argc, char **argv) {
    unsigned hops=30, probes=3, seconds=1;
    const char *destination=NULL;
    used=0; tool_output_failed=false;
    if (argc==2 && tool_equal(argv[1],"--help")) {
        str("Usage: traceroute [-m 1..30] [-q 1..3] [-W 1..5] IPV4\n");
        str("Numeric ICMP trace; default 30 hops, 3 probes, 1 second. Total budget 120 seconds.\n");
        str("One line per probe. Busy ping/trace: report and exit; no retry.\n");
        return flush();
    }
    for (int i=1;i<argc;i++) {
        if (tool_equal(argv[i],"-m") || tool_equal(argv[i],"-q") || tool_equal(argv[i],"-W")) {
            char option=argv[i][1]; unsigned value;
            if (++i==argc || !decimal(argv[i],option=='m' ? 30:option=='q' ? 3:5,&value)) goto usage;
            if (option=='m') hops=value; else if (option=='q') probes=value; else seconds=value;
        } else if (!destination && argv[i][0]!='-') destination=argv[i]; else goto usage;
    }
    uint32_t ip;
    if (!destination || !address(destination,&ip)) goto usage;
    sysinfo_t info;
    if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)<0 || !info.tick_hz ||
        info.tick_hz>UINT64_MAX/NETTRACE_HORIZON_SECONDS ||
        info.uptime_ticks>UINT64_MAX-NETTRACE_HORIZON_SECONDS*info.tick_hz)
        return tool_error("traceroute","clock unavailable",NULL);
    const uint64_t deadline=info.uptime_ticks+NETTRACE_HORIZON_SECONDS*info.tick_hz;
    str("traceroute to "); ip_text(ip); str(", "); number(hops); str(" hops max\n");
    if (flush()) return 1;
    uint32_t sequence=0;
    for (unsigned ttl=1;ttl<=hops;ttl++) for (unsigned probe=0;probe<probes;probe++) {
        if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0)<0)
            return tool_error("traceroute","clock unavailable",NULL);
        if (info.uptime_ticks>=deadline) return tool_error("traceroute","command deadline reached",NULL);
        net_trace_v1_t request={.version=1,.destination=ip,.ttl=ttl,.timeout_seconds=seconds,
            .sequence=++sequence,.deadline_ticks=deadline};
        long ret=tool_syscall(SYS_NETCTL,NETCTL_TRACE_PROBE,(uintptr_t)&request,sizeof(request));
        if (ret<0) {
            if (ret==SYSCALL_EAGAIN) return tool_error("traceroute","ping/trace busy; retry manually",NULL);
            if (ret==SYSCALL_ETIMEDOUT) return tool_error("traceroute","command deadline reached",NULL);
            return tool_error("traceroute","probe control failed",NULL);
        }
        number(ttl); str("  ");
        if (request.outcome<=NETTRACE_UNREACHABLE && request.tick_hz) {
            ip_text(request.responder); str("  ");
            /* Avoid overflowing rtt_ticks*1000 even on a changed timebase. */
            uint64_t whole=request.rtt_ticks/request.tick_hz, rem=request.rtt_ticks%request.tick_hz;
            if (whole>UINT64_MAX/1000 || rem>UINT64_MAX/1000)
                return tool_error("traceroute","invalid RTT",NULL);
            number(whole*1000+rem*1000/request.tick_hz); str(" ms");
            if (request.outcome==NETTRACE_UNREACHABLE) {
                static const char *const flags[]={" !N"," !H"," !P"," !PORT"," !FRAG"," !ROUTE"};
                if (request.icmp_code>5) return tool_error("traceroute","invalid unreachable result",NULL);
                str(flags[request.icmp_code]);
            }
        } else str("*");
        str("\n"); if (flush()) return 1;
        if (request.outcome==NETTRACE_REPLY) return 0;
        if (request.outcome==NETTRACE_UNREACHABLE) return 1;
        if (request.outcome==NETTRACE_TX_FAILED) return tool_error("traceroute","transmit failed",NULL);
        /* Re-read the clock before the next probe; no fresh budget or EAGAIN retry. */
    }
    return 1;
usage:
    tool_error("traceroute","use traceroute [-m 1..30] [-q 1..3] [-W 1..5] IPV4",NULL);
    return 2;
}
