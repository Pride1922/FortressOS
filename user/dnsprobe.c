/* Finite Ring 3 deadline/signal fixture, not a general-purpose resolver tool. */
#include "dns.h"
#include "udp_common.h"
static dns_context_t context;
static dns_result_t result;
static volatile unsigned caught_count;
static void caught(unsigned signal) { if(signal==SIGINT) ++caught_count; }
int dnsprobe_main(int argc,char **argv) {
    dns_options_t opt={0};
    if(argc<3 || argc>4 || !udp_ip(argv[1],&opt.server_ipv4)) return 1;
    bool catching=argc==4 && udp_equal(argv[3],"--caught");
    if(argc==4 && !catching) return 1;
    if(catching) {
        signal_action_t action={.handler=(uintptr_t)caught};
        if(udp_call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0,0,0,0)) return 1;
    }
    sysinfo_t info;
    if(udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0) || !info.tick_hz ||
       info.tick_hz>UINT64_MAX/5 || info.uptime_ticks>UINT64_MAX-5*info.tick_hz) return 1;
    opt.deadline_ticks=info.uptime_ticks+5*info.tick_hz;
    dns_context_init(&context); udp_write("DNS probe waiting\n");
    size_t len=0; while(len<=254 && argv[2][len]) ++len;
    int status=dns_resolve_ipv4(&context,&opt,argv[2],len,&result);
    udp_write("DNS probe "); udp_write(dns_status_name(status)); udp_write("\n");
    if(catching && (status!=DNS_INTERRUPTED || caught_count!=1)) return 1;
    return status==DNS_TIMEOUT || status==DNS_INTERRUPTED ? 0 : 1;
}
