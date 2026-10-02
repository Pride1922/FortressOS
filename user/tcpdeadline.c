/* Ring 3 deadline fixture; tests accepted bytes separately from wire delivery. */
#include "udp_common.h"
static uint8_t bytes[8192];
int tcpdeadline_main(int argc,char **argv) {
    net_sockaddr_in_t address={.family=2}; unsigned port;
    const char *number=argc>=3 ? argv[2] : "";
    if(argc!=4 || !udp_ip(argv[1],&address.address) || !udp_number(&number,65535,&port) || *number || !port) return 1;
    bool send=udp_equal(argv[3],"--send"), connect=udp_equal(argv[3],"--connect");
    if(!send && !connect) return 1;
    address.port=__builtin_bswap16((uint16_t)port);
    sysinfo_t info;
    if(udp_call(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0) || !info.tick_hz ||
       info.tick_hz>UINT64_MAX/2 || info.uptime_ticks>UINT64_MAX-info.tick_hz*2) return 1;
    uint64_t deadline=info.uptime_ticks+2*info.tick_hz;
    long fd=udp_call(SYS_SOCKET,2,NET_SOCK_STREAM|NET_SOCK_CLOEXEC,6,0,0,0);
    if(fd<0) return 1;
    long r=udp_call(SYS_CONNECT_UNTIL,fd,(uintptr_t)&address,16,deadline,0,0);
    if(connect) {
        udp_call(SYS_CLOSE,fd,0,0,0,0,0);
        if(r!=SYSCALL_ETIMEDOUT) { udp_write("TCP deadline connect FAIL\n"); return 1; }
        udp_write("TCP deadline connect PASS\n"); return 0;
    }
    if(r) { udp_call(SYS_CLOSE,fd,0,0,0,0,0); udp_write("TCP deadline send-connect FAIL\n"); return 1; }
    long duplicate=udp_call(SYS_DUP,fd,0,0,0,0,0);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0); fd=duplicate;
    if(fd<0) { udp_write("TCP deadline dup FAIL\n"); return 1; }
    for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=0x5a;
    r=udp_call(SYS_SEND_UNTIL,fd,(uintptr_t)bytes,sizeof(bytes),0,deadline,0);
    bool accepted=r==(long)sizeof(bytes);
    if(accepted) r=udp_call(SYS_SEND_UNTIL,fd,(uintptr_t)bytes,1,0,deadline,0);
    udp_call(SYS_CLOSE,fd,0,0,0,0,0);
    if(!accepted || r!=SYSCALL_ETIMEDOUT) { udp_write("TCP deadline blocked send FAIL\n"); return 1; }
    udp_write("TCP deadline blocked send / shared fd PASS\n"); return 0;
}
