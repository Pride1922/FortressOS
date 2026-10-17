#include "internal.h"
int cr_entropy(uint8_t seed[32]) {
#ifdef CRYPTO_HOST_TEST
    extern int64_t crypto_test_getrandom(void *,size_t,uint64_t);
    int64_t r=crypto_test_getrandom(seed,32,0);
#else
    register uint64_t nr __asm__("rax")=SYS_GETRANDOM;
    __asm__ volatile("syscall":"+a"(nr):"D"(seed),"S"((uint64_t)32),"d"((uint64_t)0):"rcx","r11","memory","cc");
    int64_t r=(int64_t)nr;
#endif
    return r==32 ? 0 : r<0 ? (int)r : SYSCALL_EIO;
}
