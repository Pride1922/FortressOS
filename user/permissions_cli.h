#ifndef FORTRESS_PERMISSIONS_CLI_H
#define FORTRESS_PERMISSIONS_CLI_H
#include "syscall_abi.h"
static inline bool permission_number(const char *s,size_t len,unsigned base,uint32_t limit,uint32_t *out) {
    if (!s || !len) return false;
    uint32_t value=0;
    for (size_t i=0;i<len;i++) {
        unsigned d=(unsigned)(s[i]-'0');
        if (d>=base || value>(limit-d)/base) return false;
        value=value*base+d;
    }
    *out=value;return true;
}
#ifdef PERMISSIONS_CLI_HOST_TEST
long permission_call4(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d);
#else
static inline long permission_call4(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    register uintptr_t r10 __asm__("r10")=d;
    __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");
    return n;
}
#endif
#endif
