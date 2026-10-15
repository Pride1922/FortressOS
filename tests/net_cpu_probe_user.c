/* Test-only Ring 3 admission probe. --worker N exercises VFS CPU placement. */
#include "types.h"
#include "syscall_abi.h"
#include "netctl_abi.h"
#include "socket_abi.h"

static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

int net_cpu_probe_main(int argc, char **argv) {
    bool ap = argc == 3 && argv[2][0] == '1' && argv[2][1] == '\0';
    netctl_ifget_t info = {.struct_version = 1};
    long query = call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&info, sizeof(info));
    long socket = call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 17);
    bool ok = query == 0 && socket >= 0;
    if (ap) ok = ok && call(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM,6)==SYSCALL_EOPNOTSUPP;
    if (socket >= 0) ok = call(SYS_CLOSE, socket, 0, 0) == 0 && ok;
    const char *msg = !ok ? "NET CPU admission FAIL\n" : ap ? "NET CPU AP control/UDP accepted; TCP rejected PASS\n" : "NET CPU BSP accepted PASS\n";
    size_t n = 0; while (msg[n]) n++;
    call(SYS_WRITE, 1, (uintptr_t)msg, n);
    return ok ? 0 : 1;
}
