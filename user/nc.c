#include "udp_common.h"
#include "terminal.h"
#include "dns.h"
#include "netconf.h"

/* Finite serial request/response: consume stdin, half-close, drain response.
 * A peer must consume the request before sending a large response. */
#ifndef NC_CALL
#define NC_CALL udp_call
#endif

#ifndef NETCONF_SYSCALL
#define NETCONF_SYSCALL(nr, a, b, c) NC_CALL(nr, a, b, c, 0, 0, 0)
#endif

static uint8_t buffer[4096];
static dns_context_t dns_context;
static dns_result_t dns_result;

static bool copy(long source, long destination) {
    for (;;) {
        long count=NC_CALL(SYS_READ,source,(uintptr_t)buffer,sizeof(buffer),0,0,0);
        if (count<0 || (size_t)count>sizeof(buffer)) return false;
        if (!count) return true;
        size_t at=0;
        while (at<(size_t)count) {
            long sent=NC_CALL(SYS_WRITE,destination,(uintptr_t)(buffer+at),(size_t)count-at,0,0,0);
            if (sent<=0 || (size_t)sent>(size_t)count-at) return false;
            at+=(size_t)sent;
        }
    }
}

static void message(const char *text) {
    size_t left=udp_length(text);
    while (left) {
        long n=NC_CALL(SYS_WRITE,2,(uintptr_t)text,left,0,0,0);
        if (n<=0 || (size_t)n>left) break;
        text+=n; left-=(size_t)n;
    }
}

static void print_usage(void) {
    message("usage: nc IPv4 port | nc host port | nc -s server-IPv4 host port | nc -l port (finite serial request/response)\n"
            "nc -l skips terminal stdin; piped or redirected stdin is sent before receiving.\n"
            "A peer that sends a large response before consuming the whole request can deadlock the serial nc; use small finite requests or a cooperating peer.\n");
}

int nc_main(int argc, char **argv) {
    bool listen=argc==3 && udp_equal(argv[1],"-l");
    unsigned port;
    uint32_t ip=0;
    bool has_s=argc==5 && udp_equal(argv[1],"-s");
    uint32_t server=0;
    const char *host=has_s ? argv[3] : argc==3 ? argv[1] : "";
    const char *number=has_s ? argv[4] : argc==3 ? argv[2] : "";
    if ((!has_s && argc!=3) || (has_s && !udp_ip(argv[2],&server)) ||
        !udp_number(&number,65535,&port) || *number || !port) {
        print_usage();
        return 1;
    }
    if (!listen && !udp_ip(host, &ip)) {
        if (!has_s && netconf_read_dns(0, &server) != 0) {
            message("nc: no DNS server specified (-s) and none found in /mnt/.fortress/network.conf\n");
            print_usage();
            return 1;
        }
        size_t length=0; while(length<=254 && host[length]) ++length;
        dns_options_t options={.server_ipv4=server}; dns_context_init(&dns_context);
        int status=dns_resolve_ipv4(&dns_context,&options,host,length,&dns_result);
        if(status) { message("nc: "); message(dns_status_name(status)); message("\n"); return 1; }
        ip=dns_result.addresses[0];
    }
    long fd=NC_CALL(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM|NET_SOCK_CLOEXEC,6,0,0,0), listener=-1;
    bool skip_stdin=false;
    if (fd<0) goto failure;
    net_sockaddr_in_t address={.family=NET_AF_INET,.port=__builtin_bswap16((uint16_t)port),.address=ip};
    if (listen) {
        listener=fd; fd=-1;
        long tty=NC_CALL(SYS_TERMCTL,TERM_ISATTY,0,0,0,0,0);
        if (tty<0) goto failure;
        skip_stdin=tty==1;
        if (NC_CALL(SYS_BIND,listener,(uintptr_t)&address,sizeof(address),0,0,0) ||
            NC_CALL(SYS_LISTEN,listener,1,0,0,0,0)) goto failure;
        fd=NC_CALL(SYS_ACCEPT,listener,0,0,NET_SOCK_CLOEXEC,0,0);
        NC_CALL(SYS_CLOSE,listener,0,0,0,0,0); listener=-1;
        if (fd<0) goto failure;
    } else if (NC_CALL(SYS_CONNECT,fd,(uintptr_t)&address,sizeof(address),0,0,0)) goto failure;
    if (!skip_stdin && (!copy(0,fd) || NC_CALL(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0))) goto failure;
    if (skip_stdin && NC_CALL(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0)) goto failure;
    if (!copy(fd,1)) goto failure;
    NC_CALL(SYS_CLOSE,fd,0,0,0,0,0);
    return 0;
failure:
    if (fd>=0) NC_CALL(SYS_CLOSE,fd,0,0,0,0,0);
    if (listener>=0) NC_CALL(SYS_CLOSE,listener,0,0,0,0,0);
    return 1;
}
