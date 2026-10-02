#include "udp_common.h"
static uint8_t bytes[16384], cross[8192] __attribute__((aligned(4096)));
static const uint8_t readonly[16]={0};
static volatile unsigned caught_count;
static void caught(unsigned sig) { if (sig==SIGINT) ++caught_count; }
static void error(long value) {
    char text[40]="tcptest: error "; unsigned at=15;
    char digits[21]; unsigned n=0; uint64_t v=(uint64_t)(value<0 ? -value : value);
    do { digits[n++]=(char)('0'+v%10); v/=10; } while (v);
    while (n) text[at++]=digits[--n];
    text[at++]='\n'; text[at]=0; udp_write(text);
}
int tcptest_main(int argc, char **argv) {
    if (argc<3 || argc>4) { udp_write("usage: tcptest IPv4 port [--hold|--caught|--reset|--unread]\n"); return 1; }
    net_sockaddr_in_t address={.family=2}; unsigned port; const char *p=argv[2];
    if (!udp_ip(argv[1],&address.address) || !udp_number(&p,65535,&port) || *p || !port) return 1;
    address.port=__builtin_bswap16((uint16_t)port);
    long fd=udp_call(SYS_SOCKET,2,NET_SOCK_STREAM|NET_SOCK_CLOEXEC,6,0,0,0);
    if (fd<0) { error(fd); return 1; }
    long result=0; unsigned line=0;
#define CHECK(x) do { if (!(x)) { line=__LINE__; goto failure; } } while (0)
    CHECK(udp_call(SYS_CONNECT,fd,0,15,0,0,0)==SYSCALL_EINVAL);
    CHECK(udp_call(SYS_CONNECT,fd,0,16,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_CONNECT,fd,~0ull-4,16,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_CONNECT,fd,0xffffffff80000000ull,16,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_CONNECT,fd,0x7ffffffffff8ull,16,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_SEND,fd,0,0x100000000ull,0,0,0)==SYSCALL_EINVAL);
    CHECK(udp_call(SYS_RECV,fd,(uintptr_t)readonly,1,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_LISTEN,fd,1,0,0,0,0)==SYSCALL_EOPNOTSUPP);
    for (unsigned i=0; i<16; ++i) cross[4089+i]=((const uint8_t *)&address)[i];
    bool connect_caught=argc==4 && udp_equal(argv[3],"--connect-caught");
    if (connect_caught) {
        signal_action_t action={.handler=(uintptr_t)caught};
        CHECK(!udp_call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0,0,0,0));
        udp_write("TCP caught connect ready\n");
    }
    result=udp_call(SYS_CONNECT,fd,(uintptr_t)(cross+4089),16,0,0,0);
    if (connect_caught) {
        CHECK(result==SYSCALL_EINTR && caught_count==1);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0); udp_write("TCP caught connect PASS\n"); return 0;
    }
    if (result<0) goto failure;
    udp_write("TCP client connected\n");
    CHECK(udp_call(SYS_CONNECT,fd,(uintptr_t)&address,16,0,0,0)==SYSCALL_EISCONN);
    CHECK(udp_call(SYS_SEND,fd,0,0,0,0,0)==0);
    CHECK(udp_call(SYS_READ,0x100000000ull+(uint64_t)fd,(uintptr_t)bytes,1,0,0,0)==SYSCALL_EBADF);
    long duplicate=udp_call(SYS_DUP,fd,0,0,0,0,0); CHECK(duplicate>=0);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0); fd=duplicate;
    if (argc==4 && (udp_equal(argv[3],"--hold") || udp_equal(argv[3],"--caught"))) {
        bool catching=udp_equal(argv[3],"--caught");
        if (catching) {
            signal_action_t action={.handler=(uintptr_t)caught};
            CHECK(!udp_call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0,0,0,0));
        }
        udp_write(catching ? "TCP caught receive ready\n" : "TCP indefinite receive ready\n");
        result=udp_call(SYS_RECV,fd,(uintptr_t)bytes,1,0,0,0);
        CHECK(catching ? result==SYSCALL_EINTR && caught_count==1 : result==0);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        udp_write(catching ? "TCP caught receive PASS\n" : "TCP indefinite receive PASS\n"); return 0;
    }
    if (argc==4 && udp_equal(argv[3],"--reset")) {
        CHECK(udp_call(SYS_RECV,fd,(uintptr_t)readonly,1,0,0,0)==SYSCALL_EFAULT);
        size_t at=0;
        while (at<32) { result=udp_call(SYS_RECV,fd,(uintptr_t)(cross+4089+at),32-at,0,0,0); CHECK(result>0); at+=(size_t)result; }
        for (unsigned i=0; i<32; ++i) CHECK(cross[4089+i]==(uint8_t)i);
        CHECK(udp_call(SYS_RECV,fd,(uintptr_t)bytes,1,0,0,0)==SYSCALL_ECONNRESET);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0); udp_write("TCP buffered reset PASS\n"); return 0;
    }
    if (argc==4 && udp_equal(argv[3],"--unread")) {
        CHECK(udp_call(SYS_SEND,fd,(uintptr_t)bytes,1,0,0,0)==1);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0); udp_write("TCP unread close submitted\n"); return 0;
    }
    CHECK(argc==3);
    size_t total=0;
    while (total<65536) {
        size_t n=65536-total; if (n>sizeof(bytes)) n=sizeof(bytes);
        for (size_t i=0; i<n; ++i) bytes[i]=(uint8_t)((total+i)*31);
        result=udp_call(SYS_WRITE,fd,(uintptr_t)bytes,n,0,0,0); CHECK(result>0 && (size_t)result<=n);
        total+=(size_t)result;
    }
    CHECK(!udp_call(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0));
    CHECK(!udp_call(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0));
    signal_action_t ignore={.handler=SIG_IGN};
    CHECK(!udp_call(SYS_SIGACTION,SIGPIPE,(uintptr_t)&ignore,0,0,0,0));
    CHECK(udp_call(SYS_SEND,fd,(uintptr_t)bytes,1,0,0,0)==SYSCALL_EPIPE);
    total=0;
    while (total<65536) {
        result=udp_call(SYS_READ,fd,(uintptr_t)bytes,sizeof(bytes),0,0,0);
        CHECK(result>0 && (size_t)result<=65536-total);
        for (long i=0; i<result; ++i) CHECK(bytes[i]==(uint8_t)((total+(size_t)i)*31));
        total+=(size_t)result;
    }
    CHECK(udp_call(SYS_RECV,fd,(uintptr_t)bytes,1,0,0,0)==0);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    udp_write("TCP 65536 bytes each direction / ABI / dup / read-write / half-close PASS\n"); return 0;
failure:
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    error(result<0 ? result : -(long)line); return 1;
}
