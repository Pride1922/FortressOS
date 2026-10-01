#include "udp_common.h"
static uint8_t s_data[1472], s_output[1472];
static net_sockaddr_in_t s_dest, s_source;
static uint32_t s_size;
static uint8_t s_cross[8192] __attribute__((aligned(4096)));
static const uint8_t s_readonly[16]={0};
static sysinfo_t s_info;
static volatile unsigned s_caught;
static void caught(unsigned sig) { if (sig==SIGINT) ++s_caught; }
static int caught_receive(void) {
    long fd=udp_call(SYS_SOCKET,2,2,0,0,0,0);
    if (fd<0) return 1;
    net_sockaddr_in_t local={.family=2,.port=__builtin_bswap16(7780)};
    signal_action_t action={.handler=(uintptr_t)caught};
    long result=udp_call(SYS_BIND,fd,(uintptr_t)&local,16,0,0,0);
    if (!result) result=udp_call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0,0,0,0);
    if (result) { udp_call(SYS_CLOSE,fd,0,0,0,0,0); return 1; }
    udp_write("UDP caught-signal receive ready\n");
    result=udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,1,0,0,0);
    bool pass=result==SYSCALL_EINTR && s_caught==1 &&
        udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN;
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    udp_write(pass ? "UDP caught-signal receive PASS\n" : "UDP caught-signal receive FAIL\n");
    return pass ? 0 : 1;
}
static char s_line[96];
static unsigned s_failed_line;
static void number(const char *prefix, uint64_t n) {
    size_t at=0;
    for (; prefix[at]; ++at) s_line[at]=prefix[at];
    char digits[21]; size_t count=0;
    do { digits[count++]=(char)('0'+n%10); n/=10; } while (n);
    while (count) s_line[at++]=digits[--count];
    s_line[at++]='\n'; s_line[at]=0; udp_write(s_line);
}
static long send(long fd, size_t n) {
    return udp_call(SYS_SENDTO,fd,(uintptr_t)s_data,n,0,(uintptr_t)&s_dest,16);
}
static long receive(long fd, size_t n, unsigned flags) {
    s_size=16;
    return udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,n,flags,(uintptr_t)&s_source,(uintptr_t)&s_size);
}
int net_udp_probe_main(int argc, char **argv) {
    if (argc==2 && udp_equal(argv[1],"--caught")) return caught_receive();
    if (argc==4 && udp_equal(argv[1],"check-fd")) {
        const char *p=argv[2]; unsigned fd;
        if (!udp_number(&p,31,&fd) || *p) return 1;
        net_sockaddr_in_t a={.family=2};
        long ret=udp_call(SYS_BIND,fd,(uintptr_t)&a,16,0,0,0);
        return ret==(udp_equal(argv[3],"closed") ? SYSCALL_EBADF : SYSCALL_EEXIST) ? 0 : 1;
    }
    if (argc!=3 || !udp_ip(argv[1],&s_dest.address)) goto bad;
    const char *p=argv[2]; unsigned port;
    if (!udp_number(&p,65535,&port) || *p || !port) goto bad;
    s_dest.family=2; s_dest.port=__builtin_bswap16((uint16_t)port);
    long fd=udp_call(SYS_SOCKET,2,2|NET_SOCK_CLOEXEC,17,0,0,0);
    if (fd<0) goto bad;
#define REQUIRE(x) do { if (!(x)) { s_failed_line=__LINE__; goto failure; } } while (0)
    REQUIRE(udp_call(SYS_SOCKET,2,0x100000002ull,0,0,0,0)==SYSCALL_EOPNOTSUPP);
    REQUIRE(udp_call(SYS_SENDTO,fd,0,1473,0,0,16)==SYSCALL_EINVAL);
    REQUIRE(udp_call(SYS_SENDTO,fd,0,1,0,(uintptr_t)&s_dest,16)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_BIND,fd,0,15,0,0,0)==SYSCALL_EINVAL);
    REQUIRE(udp_call(SYS_BIND,fd,0,16,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_BIND,fd,0xffffffff80000000ull,16,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_BIND,fd,~0ull-4,16,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_BIND,fd,0x7ffffffff000ull,16,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_BIND,fd,0x7ffffffffff8ull,16,0,0,0)==SYSCALL_EFAULT);
    /* Unaligned sockaddr spanning two mapped pages. */
    net_sockaddr_in_t cross_address={.family=NET_AF_INET};
    for (unsigned i=0; i<16; ++i) s_cross[4089+i]=((const uint8_t *)&cross_address)[i];
    long cross_fd=udp_call(SYS_SOCKET,2,2,0,0,0,0); REQUIRE(cross_fd>=0);
    REQUIRE(!udp_call(SYS_BIND,cross_fd,(uintptr_t)(s_cross+4089),16,0,0,0));
    udp_call(SYS_CLOSE,cross_fd,0,0,0,0,0);
    REQUIRE(udp_call(SYS_RECVFROM,fd,(uintptr_t)s_readonly,16,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_RECVFROM,fd,0,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EFAULT);
    REQUIRE(udp_call(SYS_RECVFROM,fd,0,0,1,0,0)==SYSCALL_EINVAL);
    REQUIRE(udp_call(SYS_RECVFROM,fd,0,0,0,(uintptr_t)&s_source,0)==SYSCALL_EINVAL);
    s_size=15;
    REQUIRE(udp_call(SYS_RECVFROM,fd,0,0,0,(uintptr_t)&s_source,(uintptr_t)&s_size)==SYSCALL_EINVAL);
    s_size=16;
    REQUIRE(udp_call(SYS_RECVFROM,fd,(uintptr_t)s_output,16,0,(uintptr_t)s_output,(uintptr_t)&s_size)==SYSCALL_EINVAL);
    REQUIRE(receive(fd,1,NET_MSG_DONTWAIT)==SYSCALL_EAGAIN);
    for (unsigned i=0; i<1472; ++i) s_data[i]=(uint8_t)(i*17);
    const unsigned sizes[]={0,1,9,1472};
    for (unsigned i=0; i<4; ++i) {
        unsigned n=sizes[i]; REQUIRE(send(fd,n)==(long)n); REQUIRE(receive(fd,1472,0)==(long)n);
        REQUIRE(s_source.address==s_dest.address && s_source.port==s_dest.port && s_size==16);
        for (unsigned j=0; j<n; ++j) REQUIRE(s_data[j]==s_output[j]);
    }
    REQUIRE(send(fd,9)==9 && receive(fd,3,0)==3);
    REQUIRE(receive(fd,1,NET_MSG_DONTWAIT)==SYSCALL_EAGAIN);
    REQUIRE(send(fd,1)==1);
    /* Fault must not consume the queued reply; writable retry succeeds. */
    REQUIRE(udp_call(SYS_RECVFROM,fd,(uintptr_t)s_readonly,1,0,0,0)==SYSCALL_EFAULT);
    REQUIRE(receive(fd,1,0)==1);
    long duplicate=udp_call(SYS_DUP,fd,0,0,0,0,0);
    REQUIRE(duplicate>=0); udp_call(SYS_CLOSE,fd,0,0,0,0,0); fd=duplicate;
    REQUIRE(send(fd,1)==1 && receive(fd,1,0)==1);
    /* Shared descriptor inheritance, then CLOEXEC sweep, using real spawn. */
    char fdtext[3]; unsigned n=(unsigned)fd;
    if (n>=10) { fdtext[0]=(char)('0'+n/10); fdtext[1]=(char)('0'+n%10); fdtext[2]=0; }
    else { fdtext[0]=(char)('0'+n); fdtext[1]=0; }
    const char *childargs[]={"/bin/net-udp-probe","check-fd",fdtext,"open",NULL};
    long child=udp_call(SYS_SPAWN,(uintptr_t)childargs[0],(uintptr_t)childargs,0,0,0,0);
    REQUIRE(child>0); int64_t status=-1;
    REQUIRE(!udp_call(SYS_WAIT,child,(uintptr_t)&status,0,0,0,0) && status==0);
    REQUIRE(!udp_call(SYS_FCNTL,fd,F_SETFD,FD_CLOEXEC,0,0,0)); childargs[3]="closed";
    child=udp_call(SYS_SPAWN,(uintptr_t)childargs[0],(uintptr_t)childargs,0,0,0,0);
    REQUIRE(child>0); status=-1;
    REQUIRE(!udp_call(SYS_WAIT,child,(uintptr_t)&status,0,0,0,0) && status==0);
    long sockets[15]; unsigned used=0;
    for (; used<15; ++used) { sockets[used]=udp_call(SYS_SOCKET,2,2,0,0,0,0); REQUIRE(sockets[used]>=0); }
    REQUIRE(udp_call(SYS_SOCKET,2,2,0,0,0,0)==SYSCALL_ENOSPC);
    for (unsigned i=0; i<used; ++i) udp_call(SYS_CLOSE,sockets[i],0,0,0,0,0);
    REQUIRE(!udp_call(SYS_SYSINFO,(uintptr_t)&s_info,0,0,0,0,0));
    uint64_t before=s_info.uptime_ticks, hz=s_info.tick_hz;
    REQUIRE(receive(fd,1,0)==SYSCALL_EAGAIN);
    REQUIRE(!udp_call(SYS_SYSINFO,(uintptr_t)&s_info,0,0,0,0,0));
    REQUIRE(s_info.uptime_ticks-before>=5*hz);
    number("UDP receive timeout BSP ticks: ",s_info.uptime_ticks-before);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    udp_write("UDP ABI/binary/fd probe PASS\n"); return 0;
failure:
    number("UDP probe failed at line ",s_failed_line);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
bad:
    udp_write("UDP ABI/binary/fd probe FAIL\n"); return 1;
}
