#ifndef USER_RESOLV_CONF_H
#define USER_RESOLV_CONF_H

#include "types.h"
#include "syscall_abi.h"

#ifndef RESOLV_SYSCALL
#if defined(DNS_CALL)
#define RESOLV_SYSCALL(nr, a, b, c) DNS_CALL(nr, a, b, c, 0, 0, 0)
#elif defined(IFCONFIG_SYSCALL)
#define RESOLV_SYSCALL(nr, a, b, c) IFCONFIG_SYSCALL(nr, a, b, c)
#elif defined(IFUP_SYSCALL)
#define RESOLV_SYSCALL(nr, a, b, c) IFUP_SYSCALL(nr, a, b, c)
#else
static inline long resolv_default_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#define RESOLV_SYSCALL resolv_default_syscall
#endif
#endif

#include "machine_config.h"
static inline long resolv_machine_call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    return RESOLV_SYSCALL(nr,a,b,c);
}
/* Returns the resolv.conf path to read/write.
 * Order: /etc/network/resolv.conf if installed (with /etc/resolv.conf fallback),
 *        else /tmp/resolv.conf if it exists,
 *        else /mnt/.fortress/resolv.conf if it exists,
 *        else /tmp/resolv.conf (default write target). */
static inline const char *resolv_conf_path(void) {
    if (machine_installed(resolv_machine_call)) {
        static const char path_net[] = "/etc/network/resolv.conf";
        static const char path_legacy[] = "/etc/resolv.conf";
        long fd = RESOLV_SYSCALL(SYS_OPEN, (uintptr_t)path_net, 0, 0);
        if (fd >= 0) {
            RESOLV_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            return path_net;
        }
        fd = RESOLV_SYSCALL(SYS_OPEN, (uintptr_t)path_legacy, 0, 0);
        if (fd >= 0) {
            RESOLV_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            return path_legacy;
        }
        return path_net;
    }
    static const char path_tmp[] = "/tmp/resolv.conf";
    static const char path_mnt[] = "/mnt/.fortress/resolv.conf";

    long fd = RESOLV_SYSCALL(SYS_OPEN, (uintptr_t)path_tmp, 0, 0);
    if (fd >= 0) {
        RESOLV_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        return path_tmp;
    }

    fd = RESOLV_SYSCALL(SYS_OPEN, (uintptr_t)path_mnt, 0, 0);
    if (fd >= 0) {
        RESOLV_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        return path_mnt;
    }

    return path_tmp;
}

#endif /* USER_RESOLV_CONF_H */
