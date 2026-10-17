#include "types.h"
#include "syscall_abi.h"
int64_t crypto_test_getrandom(void *p,size_t n,uint64_t f) {(void)p;(void)n;(void)f;return SYSCALL_EIO;}
void crypto_test_wiped(const void *p,size_t n) {(void)p;(void)n;}
