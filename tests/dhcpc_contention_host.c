/* Actual foreground DHCP entry; socket, owner progress and filesystem mocked.
 * No second DHCP exchange or configuration writes are allowed when busy. */
#include "syscall_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static long mocked(long,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
#define DHCPC_TEST_CALL mocked
#include "../user/dhcpc.c"
static uint64_t clock_ms;
static unsigned scenario, sleeps, sockets, closes, binds;
static size_t positions[2];
static char output[4096];
static size_t output_len;
static const uint8_t test_mac[6]={2,1,2,3,4,5};
static long mocked(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    (void)d;(void)e;(void)f;
    if(nr==SYS_SOCKET){++sockets;return 8;}
    if(nr==SYS_BIND){assert(a==8);++binds;return scenario==7?SYSCALL_EPERM:SYSCALL_EEXIST;}
    if(nr==SYS_CLOSE){if(a==8)++closes;return 0;}
    if(nr==SYS_WRITE){assert(a==1||a==2);assert(output_len+c<sizeof(output));memcpy(output+output_len,(void*)b,c);output_len+=c;output[output_len]=0;return (long)c;}
    if(nr==SYS_OPEN){assert(b==VFS_O_CLOEXEC);if(!strcmp((char*)a,"/run/dhcpc.pid")){if(scenario==6)return SYSCALL_ENOENT;positions[0]=0;return 10;}assert(!strcmp((char*)a,"/run/dhcpc.lease"));positions[1]=0;return 11;}
    if(nr==SYS_READ){assert(a==10||a==11);const char *text=a==10?"98\n":
        scenario==1?"IP=192.168.0.194\nSERVER=192.168.0.1\nNETMASK=255.255.255.0\nEXPIRE_EPOCH=900\n":
        scenario==8?"IP=192.168.0.194\nSERVER=192.168.0.1\nNETMASK=255.255.255.0\nEXPIRE_EPOCH=8200":
        scenario==4?"IP=192.168.0.195\nSERVER=192.168.0.1\nNETMASK=255.255.255.0\nEXPIRE_EPOCH=8200\n":
        clock_ms<2000?"":"IP=192.168.0.194\nSERVER=192.168.0.1\nNETMASK=255.255.255.0\nEXPIRE_EPOCH=8200\n";
        size_t *pos=&positions[a-10],n=strlen(text)-*pos;if(n>c)n=c;memcpy((void*)b,text+*pos,n);*pos+=n;return (long)n;}
    if(nr==SYS_KILL){assert(a==98&&b==0);return scenario==3&&clock_ms>=250?SYSCALL_ESRCH:0;}
    if(nr==SYS_CLOCK_GETTIME){timespec_t *t=(void*)b;t->tv_sec=a==CLOCK_REALTIME?1000:(int64_t)(clock_ms/1000);t->tv_nsec=a==CLOCK_REALTIME?0:(int64_t)(clock_ms%1000)*1000000;return 0;}
    if(nr==SYS_NANOSLEEP){timespec_t *t=(void*)a;assert(t->tv_sec==0&&t->tv_nsec==50000000);++sleeps;if(scenario!=5)clock_ms+=50;return 0;}
    if(nr==SYS_NETCTL){assert(a==NETCTL_IFGET);netctl_ifget_t *q=(void*)b;memset(q,0,sizeof(*q));q->struct_version=1;memcpy(q->mac,test_mac,6);q->link_state=NET_IF_LINK_ONLINE;
        if(scenario==2&&clock_ms>=250)q->mac[5]^=1;
        if(scenario==9&&clock_ms>=250)q->link_state=NET_IF_LINK_DOWN;
        if(clock_ms>=2000||scenario==1||scenario==4||scenario==8){q->local_ipv4=0xc200a8c0;q->netmask_ipv4=0x00ffffff;}
        return 0;}
    if(nr==SYS_WIFI){wifi_status_t *w=(void*)b;memset(w,0,sizeof(*w));memcpy(w->mac,test_mac,6);return 0;}
    assert(!"Unexpected syscall: must not exchange packets, spawn another client or rewrite a managed lease");return SYSCALL_ENOSYS;
}
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){return mocked(n,a,b,c,0,0,0);}
int netconf_parse_file(const char *p,const char *n,bool v,netconf_t *o,char *e,size_t c){(void)p;(void)n;(void)v;(void)o;(void)e;(void)c;assert(!"not reached in managed foreground acquisition");return -1;}
int timeline_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;assert(!"foreground must not publish owner logs");return -1;}
int main(void){
    for(scenario=0;scenario<10;++scenario){
        clock_ms=0;sleeps=sockets=closes=binds=0;output_len=0;output[0]=0;g_term_requested=0;
        char *argv[]={"dhcpc",NULL};int rc=dhcpc_main(1,argv);
        if(scenario==0){if(rc)fprintf(stderr,"Foreground returned %d instead of joining owner: %s",rc,output);assert(rc==0);assert(clock_ms==2000&&sleeps==40);}
        else {assert(rc!=0);assert(sleeps<=200);}
        assert(sockets==1&&closes==1&&binds==1);
    }
    puts("DHCP contention PASS: delayed owner lease joins without another exchange; expired/wrong-IP/partial leases, link loss, MAC change, owner death, frozen clock, absent PID and non-contention bind failures cannot report success; all sockets closed");
}
