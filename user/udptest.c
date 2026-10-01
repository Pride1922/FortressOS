#include "udp_common.h"
static uint8_t s_received[NET_UDP_DATA_MAX];
static net_sockaddr_in_t s_peer, s_source;
static uint32_t s_source_size;
static char s_error[64];
static int fail(long error) {
    const char prefix[]="udptest: error "; size_t at=0;
    for (; prefix[at]; ++at) s_error[at]=prefix[at];
    unsigned n=(unsigned)(error<0 ? -error : error), digits=1, div=1;
    while (n/div>=10) { div*=10; ++digits; }
    while (digits--) { s_error[at++]=(char)('0'+n/div); n%=div; div/=10; }
    s_error[at++]='\n'; s_error[at]=0; udp_write(s_error); return 1;
}
int udptest_main(int argc, char **argv) {
    bool listen=argc==4 && udp_equal(argv[1],"--listen");
    unsigned port, count=1; uint32_t ip=0;
    const char *p;
    if (argc!=4) goto usage;
    if (listen) {
        p=argv[2]; if (!udp_number(&p,65535,&port) || *p || !port) goto usage;
        p=argv[3]; if (!udp_number(&p,100,&count) || *p || !count) goto usage;
    } else {
        if (!udp_ip(argv[1],&ip)) goto usage;
        p=argv[2]; if (!udp_number(&p,65535,&port) || *p || !port || udp_length(argv[3])>NET_UDP_DATA_MAX) goto usage;
    }
    s_peer=(net_sockaddr_in_t){.family=NET_AF_INET,.address=ip,.port=__builtin_bswap16((uint16_t)port)};
    long fd=udp_call(SYS_SOCKET,NET_AF_INET,NET_SOCK_DGRAM|NET_SOCK_CLOEXEC,0,0,0,0);
    if (fd<0) return fail(fd);
    long ret;
    if (listen) {
        ret=udp_call(SYS_BIND,fd,(uintptr_t)&s_peer,16,0,0,0);
        if (ret<0) goto error;
        if (!udp_write("UDP listener ready\n")) { ret=SYSCALL_EIO; goto error; }
    } else {
        ret=udp_call(SYS_SENDTO,fd,(uintptr_t)argv[3],udp_length(argv[3]),0,(uintptr_t)&s_peer,16);
        if (ret<0) goto error;
    }
    for (unsigned i=0; i<count; ++i) {
        s_source_size=16;
        ret=udp_call(SYS_RECVFROM,fd,(uintptr_t)s_received,sizeof(s_received),0,
            (uintptr_t)&s_source,(uintptr_t)&s_source_size);
        if (ret<0) goto error;
        if (listen) {
            ret=udp_call(SYS_SENDTO,fd,(uintptr_t)s_received,(size_t)ret,0,(uintptr_t)&s_source,16);
            if (ret<0) goto error;
            if (!udp_write("UDP echoed datagram\n")) { ret=SYSCALL_EIO; goto error; }
        } else {
            size_t n=udp_length(argv[3]);
            if ((size_t)ret!=n || s_source.family!=NET_AF_INET || s_source_size!=16 ||
                s_source.address!=s_peer.address || s_source.port!=s_peer.port) { ret=SYSCALL_EIO; goto error; }
            for (size_t j=0; j<n; ++j) if (s_received[j]!=(uint8_t)argv[3][j]) { ret=SYSCALL_EIO; goto error; }
        }
    }
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    return udp_write(listen ? "UDP listener PASS\n" : "UDP echo PASS\n") ? 0 : 1;
error:
    udp_call(SYS_CLOSE,fd,0,0,0,0,0); return fail(ret);
usage:
    udp_write("usage: udptest <IPv4> <port> <message> | udptest --listen <port> <1..100>\n"); return 2;
}
