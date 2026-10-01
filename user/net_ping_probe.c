/* Ring 3 verification fixture; no normal shell integration. */
#include "syscall_abi.h"
static net_ping_v1_t request;
static const net_ping_v1_t readonly_request={.version=1,.destination=0x0202000a,.timeout_seconds=1};
static long call(uint64_t command, uintptr_t address, uint64_t size) {
    long number=SYS_NETCTL;
    __asm__ volatile("syscall" : "+a"(number) : "D"(command),"S"(address),"d"(size) : "rcx","r11","memory","cc");
    return number;
}
static void write(const char *s, size_t n) {
    long number=SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(number) : "D"(1ul),"S"((uintptr_t)s),"d"(n) : "rcx","r11","memory","cc");
}
int net_ping_probe_main(int argc, char **argv) {
    (void)argc; (void)argv;
    static const uintptr_t invalid[]={0,0xffffffff80000000ull,0xfffffffffffffff0ull,0x7ffffffff000ull};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(*invalid); ++i)
        if (call(NETCTL_PING,invalid[i],48)!=SYSCALL_EFAULT) goto fail;
    if (call(NETCTL_PING,(uintptr_t)&readonly_request,48)!=SYSCALL_EFAULT) goto fail;
    if (call(NETCTL_PING,(uintptr_t)&request,47)!=SYSCALL_EINVAL) goto fail;
    if (call(99,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    request=readonly_request; request.version=2;
    if (call(NETCTL_PING,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    request=readonly_request; request.reserved=1;
    if (call(NETCTL_PING,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    request=readonly_request; request.timeout_seconds=0;
    if (call(NETCTL_PING,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    request=readonly_request; request.sequence=65536;
    if (call(NETCTL_PING,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    request=readonly_request; request.destination=0xffffffffu;
    if (call(NETCTL_PING,(uintptr_t)&request,48)!=SYSCALL_EINVAL) goto fail;
    write("NETCTL ABI probe PASS\n",22); return 0;
fail:
    write("NETCTL ABI probe FAIL\n",22); return 1;
}
