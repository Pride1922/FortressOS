/* Snapshot-only AP probe. Reuse the actual production UDP ABI regression and
 * Phase 2A controls rather than inventing a separate protocol path. */
#define net_cpu_probe_main control_probe_main
#include "net_control_guest_user.c"
#undef net_cpu_probe_main
#include "../user/net_udp_probe.c"
#include "../user/dns_codec.c"
#include "../user/dns.c"
static dns_context_t dns_context;
static dns_result_t dns_result;
int net_cpu_probe_main(int argc,char **argv) {
    char mode=argc>1 ? argv[argc-1][0] : 'a';
    if (mode=='l' || mode=='h') return call(0x7ff2,mode=='l' ? 3 : 4,0,0) ? 27 : 0;
    if (mode=='u') {
        char *args[]={"net-udp-probe","10.0.2.2","17777"};
        return net_udp_probe_main(3,args);
    }
    if (mode=='w') {
        long fd=udp_call(SYS_SOCKET,2,2,17,0,0,0);
        if (fd<0) return 20;
        long ret=udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,1,0,0,0);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        return ret==SYSCALL_EAGAIN || ret==SYSCALL_EINTR ? 0 : 21;
    }
    if (mode=='v') {
        long fd=udp_call(SYS_SOCKET,2,2,17,0,0,0);
        if (fd<0) return 22;
        net_sockaddr_in_t dest={.family=2,.port=__builtin_bswap16(17777),.address=ip(2)};
        for (unsigned i=0;i<32;++i) {
            s_data[0]=(uint8_t)i;
            if (udp_call(SYS_SENDTO,fd,(uintptr_t)s_data,1,0,(uintptr_t)&dest,16)!=1 ||
                udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,1,0,0,0)!=1 || s_output[0]!=s_data[0]) return 23;
        }
        udp_call(SYS_CLOSE,fd,0,0,0,0,0); return 0;
    }
    if (mode=='d') {
        dns_options_t options={.server_ipv4=ip(2)};
        dns_context_init(&dns_context);
        if (dns_resolve_ipv4(&dns_context,&options,"service.test",12,&dns_result)!=DNS_OK ||
            dns_result.addresses[0]!=ip(2)) return 24;
        if (dns_resolve_ipv4(&dns_context,&options,"alias.test",10,&dns_result)!=DNS_OK ||
            dns_result.addresses[0]!=ip(2)) return 25;
        if (dns_resolve_ipv4(&dns_context,&options,"missing.test",12,&dns_result)!=DNS_NXDOMAIN) return 26;
        return 0;
    }
    return control_probe_main(argc,argv);
}
