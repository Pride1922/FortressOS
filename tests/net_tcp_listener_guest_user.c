/* Private installed-root fixture, absent from ordinary initramfs. */
#define net_cpu_probe_main client_stream_main
#include "net_tcp_owner_guest_user.c"
#undef net_cpu_probe_main
static int serve(long listener,bool exhaust,bool keep_listener) {
    if (exhaust) {
        for (unsigned i=3;i<32;++i)
            if ((long)i!=listener && udp_call(SYS_DUP2,listener,i,0,0,0,0)!=(long)i) return 61;
        if (udp_call(SYS_ACCEPT,listener,0,0,0,0,0)!=SYSCALL_EMFILE) return 62;
        for (unsigned i=3;i<32;++i) if ((long)i!=listener) udp_call(SYS_CLOSE,i,0,0,0,0,0);
    }
    net_sockaddr_in_t peer; uint32_t size=16;
    long fd=udp_call(SYS_ACCEPT,listener,(uintptr_t)&peer,(uintptr_t)&size,NET_SOCK_CLOEXEC,0,0);
    if (fd<0 || size!=16 || peer.family!=NET_AF_INET || !peer.port) return 63;
    if (!keep_listener && udp_call(SYS_CLOSE,listener,0,0,0,0,0)) return 64;
    size_t at=0;
    while (at<256) {
        long n=udp_call(SYS_RECV,fd,(uintptr_t)(received+at),256-at,0,0,0);
        if (n<=0 || (size_t)n>256-at) return 65;
        at+=(size_t)n;
    }
    unsigned tag=received[0];
    for (unsigned i=0;i<256;++i) if (received[i]!=(uint8_t)(i+tag)) return 66;
    if (udp_call(SYS_RECV,fd,(uintptr_t)sent,1,0,0,0)) return 67;
    for (at=0;at<256;) {
        long n=udp_call(SYS_SEND,fd,(uintptr_t)(received+at),256-at,0,0,0);
        if (n<=0 || (size_t)n>256-at) return 68;
        at+=(size_t)n;
    }
    if (udp_call(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0) || udp_call(SYS_CLOSE,fd,0,0,0,0,0)) return 69;
    return 0;
}
static int shared_read(long fd) {
    long n=udp_call(SYS_RECV,fd,(uintptr_t)received,128,0,0,0);
    if (n!=128) return 500;
    for (unsigned i=0;i<128;++i) if (received[i]!=(uint8_t)(received[0]+i)) return 501;
    return received[0]+1;
}
static int shared_accepted(long listener) {
    long fd=udp_call(SYS_ACCEPT,listener,0,0,0,0,0);
    if (fd<0 || udp_call(SYS_CLOSE,listener,0,0,0,0,0)) return 80;
    if (udp_call(SYS_DUP2,fd,30,0,0,0,0)!=30) return 81;
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    sysinfo_t info;
    if (udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0)) return 82;
    unsigned current=(unsigned)udp_call(0x7ff3,2,0,0,0,0,0);
    char cpu[2]={(char)('0'+(current+1)%info.cpu_count),0};
    const char *args[]={"net-cpu-probe","--worker",cpu,"R",NULL};
    long pid=udp_call(SYS_SPAWN,(uintptr_t)"/bin/net-cpu-probe",(uintptr_t)args,0,0,0,0);
    if (pid<0) return 83;
    int mine=shared_read(30);
    udp_call(SYS_CLOSE,30,0,0,0,0,0);
    int64_t theirs=-1;
    if (udp_call(SYS_WAIT,pid,(uintptr_t)&theirs,0,0,0,0) || mine<1 || mine>256 ||
        theirs<1 || theirs>256 || ((mine-1)^128)!=(int)(theirs-1)) return 84;
    return 0;
}
int net_cpu_probe_main(int argc,char **argv) {
    char mode=argc>1 ? argv[argc-1][0] : 'L';
    if (mode=='A') return serve(30,false,false);
    if (mode=='R') { int rc=shared_read(30); udp_call(SYS_CLOSE,30,0,0,0,0,0); return rc; }
    if (mode=='N') return udp_call(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6,0,0,0)==SYSCALL_EIO &&
        udp_call(SYS_SOCKET,0,NET_SOCK_STREAM,6,0,0,0)==SYSCALL_EOPNOTSUPP ? 0 : 79;
    if (mode!='L' && mode!='H' && mode!='F' && mode!='E' && mode!='S' && mode!='P' && mode!='C' && mode!='B' && mode!='V' && mode!='W' && mode!='X' && mode!='Y' && mode!='D' && mode!='Q') return client_stream_main(argc,argv);
    unsigned stage=mode=='S' ? 1 : mode=='P' ? 2 : mode=='C' ? 3 : mode=='B' ? 4 : 0;
    unsigned killed=mode=='V' ? 1 : mode=='W' ? 2 : mode=='X' ? 3 : mode=='Y' ? 4 : 0;
    if ((stage || killed) && udp_call(0x7ff4,killed ? killed+16 : stage,0,0,0,0,0)) return 78;
    long listener=udp_call(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6,0,0,0);
    if (listener<0) return 70;
    net_sockaddr_in_t local={.family=NET_AF_INET,.port=__builtin_bswap16(9000)};
    if (udp_call(SYS_BIND,listener,(uintptr_t)&local,16,0,0,0) ||
        udp_call(SYS_LISTEN,listener,4,0,0,0,0)) return 71;
    long children[3]; unsigned count=mode=='H' ? 3 : 0;
    if (count) {
        if (udp_call(SYS_DUP2,listener,30,0,0,0,0)!=30) return 72;
        udp_call(SYS_CLOSE,listener,0,0,0,0,0); listener=30;
        sysinfo_t info;
        if (udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0)) return 73;
        for (unsigned i=0;i<count;++i) {
            char cpu[2]={(char)('0'+(i+1)%info.cpu_count),0};
            const char *args[]={"net-cpu-probe","--worker",cpu,"A",NULL};
            children[i]=udp_call(SYS_SPAWN,(uintptr_t)"/bin/net-cpu-probe",(uintptr_t)args,0,0,0,0);
            if (children[i]<0) return 74;
        }
    }
    if (!udp_write("NET SMP2D LISTENER READY\n")) return 75;
    if (mode=='D') return shared_accepted(listener);
    if (mode=='Q') {
        for (unsigned i=0;i<4;++i) { int rc=serve(listener,false,true); if (rc) return rc; }
        return udp_call(SYS_CLOSE,listener,0,0,0,0,0) ? 85 : 0;
    }
    if (mode=='E') {
        long rc=udp_call(SYS_ACCEPT,listener,0,0,0,0,0);
        udp_call(SYS_CLOSE,listener,0,0,0,0,0);
        return rc==SYSCALL_EINTR || rc==SYSCALL_EIO ? 0 : 76;
    }
    int rc=serve(listener,mode=='F' || mode=='B' || mode=='Y',false); if (rc) return rc;
    for (unsigned i=0;i<count;++i) {
        int64_t status=-1;
        if (udp_call(SYS_WAIT,children[i],(uintptr_t)&status,0,0,0,0) || status) return 77;
    }
    return 0;
}
