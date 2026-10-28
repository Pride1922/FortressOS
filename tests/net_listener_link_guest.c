/* Private ISO-only Ring 3 listener carrier-loss regression. */
#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "netctl_abi.h"
static long call(long n,uintptr_t a,uintptr_t b,uintptr_t c) {
    register uintptr_t r10 __asm__("r10")=0, r8 __asm__("r8")=0, r9 __asm__("r9")=0;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a),"S"(b),"d"(c),
                     "r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory","cc");
    return n;
}
static void say(const char *s) { size_t n=0; while (s[n]) ++n; call(SYS_WRITE,1,(uintptr_t)s,n); }
static int fail(unsigned line) {
    char message[]="LISTENER LINK FAIL line 000\n";
    message[sizeof(message)-5]=(char)('0'+line/100);
    message[sizeof(message)-4]=(char)('0'+line/10%10);
    message[sizeof(message)-3]=(char)('0'+line%10);
    say(message); return (int)line;
}
#define REQUIRE(x) do { if (!(x)) return fail(__LINE__); } while (0)
static int listener(void) {
    long fd=call(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6);
    if (fd<0) return -1;
    net_sockaddr_in_t address={.family=NET_AF_INET,.port=(uint16_t)((9000u<<8)|(9000u>>8))};
    if (call(SYS_BIND,fd,(uintptr_t)&address,sizeof(address)) || call(SYS_LISTEN,fd,2,0)) {
        call(SYS_CLOSE,fd,0,0); return -1;
    }
    return (int)fd;
}
static bool online(void) {
    netctl_ifget_t get={.struct_version=1};
    return !call(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&get,sizeof(get)) && get.link_state==NET_IF_LINK_ONLINE;
}
static bool error_ready(int fd) {
    struct pollfd p={.fd=fd,.events=POLLIN};
    return call(SYS_POLL,(uintptr_t)&p,1,0)==1 && (p.revents&POLLERR);
}
int listener_link_main(int argc,char **argv) {
    REQUIRE(argc==3 && argv[2][0]>='0' && argv[2][0]<='7');
    REQUIRE(call(0x7ff7,0,0,0)==argv[2][0]-'0'); /* Actual entry CPU in private snapshot. */
    int old=listener(); REQUIRE(old>=0);
    say("LISTENER LINK READY\n");
    REQUIRE(call(SYS_ACCEPT,old,0,0)==SYSCALL_EIO);
    REQUIRE(error_ready(old));
    say("LISTENER LINK DOWN\n");
    for (unsigned i=0;i<1500 && !online();++i) call(SYS_POLL,0,0,10);
    REQUIRE(online() && error_ready(old));
    /* Blocking ACCEPT must fail immediately after recovery, not merely return
     * a transient nonblocking EAGAIN. Kernel regression hangs here. */
    REQUIRE(call(SYS_ACCEPT,old,0,0)==SYSCALL_EIO);
    REQUIRE(!call(SYS_CLOSE,old,0,0));
    int fresh=listener(); REQUIRE(fresh>=0);
    say("LISTENER LINK FRESH\n");
    long child=call(SYS_ACCEPT,fresh,0,0); REQUIRE(child>=0);
    static const char expected[]="REPLUG-FRESH\n";
    char received[sizeof(expected)-1]; size_t offset=0;
    while (offset<sizeof(received)) {
        long n=call(SYS_READ,child,(uintptr_t)(received+offset),sizeof(received)-offset);
        REQUIRE(n>0 && (size_t)n<=sizeof(received)-offset); offset+=(size_t)n;
    }
    for (unsigned i=0;i<sizeof(received);++i) REQUIRE(received[i]==expected[i]);
    offset=0;
    while (offset<sizeof(received)) {
        long n=call(SYS_WRITE,child,(uintptr_t)(received+offset),sizeof(received)-offset);
        REQUIRE(n>0 && (size_t)n<=sizeof(received)-offset); offset+=(size_t)n;
    }
    REQUIRE(!call(SYS_CLOSE,child,0,0) && !call(SYS_CLOSE,fresh,0,0));
    REQUIRE(call(0x7ff7,1,0,0)==1); /* Registered protocol owner mask is CPU0. */
    say("LISTENER LINK PASS\n"); return 0;
}
