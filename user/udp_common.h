#ifndef FORTRESS_UDP_USER_COMMON_H
#define FORTRESS_UDP_USER_COMMON_H
#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
static inline long udp_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                             uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10")=d;
    register uintptr_t r8 __asm__("r8")=e;
    register uintptr_t r9 __asm__("r9")=f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a),"S"(b),"d"(c),
        "r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory","cc");
    return nr;
}
static inline bool udp_equal(const char *a, const char *b) {
    while (*a && *a==*b) { ++a; ++b; } return *a==*b;
}
static inline size_t udp_length(const char *p) { size_t n=0; while (p[n]) ++n; return n; }
static inline bool udp_write(const char *p) {
    size_t n=udp_length(p), at=0;
    while (at<n) {
        long result=udp_call(SYS_WRITE,1,(uintptr_t)(p+at),n-at,0,0,0);
        if (result<=0) return false;
        at+=(size_t)result;
    }
    return true;
}
static inline bool udp_number(const char **p, unsigned max, unsigned *out) {
    unsigned n=0, digits=0;
    while (**p>='0' && **p<='9') {
        if (++digits>5) return false;
        n=n*10+(unsigned)(*(*p)++-'0'); if (n>max) return false;
    }
    *out=n; return digits!=0;
}
static inline bool udp_ip(const char *p, uint32_t *out) {
    uint32_t n=0;
    for (unsigned i=0; i<4; ++i) {
        unsigned octet; if (!udp_number(&p,255,&octet)) return false;
        n=(n<<8)|octet;
        if (i<3 && *p++!='.') return false;
    }
    if (*p) return false;
    *out=__builtin_bswap32(n); return true;
}
#endif
