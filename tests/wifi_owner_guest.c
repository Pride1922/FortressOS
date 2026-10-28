/* Private Ring 3 BSP/AP probe; no secrets in command arguments or logs. */
#include "types.h"
#include "syscall_abi.h"
#include "wifi_abi.h"
#include "netctl_abi.h"
static long sc(long n,uintptr_t a,uintptr_t b,uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(n) : "D"(a),"S"(b),"d"(c) : "rcx","r11","memory","cc");
    return n;
}
static long observe(unsigned op) { return sc(0x7ff6,op,0,0); }
static void say(const char *p) { size_t n=0; while(p[n]) ++n; sc(SYS_WRITE,1,(uintptr_t)p,n); }
static void pause_ms(unsigned ms) { sc(SYS_POLL,0,0,ms); }
static long wifi(unsigned op,void *p,size_t n) {
    for (unsigned i=0;i<200;++i) {
        long r=sc(SYS_WIFI,op,(uintptr_t)p,n);
        if (r!=SYSCALL_EAGAIN) return r;
        pause_ms(1);
    }
    return SYSCALL_EAGAIN;
}
#define CHECK(x) do { if (!(x)) { say("WIFI OWNER FAIL\n"); return __LINE__; } } while (0)
static wifi_scan_result_t scan;
static wifi_status_t status;
static const wifi_status_t readonly_status={0};
static uint8_t firmware[96]={[4]=0x49,[5]=0x57,[6]=0x4c,[7]=0x0a,[88]=18};
int wifi_owner_main(int argc,char **argv) {
    CHECK(argc>=4);
    CHECK(observe(0)==argv[2][0]-'0');
    char mode=argv[3][0];
    if (mode=='h') { CHECK(!observe(1)); return 0; }
    if (mode=='u') { CHECK(!observe(2)); return 0; }
    if (mode=='p') { CHECK(observe(3)>0); return 0; }
    if (mode=='f') {
        for (unsigned i=0;i<500 && observe(3)!=4;++i) pause_ms(10);
        CHECK(observe(3)==4); return 0;
    }
    if (mode=='e') {
        CHECK(sc(SYS_WIFI,SYS_WIFI_CMD_STATUS,(uintptr_t)&status,sizeof(status))==SYSCALL_EAGAIN);
        return 0;
    }
    if (mode=='c') {
        wifi_connect_req_t req={.ssid="Fixture",.passphrase="fixture-key",.bssid={2,3,4,5,6,7}};
        CHECK(!wifi(SYS_WIFI_CMD_CONNECT,&req,sizeof(req))); return 0;
    }
    if (mode=='w') { CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status))); return 0; }
    if (mode=='b') {
        for (unsigned i=0;i<64;++i) CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status)));
        say("WIFI concurrent PASS\n"); return 0;
    }
    if (mode=='g') {
        for (unsigned i=0;i<500 && observe(4);++i) pause_ms(10);
        CHECK(!observe(4) && observe(5)==31 && observe(7)==1 && observe(8)>0);
        sysinfo_t info={0}; CHECK(!sc(SYS_SYSINFO,(uintptr_t)&info,sizeof(info),0));
        CHECK(observe(6)==(long)((1ULL<<info.cpu_count)-1));
        say("WIFI ownership/cleanup PASS\n"); return 0;
    }
    CHECK(sc(SYS_WIFI,0,0,0)==SYSCALL_EINVAL);
    CHECK(sc(SYS_WIFI,SYS_WIFI_CMD_STATUS,0,sizeof(status))==SYSCALL_EFAULT);
    CHECK(sc(SYS_WIFI,SYS_WIFI_CMD_STATUS,(uintptr_t)&readonly_status,sizeof(status))==SYSCALL_EFAULT);
    CHECK(sc(SYS_WIFI,SYS_WIFI_CMD_SCAN,(uintptr_t)&scan,1)==SYSCALL_EINVAL);
    CHECK(sc(SYS_WIFI,SYS_WIFI_CMD_LOAD_FW,(uintptr_t)firmware,4*1024*1024+1)==SYSCALL_EINVAL);
    CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status)));
    CHECK(!wifi(SYS_WIFI_CMD_LOAD_FW,firmware,sizeof(firmware)));
    CHECK(!wifi(SYS_WIFI_CMD_SCAN,&scan,sizeof(scan)));
    CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status)));
    scan.max_entries=16;
    CHECK(!wifi(SYS_WIFI_CMD_SCAN,&scan,sizeof(scan)) && scan.count==1 && scan.entries[0].ssid[0]=='F');
    wifi_connect_req_t req={.ssid="Fixture",.passphrase="fixture-key",.bssid={2,3,4,5,6,7}};
    CHECK(!wifi(SYS_WIFI_CMD_CONNECT,&req,sizeof(req)));
    CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status)) && status.state==WIFI_STATUS_AUTHENTICATING);
    /* No peer responses: progression/retries/timeout must work without GUI polling. */
    pause_ms(6800);
    CHECK(!wifi(SYS_WIFI_CMD_STATUS,&status,sizeof(status)) && status.state==WIFI_STATUS_FAILED);
    CHECK(!wifi(SYS_WIFI_CMD_DISCONNECT,NULL,0));
    netctl_ifget_t net={.struct_version=1};
    CHECK(!sc(SYS_NETCTL,NETCTL_IFGET,(uintptr_t)&net,sizeof(net)));
    say("WIFI BSP/AP operations PASS\n"); return 0;
}
