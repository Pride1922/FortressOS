/* Actual nc with syscall adapters; host sanitizer evidence only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "syscall_abi.h"
#include "socket_abi.h"
static long mock_call(long,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
#define NC_CALL mock_call
#include "../user/nc.c"
static uint8_t input[8193], reply[10007], sent[8193], output[10007];
static size_t in_size,in_at,reply_at,sent_at,out_at;
static unsigned closed[32], sockets,shutdowns,accepts,short_sends;
static long failed_call, error_value;
static bool reset_after_data, zero_write, listening,warning_seen;
static long mock_call(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    (void)e; (void)f;
    if (nr==SYS_WRITE && a==2) {
        if (strstr((const char *)b,"can deadlock the serial nc; use small finite requests or a cooperating peer.")) warning_seen=true;
        return (long)c;
    }
    if (nr==failed_call) return error_value;
    if (nr==SYS_SOCKET) {
        ++sockets; assert(a==NET_AF_INET && b==(NET_SOCK_STREAM|NET_SOCK_CLOEXEC) && c==6); return 10;
    }
    if (nr==SYS_CONNECT || nr==SYS_BIND) {
        const net_sockaddr_in_t *address=(void *)b;
        assert(a==10 && c==16 && address->family==2 && address->port==__builtin_bswap16(7777));
        assert(address->address==(listening ? 0 : __builtin_bswap32(0x0a000202)));
        for (unsigned i=0;i<8;++i) assert(!address->reserved[i]);
        return 0;
    }
    if (nr==SYS_LISTEN) { assert(a==10 && b==1); return 0; }
    if (nr==SYS_ACCEPT) { assert(a==10 && !b && !c && d==NET_SOCK_CLOEXEC); ++accepts; return 11; }
    if (nr==SYS_CLOSE) { assert(a<32); ++closed[a]; assert(closed[a]==1); return 0; }
    if (nr==SYS_SHUTDOWN) { assert(a==(listening ? 11u : 10u) && b==NET_SHUT_WR && sent_at==in_size); ++shutdowns; return 0; }
    if (nr==SYS_READ) {
        uint8_t *destination=(void *)b;
        if (!a) {
            size_t n=in_size-in_at; if(n>257)n=257; if(n>c)n=c;
            memcpy(destination,input+in_at,n); in_at+=n; return (long)n;
        }
        assert(shutdowns==1);
        size_t n=sizeof(reply)-reply_at; if(n>113)n=113; if(n>c)n=c;
        memcpy(destination,reply+reply_at,n); reply_at+=n;
        if (!n && reset_after_data) return SYSCALL_ECONNRESET;
        return (long)n;
    }
    if (nr==SYS_WRITE) {
        if(zero_write) return 0;
        size_t n=c; if(n>7)n=7;
        if(a==1) { assert(out_at+n<=sizeof(output)); memcpy(output+out_at,(void *)b,n); out_at+=n; }
        else { if(n<c) ++short_sends; assert(sent_at+n<=sizeof(sent)); memcpy(sent+sent_at,(void *)b,n); sent_at+=n; }
        return (long)n;
    }
    assert(0); return -1;
}
static void setup(bool listen) {
    for(size_t i=0;i<sizeof(input);++i)input[i]=(uint8_t)(i*31);
    for(size_t i=0;i<sizeof(reply);++i)reply[i]=(uint8_t)(i*17);
    memset(closed,0,sizeof(closed)); in_size=sizeof(input); in_at=reply_at=sent_at=out_at=0;
    sockets=shutdowns=accepts=short_sends=0; warning_seen=false; failed_call=-1; error_value=SYSCALL_EINTR;
    reset_after_data=zero_write=false; listening=listen;
}
int main(void) {
    char *client[]={"nc","10.0.2.2","7777"}, *server[]={"nc","-l","7777"};
    for(unsigned mode=0;mode<2;++mode) {
        setup(mode); assert(!nc_main(3,mode ? server : client));
        assert(sent_at==sizeof(input) && !memcmp(input,sent,sent_at));
        assert(short_sends>1000); /* Actual nc must retry positive short writes. */
        assert(out_at==sizeof(reply) && !memcmp(reply,output,out_at));
        assert(shutdowns==1 && closed[10]==1 && accepts==mode && closed[11]==mode);
        setup(mode); in_size=0; assert(!nc_main(3,mode ? server : client)); assert(!sent_at && out_at==sizeof(reply));
        setup(mode); reset_after_data=true; assert(nc_main(3,mode ? server : client)==1);
        assert(out_at==sizeof(reply) && !memcmp(reply,output,out_at) && closed[10]==1);
    }
    const long errors[]={SYS_SOCKET,SYS_CONNECT,SYS_SHUTDOWN,SYS_READ,SYS_WRITE};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        setup(false); failed_call=errors[i]; assert(nc_main(3,client)==1);
        assert(closed[10]==(errors[i]!=SYS_SOCKET));
    }
    const long server_errors[]={SYS_BIND,SYS_LISTEN,SYS_ACCEPT};
    for(unsigned i=0;i<sizeof(server_errors)/sizeof(server_errors[0]);++i) {
        setup(true); failed_call=server_errors[i]; assert(nc_main(3,server)==1); assert(closed[10]==1 && !closed[11]);
    }
    setup(false); zero_write=true; assert(nc_main(3,client)==1 && closed[10]==1);
    char *bad[][3]={{"nc","hostname","7777"},{"nc","10.0.2.256","7777"},{"nc","-l","0"},{"nc","10.0.2.2","65536"},{"nc","-k","7777"}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { setup(false); assert(nc_main(3,bad[i])==1 && !sockets && warning_seen); }
    puts("nc actual serial copy / binary short I/O / empty stdin / buffered reset / errors / cleanup PASS");
    return 0;
}
