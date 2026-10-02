#include "udp_common.h"
static uint8_t bytes[1024], cross[8192] __attribute__((aligned(4096)));
static const uint8_t readonly[16]={0};
static volatile unsigned caught_count;
static void caught(unsigned signal) { if (signal==SIGINT) ++caught_count; }
int tcpserve_main(int argc, char **argv) {
    if (argc<2 || argc>3) { udp_write("usage: tcpserve port [--caught|--compete|--shared] (one child, <=65536-byte echo)\n"); return 1; }
    unsigned number; const char *p=argv[1];
    if (!udp_number(&p,65535,&number) || *p || !number) return 1;
    bool inherited=argc==3 && udp_equal(argv[2],"--fd");
    bool catching=argc==3 && udp_equal(argv[2],"--caught");
    bool compete=argc==3 && udp_equal(argv[2],"--compete");
    bool shared=argc==3 && udp_equal(argv[2],"--shared");
    if (argc==3 && !inherited && !catching && !compete && !shared) return 1;
    long listener=-1, child=-1, spawned=-1;
#define CHECK(x) do { if (!(x)) goto failure; } while (0)
    if (inherited) listener=number;
    else {
        listener=udp_call(SYS_SOCKET,2,NET_SOCK_STREAM,6,0,0,0); CHECK(listener>=0);
        CHECK(udp_call(SYS_LISTEN,listener,1,0,0,0,0)==SYSCALL_EINVAL);
        net_sockaddr_in_t address={.family=2,.port=__builtin_bswap16((uint16_t)number)};
        CHECK(!udp_call(SYS_BIND,listener,(uintptr_t)&address,16,0,0,0));
        CHECK(udp_call(SYS_LISTEN,listener,5,0,0,0,0)==SYSCALL_EINVAL);
        CHECK(!udp_call(SYS_LISTEN,listener,4,0,0,0,0));
        CHECK(udp_call(SYS_LISTEN,listener,4,0,0,0,0)==SYSCALL_EINVAL);
    }
    uint32_t capacity=16;
    CHECK(udp_call(SYS_ACCEPT,listener,(uintptr_t)readonly,(uintptr_t)&capacity,0,0,0)==SYSCALL_EFAULT);
    CHECK(udp_call(SYS_ACCEPT,listener,0,(uintptr_t)&capacity,0,0,0)==SYSCALL_EINVAL);
    CHECK(udp_call(SYS_ACCEPT,listener,(uintptr_t)cross,(uintptr_t)cross,0,0,0)==SYSCALL_EINVAL);
    CHECK(udp_call(SYS_ACCEPT,listener,0,0,64,0,0)==SYSCALL_EINVAL);
    if (compete || shared) {
        char descriptor[3]={(char)('0'+listener/10),(char)('0'+listener%10),0};
        const char *args[]={"/bin/tcpserve",descriptor,"--fd",NULL};
        spawned=udp_call(SYS_SPAWN,(uintptr_t)args[0],(uintptr_t)args,0,0,0,0); CHECK(spawned>0);
        if (shared) {
            udp_call(SYS_CLOSE,listener,0,0,0,0,0); listener=-1;
            udp_write("TCP shared listener parent closed\n");
            int64_t status; CHECK(!udp_call(SYS_WAIT,spawned,(uintptr_t)&status,0,0,0,0) && status==0);
            udp_write("TCP shared listener PASS\n"); return 0;
        }
    }
    if (catching) {
        signal_action_t action={.handler=(uintptr_t)caught};
        CHECK(!udp_call(SYS_SIGACTION,SIGINT,(uintptr_t)&action,0,0,0,0));
    }
    udp_write(catching ? "TCP caught accept ready\n" : "TCP server listening\n");
    /* Exercise unaligned cross-page sockaddr output and unaligned capacity. */
    for (unsigned i=0; i<4; ++i) cross[100+i]=((uint8_t *)&capacity)[i];
    child=udp_call(SYS_ACCEPT,listener,(uintptr_t)(cross+4089),(uintptr_t)(cross+100),NET_SOCK_CLOEXEC,0,0);
    if (catching) {
        CHECK(child==SYSCALL_EINTR && caught_count==1);
        udp_call(SYS_CLOSE,listener,0,0,0,0,0); udp_write("TCP caught accept PASS\n"); return 0;
    }
    CHECK(child>=0);
    net_sockaddr_in_t peer;
    for (unsigned i=0; i<16; ++i) ((uint8_t *)&peer)[i]=cross[4089+i];
    CHECK(peer.family==2 && peer.port);
    for (unsigned i=0; i<8; ++i) CHECK(!peer.reserved[i]);
    for (unsigned i=0; i<4; ++i) ((uint8_t *)&capacity)[i]=cross[100+i];
    CHECK(capacity==16 && udp_call(SYS_FCNTL,child,F_GETFD,0,0,0,0)==FD_CLOEXEC);
    udp_call(SYS_CLOSE,listener,0,0,0,0,0); listener=-1;
    udp_write("TCP accepted child; listener closed\n");
    size_t total=0;
    for (;;) {
        long n=udp_call(SYS_READ,child,(uintptr_t)bytes,sizeof(bytes),0,0,0); CHECK(n>=0);
        if (!n) break;
        CHECK((size_t)n<=65536-total); total+=(size_t)n;
        size_t at=0;
        while (at<(size_t)n) {
            long sent=udp_call(SYS_WRITE,child,(uintptr_t)(bytes+at),(size_t)n-at,0,0,0);
            CHECK(sent>0 && (size_t)sent<=(size_t)n-at); at+=(size_t)sent;
        }
    }
    CHECK(!udp_call(SYS_SHUTDOWN,child,NET_SHUT_WR,0,0,0,0));
    udp_call(SYS_CLOSE,child,0,0,0,0,0); child=-1;
    udp_write("TCP finite server / accept ABI / child independence PASS\n");
    if (spawned>0) {
        int64_t status; CHECK(!udp_call(SYS_WAIT,spawned,(uintptr_t)&status,0,0,0,0) && status==0);
        udp_write("TCP competing acceptors PASS\n");
    }
    return 0;
failure:
    if (child>=0) udp_call(SYS_CLOSE,child,0,0,0,0,0);
    if (listener>=0) udp_call(SYS_CLOSE,listener,0,0,0,0,0);
    udp_write("TCP finite server FAIL\n"); return 1;
}
