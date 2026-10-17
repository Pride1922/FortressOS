#ifndef FORTRESS_CRYPTO_INTERNAL_H
#define FORTRESS_CRYPTO_INTERNAL_H
#include "crypto.h"
#include "namespace.h"
#include "upstream/src/monocypher.h"
#include "upstream/src/optional/monocypher-ed25519.h"
#include "namespace_end.h"
#include "string.h"
#include "syscall_abi.h"
static inline bool cr_range(const void *p,size_t n) {
    return (!n || p) && n<=CRYPTO_MAX_INPUT && (uintptr_t)p<=UINTPTR_MAX-n;
}
static inline bool cr_overlap(const void *a,size_t an,const void *b,size_t bn) {
    return an && bn && (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an;
}
static inline bool cr_output(const void *out,size_t n,const void *input,size_t len) {
    return cr_range(out,n) && cr_range(input,len) && !cr_overlap(out,n,input,len);
}
int cr_entropy(uint8_t seed[32]);
#endif
