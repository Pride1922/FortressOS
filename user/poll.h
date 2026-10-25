#ifndef FORTRESS_USER_POLL_H
#define FORTRESS_USER_POLL_H

#include "types.h"
#include "syscall_abi.h"

#ifndef POLL_SYSCALL
static inline long poll_syscall_3(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    register uintptr_t r10 __asm__("r10") = 0;
    register uintptr_t r8  __asm__("r8")  = 0;
    register uintptr_t r9  __asm__("r9")  = 0;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}
#define POLL_SYSCALL(nr, a, b, c) poll_syscall_3(nr, (uintptr_t)(a), (uintptr_t)(b), (uintptr_t)(c))
#endif

static inline int poll(struct pollfd *fds, uint64_t nfds, int64_t timeout_ms) {
    return (int)POLL_SYSCALL(SYS_POLL, fds, nfds, timeout_ms);
}

#endif
