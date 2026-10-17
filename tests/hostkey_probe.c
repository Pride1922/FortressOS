/* Explicit test-media only. Never prints or persists a private container. */
#include "common.h"
#include "crypto.h"
#include "hostkey_abi.h"
#include "../user/permissions_cli.h"
static uint8_t output[HOSTKEY_FILE_MAX+32],saved[HOSTKEY_FILE_MAX+32];
static const uint8_t readonly[HOSTKEY_FILE_MAX]={0x42};
static unsigned checks;
extern char probe_data_end;
static void wipe(void) {crypto_wipe(output,sizeof(output));crypto_wipe(saved,sizeof(saved));}
#define CHECK(x) do {checks++;if(!(x)) {wipe();tool_error("hostkey-probe","FAIL: " #x,NULL);return 1;}}while(0)
static long call(uint64_t op,uintptr_t p,uint64_t n) {return tool_syscall(SYS_HOSTKEY,op,p,n);}
static void canary(void) {for(unsigned i=0;i<sizeof(output);i++) output[i]=saved[i]=0xa5;}
static bool equal(const void *a,const void *b,size_t n) {
    const uint8_t *x=a,*y=b;for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return false;return true;
}
int hostkey_probe_main(int argc,char **argv) {
    bool ordinary=argc==2 && tool_equal(argv[1],"unprivileged");
    bool present=argc==2 && tool_equal(argv[1],"present");
    bool absent=argc==2 && tool_equal(argv[1],"absent");
    CHECK(ordinary || present || absent);canary();
    CHECK(call(UINT64_MAX,(uintptr_t)output,1024)==SYSCALL_EINVAL);
    CHECK(call(1ULL<<32,0,1024)==SYSCALL_EINVAL);
    for(unsigned i=0;i<5;i++) {
        uint64_t n=(uint64_t[]){0,1,1023,1025,UINT64_MAX}[i];
        CHECK(call(HOSTKEY_READ,(uintptr_t)output,n)==SYSCALL_EINVAL);
    }
    CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,0)==SYSCALL_EINVAL);
    CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,1024)==SYSCALL_EINVAL);
    CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,UINT64_MAX)==SYSCALL_EINVAL);
    CHECK(equal(output,saved,sizeof(output)));
    if(ordinary) {
        CHECK(call(HOSTKEY_READ,0,1024)==SYSCALL_EPERM);
        CHECK(call(HOSTKEY_READ,(uintptr_t)output,1024)==SYSCALL_EPERM);
        CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,1)==SYSCALL_EPERM);
        CHECK(equal(output,saved,sizeof(output)));
    } else {
        uintptr_t bad[]={0,0xffffffff80000000ULL,UINTPTR_MAX-511,0x400000000000ULL,0x800000000000ULL};
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
            CHECK(call(HOSTKEY_READ,bad[i],1024)==SYSCALL_EFAULT);
            CHECK(call(HOSTKEY_PUBLISH,bad[i],1023)==SYSCALL_EFAULT);
            CHECK(equal(output,saved,sizeof(output)));
        }
        CHECK(call(HOSTKEY_READ,(uintptr_t)readonly,1024)==SYSCALL_EFAULT);
        CHECK(call(HOSTKEY_READ,(uintptr_t)hostkey_probe_main,1024)==SYSCALL_EFAULT);
        /* First 512 bytes are in the final writable ELF page; the next page
         * has no PT_LOAD or heap mapping. Proves whole-range prevalidation. */
        uintptr_t end=((uintptr_t)&probe_data_end+4095u)&~(uintptr_t)4095u;
        volatile uint8_t *partial=(void *)(end-512);
        for(unsigned i=0;i<512;i++) partial[i]=0x5a;
        CHECK(call(HOSTKEY_READ,(uintptr_t)partial,1024)==SYSCALL_EFAULT);
        CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)partial,1023)==SYSCALL_EFAULT);
        for(unsigned i=0;i<512;i++) CHECK(partial[i]==0x5a);
        long n=call(HOSTKEY_READ,(uintptr_t)output,1024);
        if(absent) {
            CHECK(n==SYSCALL_ENOENT);CHECK(equal(output,saved,sizeof(output)));
        } else {
            CHECK(n>0 && n<1024);
            for(size_t i=(size_t)n;i<sizeof(output);i++) CHECK(output[i]==0xa5);
            for(unsigned i=0;i<sizeof(output);i++) saved[i]=output[i];
            CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,1)==SYSCALL_EEXIST);
            CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)readonly,1023)==SYSCALL_EEXIST);
            CHECK(call(HOSTKEY_PUBLISH,(uintptr_t)output,(uint64_t)n)==SYSCALL_EEXIST);
            CHECK(call(HOSTKEY_READ,(uintptr_t)output,1024)==n);
            CHECK(equal(output,saved,sizeof(output)));
        }
        CHECK(!tool_syscall(SYS_CAPSET,0,0,0));
        CHECK(call(HOSTKEY_READ,0,1024)==SYSCALL_EPERM);
        CHECK(call(HOSTKEY_READ,(uintptr_t)output,1024)==SYSCALL_EPERM);
        CHECK(equal(output,saved,sizeof(output)));
    }
    wipe();char count[32];size_t n=tool_format_u64(count,checks);
    const char *message="HOSTKEY boundary PASS checks=";
    tool_write("hostkey-probe",message,tool_length(message));tool_write("hostkey-probe",count,n);tool_write("hostkey-probe","\n",1);return 0;
}
