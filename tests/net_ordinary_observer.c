/* Snapshot-only observation. No CPU placement or network operation. */
#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "net_cpu_observation.h"
static long invoke(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    register uintptr_t r10 __asm__("r10")=d;
    register uintptr_t r8 __asm__("r8")=e;
    register uintptr_t r9 __asm__("r9")=f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory","cc");
    return nr;
}
static bool equal(const char *a,const char *b) { while (*a && *a==*b) {++a;++b;} return *a==*b; }
static bool output(const char *s) {
    size_t n=0,at=0;while(s[n]) ++n;
    while(at<n) {
        long w=invoke(SYS_WRITE,1,(uintptr_t)(s+at),n-at,0,0,0);
        if(w<=0 || (size_t)w>n-at) return false;
        at+=(size_t)w;
    }
    return true;
}
static void hex(uint64_t n) {
    char s[19]={'0','x'};unsigned at=2;bool started=false;
    for(int shift=60;shift>=0;shift-=4) {
        unsigned digit=(unsigned)(n>>shift)&15;
        if(digit || started || !shift) {s[at++]="0123456789abcdef"[digit];started=true;}
    }
    s[at]=0;(void)output(s);
}
static int cpu_report(bool detail,bool invalid) {
    static net_cpu_observation_t snapshot;
    if(invalid) {
        if(invoke(NET_CPU_OBSERVATION_NR,0,0,sizeof(snapshot),0,0,0)!=SYSCALL_EFAULT ||
           invoke(NET_CPU_OBSERVATION_NR,0,UINT64_MAX-32,sizeof(snapshot),0,0,0)!=SYSCALL_EFAULT ||
           invoke(NET_CPU_OBSERVATION_NR,0,0xffffffff80000000ULL,sizeof(snapshot),0,0,0)!=SYSCALL_EFAULT ||
           invoke(NET_CPU_OBSERVATION_NR,0,(uintptr_t)"readonly",sizeof(snapshot),0,0,0)!=SYSCALL_EFAULT ||
           invoke(NET_CPU_OBSERVATION_NR,0,(uintptr_t)&snapshot,1,0,0,0)!=SYSCALL_EINVAL ||
           invoke(NET_CPU_OBSERVATION_NR,UINT64_MAX,(uintptr_t)&snapshot,sizeof(snapshot),0,0,0)!=SYSCALL_ENOENT)
            return 1;
        return output("OBSERVER bounds PASS\n") ? 0 : 1;
    }
    (void)output("DELL OBSERVER TEST: CS=3 entry, no placement changes\n");
    unsigned passed=0;uint64_t owner=0,steps=0,boot=0;
    for(unsigned row=0;row<64;row++) {
        long r=invoke(NET_CPU_OBSERVATION_NR,row,(uintptr_t)&snapshot,sizeof(snapshot),0,0,0);
        if(r==SYSCALL_ENOENT) break;
        if(r || snapshot.version!=NET_CPU_OBSERVATION_VERSION) {
            (void)output("Observer unavailable or ABI mismatch; use diagnostic image.\n");return 1;
        }
        owner=snapshot.owner_cpus;steps=snapshot.owner_steps;boot=snapshot.boot_dhcp_cpus;
        bool wanted=equal(snapshot.name,"/bin/ping") || equal(snapshot.name,"/bin/nslookup") ||
            equal(snapshot.name,"/bin/wget") || equal(snapshot.name,"/sbin/dhcpc");
        if(!wanted) continue;
        uint64_t all=0,net=0;
        for(unsigned nr=0;nr<NET_CPU_OBSERVATION_SYSCALLS;nr++) {
            all|=snapshot.masks[nr];
            if(nr>=SYS_SOCKET && nr<=SYS_CONNECT_UNTIL) net|=snapshot.masks[nr];
        }
        (void)output(snapshot.name);(void)output(" all=");hex(all);
        (void)output(" net=");hex(net);(void)output((net&~1ULL)?" AP=YES\n":" AP=NO\n");
        if(net&~1ULL) ++passed;
        if(detail) for(unsigned nr=SYS_SOCKET;nr<=SYS_CONNECT_UNTIL;nr++) if(snapshot.masks[nr]) {
            (void)output("  nr=");hex(nr);(void)output(" cpus=");hex(snapshot.masks[nr]);(void)output("\n");
        }
    }
    (void)output("OWNER cpus=");hex(owner);(void)output(" steps=");hex(steps);
    (void)output("\nBOOT DHCP pinned cpus=");hex(boot);
    (void)output(" (excluded from ordinary masks)\n");
    bool pass=passed==4 && owner==1 && steps;
    (void)output(pass?"AP evidence PASS: four commands; owner CPU0\n":"AP evidence INCOMPLETE: run commands, then check again\n");
    return pass?0:1;
}
static void close_fd(long fd) { (void)invoke(SYS_CLOSE,fd,0,0,0,0,0); }
static int consume(long fd,unsigned count,bool echo) {
    uint8_t bytes[256];unsigned at=0;
    while (at<count) {
        long n=invoke(SYS_RECV,fd,(uintptr_t)(bytes+at),count-at,0,0,0);
        if (n<=0 || (unsigned)n>count-at) return -1;
        at+=(unsigned)n;
    }
    for (unsigned i=0;i<count;i++) if (bytes[i]!=(uint8_t)(bytes[0]+i)) return -1;
    if (echo) {
        if (invoke(SYS_RECV,fd,(uintptr_t)bytes,1,0,0,0)!=0) return -1;
        for (at=0;at<count;) {
            long n=invoke(SYS_SEND,fd,(uintptr_t)(bytes+at),count-at,0,0,0);
            if (n<=0 || (unsigned)n>count-at) return -1;
            at+=(unsigned)n;
        }
        if (invoke(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0)) return -1;
    }
    return bytes[0]+1;
}
static long spawn(const char *mode) {
    const char *args[]={"net-cpu-probe",mode,NULL};
    return invoke(SYS_SPAWN,(uintptr_t)"/bin/net-cpu-probe",(uintptr_t)args,0,0,0,0);
}
static int inherited_udp(bool child) {
    if (!child) {
        long fd=invoke(SYS_SOCKET,NET_AF_INET,NET_SOCK_DGRAM,0,0,0,0);
        net_sockaddr_in_t local={.family=NET_AF_INET};
        if (fd<0 || invoke(SYS_BIND,fd,(uintptr_t)&local,sizeof(local),0,0,0) ||
                invoke(SYS_DUP2,fd,28,0,0,0,0)!=28) return 61;
        close_fd(fd);long pid=spawn("udp-child");close_fd(28);uint64_t status=99;
        if (pid<0 || invoke(SYS_WAIT,pid,(uintptr_t)&status,0,0,0,0) || status) return 62;
        return 0;
    }
    static const char message[]="inherited-udp-proof";
    char received[sizeof(message)];
    net_sockaddr_in_t peer={.family=NET_AF_INET,.port=__builtin_bswap16(17777),.address=__builtin_bswap32(0x0a000202)},source;
    uint32_t size=sizeof(source);
    long n=invoke(SYS_SENDTO,28,(uintptr_t)message,sizeof(message)-1,0,(uintptr_t)&peer,sizeof(peer));
    if (n!=(long)sizeof(message)-1) return 63;
    n=invoke(SYS_RECVFROM,28,(uintptr_t)received,sizeof(received),0,(uintptr_t)&source,(uintptr_t)&size);
    if (n!=(long)sizeof(message)-1 || size!=sizeof(source) || source.address!=peer.address || source.port!=peer.port) return 64;
    for (unsigned i=0;i<sizeof(message)-1;i++) if (received[i]!=message[i]) return 65;
    close_fd(28);return 0;
}
static int listener(bool shared) {
    long fd=invoke(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6,0,0,0);
    if (fd<0) return 41;
    net_sockaddr_in_t address={.family=NET_AF_INET,.port=__builtin_bswap16(9000)};
    if (invoke(SYS_BIND,fd,(uintptr_t)&address,sizeof(address),0,0,0) ||
            invoke(SYS_LISTEN,fd,4,0,0,0,0) || invoke(SYS_DUP2,fd,30,0,0,0,0)!=30) return 42;
    close_fd(fd);
    static const char ready[]="PRIVATE LISTENER READY\n";
    if (invoke(SYS_WRITE,1,(uintptr_t)ready,sizeof(ready)-1,0,0,0)!=(long)sizeof(ready)-1) return 43;
    if (!shared) {
        long children[3];
        for (unsigned i=0;i<3;i++) if ((children[i]=spawn("accept-child"))<0) return 44;
        close_fd(30);
        for (unsigned i=0;i<3;i++) {
            uint64_t status=99;
            if (invoke(SYS_WAIT,children[i],(uintptr_t)&status,0,0,0,0) || status) return 45;
        }
        return 0;
    }
    fd=invoke(SYS_ACCEPT,30,0,0,0,0,0);close_fd(30);
    if (fd<0 || invoke(SYS_DUP2,fd,29,0,0,0,0)!=29) return 46;
    close_fd(fd);long child=spawn("read-child");if (child<0) return 47;
    int first=consume(29,128,false);close_fd(29);uint64_t second=0;
    if (invoke(SYS_WAIT,child,(uintptr_t)&second,0,0,0,0) || first<1 || first>256 ||
            second<1 || second>256 || ((first-1)^128)!=(int)second-1) return 48;
    return 0;
}
int net_cpu_probe_main(int argc,char **argv) {
    if(argc==2 && equal(argv[1],"cpu")) return cpu_report(false,false);
    if(argc==2 && equal(argv[1],"cpu-detail")) return cpu_report(true,false);
    if(argc==2 && equal(argv[1],"cpu-invalid")) return cpu_report(false,true);
    if (argc==2 && equal(argv[1],"udp-parent")) return inherited_udp(false);
    if (argc==2 && equal(argv[1],"udp-child")) return inherited_udp(true);
    if (argc==2 && equal(argv[1],"shared-listener")) return listener(false);
    if (argc==2 && equal(argv[1],"shared-stream")) return listener(true);
    if (argc==2 && equal(argv[1],"accept-child")) {
        long fd=invoke(SYS_ACCEPT,30,0,0,0,0,0);close_fd(30);
        if (fd<0) return 49;
        int result=consume(fd,256,true);close_fd(fd);return result<1 ? 50 : 0;
    }
    if (argc==2 && equal(argv[1],"read-child")) {
        int result=consume(29,128,false);close_fd(29);return result<1 ? 51 : result;
    }
    long result=0x7ff5;
    __asm__ volatile("syscall" : "+a"(result) : : "rcx","r11","memory","cc");
    return (int)result;
}
