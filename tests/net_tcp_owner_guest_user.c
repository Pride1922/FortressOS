/* Disposable installed-root probe; never packaged into release media. */
#include "../user/dns_codec.c"
#include "../user/dns.c"
static uint8_t sent[65536], received[65536];
static dns_context_t context;
static dns_result_t resolved;
static uint64_t clock_now(unsigned seconds) {
    sysinfo_t info;
    if (udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0)) return 0;
    return info.uptime_ticks+seconds*info.tick_hz;
}
int net_cpu_probe_main(int argc,char **argv) {
    char mode=argc>1 ? argv[argc-1][0] : 'u';
    if (mode=='o') return udp_call(0x7ff3,6,0,0,0,0,0) ? 52 : 0;
    if (mode=='b') {
        sysinfo_t info;
        if (udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0)) return 48;
        return udp_call(0x7ff3,3,info.cpu_count>4 ? 4 : info.cpu_count,0,0,0,0) ? 49 : 0;
    }
    if (mode=='j' || mode=='v') return udp_call(0x7ff3,mode=='j' ? 4 : 5,0,0,0,0,0) ? 50 : 0;
    if (mode=='g' || mode=='m') {
        for (unsigned i=0;i<200000;++i) {
            long result=udp_call(0x7ff3,mode=='m',0,0,0,0,0);
            if (mode=='m' ? result==1 : result==0) return 0;
            if (result<0) return 40;

        }
        return 41;
    }
    if (mode=='q') {
        long result=udp_call(SYS_RECV_UNTIL,30,(uintptr_t)received,1,0,clock_now(5),0);
        udp_call(SYS_CLOSE,30,0,0,0,0,0);
        return result==SYSCALL_ETIMEDOUT ? 0 : 37;
    }
    uint32_t ip=0x0202000a;
    if (mode=='d') {
        dns_options_t options={.server_ipv4=ip};
        dns_context_init(&context);
        if (dns_resolve_ipv4(&context,&options,"tcp.test",8,&resolved)!=DNS_OK ||
            !(resolved.flags&DNS_RESULT_TCP) || resolved.addresses[0]!=ip) return 20;
        ip=resolved.addresses[0];
    }
    long fd=udp_call(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6,0,0,0);
    if (fd<0) return 21;
    if (udp_call(0x7ff3,2,0,0,0,0,0)>0 &&
        (udp_call(SYS_LISTEN,fd,1,0,0,0,0)!=SYSCALL_EINVAL ||
         udp_call(SYS_ACCEPT,fd,0,0,0,0,0)!=SYSCALL_EINVAL)) return 43;
    net_sockaddr_in_t address={.family=2,.port=__builtin_bswap16(mode=='z' ? 17779 : 17778),.address=mode=='c' ? 0x7b02000a : ip};
    net_sockaddr_in_t local={.family=2};
    if (udp_call(SYS_BIND,fd,(uintptr_t)&local,16,0,0,0)) return 31;
    uint64_t deadline=clock_now(mode=='c' ? 10 : mode=='z' ? 20 : 20);
    if (mode=='c') {
        long result=udp_call(SYS_CONNECT_UNTIL,fd,(uintptr_t)&address,16,deadline,0,0);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        return result==SYSCALL_ETIMEDOUT ? 0 : 51;
    }
    if (mode=='t') {
        if (udp_call(SYS_CONNECT_UNTIL,fd,(uintptr_t)&address,16,deadline,0,0)) return 22;
    } else if (udp_call(SYS_CONNECT,fd,(uintptr_t)&address,16,0,0,0)) return 23;
    if (mode=='x') {
        const char request='R';
        if (udp_call(SYS_SEND,fd,(uintptr_t)&request,1,0,0,0)!=1) return 44;
        size_t at=0;
        while (at<18) {
            long result=udp_call(SYS_RECV,fd,(uintptr_t)(received+at),18-at,0,0,0);
            if (result<=0 || (size_t)result>18-at) return 45;
            at+=(size_t)result;
        }
        const char expected[]="reset-before-error";
        for (size_t i=0;i<18;++i) if (received[i]!=(uint8_t)expected[i]) return 46;
        long result=udp_call(SYS_RECV,fd,(uintptr_t)received,1,0,0,0);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        return result==SYSCALL_ECONNRESET ? 0 : 47;
    }
    if (mode=='h') {
        if (udp_call(SYS_DUP2,fd,30,0,0,0,0)!=30) return 38;
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        const char *child_argv[]={"net-cpu-probe","--worker","1","q",NULL};
        long pid=udp_call(SYS_SPAWN,(uintptr_t)"/bin/net-cpu-probe",(uintptr_t)child_argv,0,0,0,0);
        if (pid<0) return 39;
        for (unsigned i=0;i<200000;++i) if (udp_call(0x7ff3,1,0,0,0,0,0)==1) break;
        udp_call(SYS_CLOSE,30,0,0,0,0,0);
        int64_t status=-1;
        long waited=udp_call(SYS_WAIT,(uintptr_t)pid,(uintptr_t)&status,0,0,0,0);
        return !waited && !status ? 0 : 42;
    }
    if (mode=='z') {
        long status=0;
        for (unsigned i=0;i<512;++i) {
            status=udp_call(SYS_SEND_UNTIL,fd,(uintptr_t)sent,4096,0,deadline,0);
            if (status<0) break;
            if (!status || status>4096) return 35;
        }
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        return status==SYSCALL_ETIMEDOUT ? 0 : 36;
    }
    if (mode=='w') {
        long result=udp_call(SYS_RECV_UNTIL,fd,(uintptr_t)received,1,0,deadline,0);
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        return result==SYSCALL_ETIMEDOUT || result==SYSCALL_EINTR ? 0 : 32;
    }
    if (udp_call(SYS_RECV,fd,(uintptr_t)received,1,NET_MSG_DONTWAIT,0,0)!=SYSCALL_EAGAIN) return 24;
    size_t size=mode=='t' ? 16384 : sizeof(sent);
    for (size_t i=0;i<size;++i) sent[i]=(uint8_t)(i*37+(i>>8));
    for (size_t at=0;at<size;) {
        size_t n=size-at; if (n>16384) n=16384;
        unsigned nr=mode=='t' ? SYS_SEND_UNTIL : at&4096 ? SYS_WRITE : SYS_SEND;
        long result=udp_call(nr,fd,(uintptr_t)(sent+at),n,0,deadline,0);
        if (result<=0 || result>4096) return 25;
        at+=(size_t)result;
    }
    if (udp_call(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0)) return 26;
    size_t at=0;
    while (at<size) {
        unsigned nr=mode=='t' ? SYS_RECV_UNTIL : at&1 ? SYS_READ : SYS_RECV;
        long result=udp_call(nr,fd,(uintptr_t)(received+at),size-at,0,deadline,0);
        if (result<=0 || (size_t)result>size-at) return 27;
        at+=(size_t)result;
    }
    for (size_t i=0;i<size;++i) if (sent[i]!=received[i]) return 28;
    /* Force READ entry independently of how the peer packetizes the reply. */
    if (udp_call(SYS_READ,fd,(uintptr_t)received,1,0,0,0)) return 29;
    signal_action_t ignored={.handler=SIG_IGN};
    if (udp_call(SYS_SIGACTION,SIGPIPE,(uintptr_t)&ignored,0,0,0,0)) return 33;
    if (udp_call(SYS_WRITE,fd,(uintptr_t)sent,1,0,0,0)!=SYSCALL_EPIPE) return 34;
    if (udp_call(SYS_CLOSE,fd,0,0,0,0,0)) return 30;
    return 0;
}
