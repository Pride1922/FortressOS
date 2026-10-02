#include "dns.h"
#include "dns_codec.h"
#include "udp_common.h"
#ifndef NSLOOKUP_CALL
#define NSLOOKUP_CALL udp_call
#endif
static dns_context_t context;
static dns_result_t result;
static bool write_text(long fd,const char *p) {
    size_t len=dns_length(p),at=0;
    while(at<len) {
        long n=NSLOOKUP_CALL(SYS_WRITE,(uintptr_t)fd,(uintptr_t)(p+at),len-at,0,0,0);
        if(n<=0 || (size_t)n>len-at) return false;
        at+=(size_t)n;
    }
    return true;
}
int nslookup_main(int argc,char **argv) {
    dns_options_t options={0};
    if(argc!=4 || !udp_equal(argv[1],"-s") || !udp_ip(argv[2],&options.server_ipv4)) {
        (void)write_text(2,"usage: nslookup -s IPv4 name\n"); return 1;
    }
    size_t len=0; while(len<=254 && argv[3][len]) ++len;
    dns_context_init(&context);
    int status=dns_resolve_ipv4(&context,&options,argv[3],len,&result);
    if(status) {
        (void)write_text(2,"nslookup: "); (void)write_text(2,dns_status_name(status));
        (void)write_text(2,"\n"); return 1;
    }
    char ip[16]; dns_format_ipv4(options.server_ipv4,ip);
    if(!write_text(1,"Server: ") || !write_text(1,ip) || !write_text(1,"\nName: ") ||
       !write_text(1,result.canonical_name) || !write_text(1,"\n")) return 1;
    for(unsigned i=0;i<result.address_count;++i) {
        dns_format_ipv4(result.addresses[i],ip);
        if(!write_text(1,"Address: ") || !write_text(1,ip) || !write_text(1,"\n")) return 1;
    }
    return 0;
}
