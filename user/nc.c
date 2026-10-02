#include "udp_common.h"
/* Finite serial request/response: consume stdin, half-close, drain response.
 * A peer must consume the request before sending a large response. */
#ifndef NC_CALL
#define NC_CALL udp_call
#endif
static uint8_t buffer[4096];

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

int nc_main(int argc, char **argv) {
    bool listen=argc==3 && udp_equal(argv[1],"-l");
    unsigned port;
    uint32_t ip=0;
    const char *number=argc==3 ? argv[2] : "";
    if (argc!=3 || (!listen && !udp_ip(argv[1],&ip)) ||
        !udp_number(&number,65535,&port) || *number || !port) {
        message("usage: nc IPv4 port | nc -l port (finite serial request/response)\n"
                "A peer that sends a large response before consuming the whole request can deadlock the serial nc; use small finite requests or a cooperating peer.\n"); return 1;
    }
    long fd=NC_CALL(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM|NET_SOCK_CLOEXEC,6,0,0,0), listener=-1;
    if (fd<0) goto failure;
    net_sockaddr_in_t address={.family=NET_AF_INET,.port=__builtin_bswap16((uint16_t)port),.address=ip};
    if (listen) {
        listener=fd; fd=-1;
        if (NC_CALL(SYS_BIND,listener,(uintptr_t)&address,sizeof(address),0,0,0) ||
            NC_CALL(SYS_LISTEN,listener,1,0,0,0,0)) goto failure;
        fd=NC_CALL(SYS_ACCEPT,listener,0,0,NET_SOCK_CLOEXEC,0,0);
        NC_CALL(SYS_CLOSE,listener,0,0,0,0,0); listener=-1;
        if (fd<0) goto failure;
    } else if (NC_CALL(SYS_CONNECT,fd,(uintptr_t)&address,sizeof(address),0,0,0)) goto failure;
    if (!copy(0,fd) || NC_CALL(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0) || !copy(fd,1)) goto failure;
    NC_CALL(SYS_CLOSE,fd,0,0,0,0,0); return 0;
failure:
    if (fd>=0) NC_CALL(SYS_CLOSE,fd,0,0,0,0,0);
    if (listener>=0) NC_CALL(SYS_CLOSE,listener,0,0,0,0,0);
    message("nc: socket or I/O failure\n"); return 1;
}
