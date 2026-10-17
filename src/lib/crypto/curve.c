#include "internal.h"
int crypto_ed25519_from_seed(const uint8_t seed[32],uint8_t pk[32],uint8_t sk[64]) {
    if(!cr_output(pk,32,seed,32) || !cr_output(sk,64,seed,32) || cr_overlap(pk,32,sk,64)) return SYSCALL_EINVAL;
    uint8_t copy[32]; memcpy(copy,seed,32);
    mono_crypto_ed25519_key_pair(sk,pk,copy); crypto_wipe(copy,sizeof(copy)); return 0;
}
int crypto_ed25519_keygen(uint8_t pk[32],uint8_t sk[64]) {
    if(!cr_range(pk,32) || !cr_range(sk,64) || cr_overlap(pk,32,sk,64)) return SYSCALL_EINVAL;
    uint8_t seed[32]={0}; int r=cr_entropy(seed);
    if(!r) r=crypto_ed25519_from_seed(seed,pk,sk);
    crypto_wipe(seed,sizeof(seed)); return r;
}
int crypto_ed25519_sign(uint8_t sig[64],const uint8_t sk[64],const uint8_t *msg,size_t n) {
    if(!cr_output(sig,64,sk,64) || !cr_output(sig,64,msg,n)) return SYSCALL_EINVAL;
    mono_crypto_ed25519_sign(sig,sk,msg,n); return 0;
}
/* Enforce canonical field encoding in the public verification path. The
 * upstream cofactored RFC8032 equation accepts noncanonical encodings; do not
 * silently inherit that policy. Comparison here is public-data variable time. */
static bool canonical(const uint8_t p[32]) {
    for(int i=31;i>=0;i--) {
        unsigned a=p[i] & (i==31 ? 127 : 255);
        unsigned b=i==0 ? 0xed : i==31 ? 0x7f : 0xff;
        if(a!=b) return a<b;
    }
    return false;
}
/* Low-order Edwards encodings map to low-order Montgomery u coordinates.
 * Use upstream point conversion + X25519 zero-result test, not new arithmetic.
 * Public keys/signature points are public. Both sign bits get the same test. */
static bool valid_point(const uint8_t p[32]) {
    static const uint8_t scalar[32]={8};
    static const uint8_t zero[32]={0};
    if(!canonical(p)) return false;
    uint8_t u[32],shared[32];
    mono_crypto_eddsa_to_x25519(u,p);
    mono_crypto_x25519(shared,scalar,u);
    bool ok=mono_crypto_verify32(shared,zero)!=0;
    crypto_wipe(u,32); crypto_wipe(shared,32); return ok;
}
int crypto_ed25519_verify(const uint8_t sig[64],const uint8_t pk[32],const uint8_t *msg,size_t n) {
    if(!cr_range(sig,64) || !cr_range(pk,32) || !cr_range(msg,n)) return SYSCALL_EINVAL;
    if(!valid_point(pk) || !valid_point(sig)) return SYSCALL_EINVAL;
    return mono_crypto_ed25519_check(sig,pk,msg,n);
}
int crypto_x25519_from_seed(const uint8_t seed[32],uint8_t sk[32],uint8_t pk[32]) {
    if(!cr_output(sk,32,seed,32) || !cr_output(pk,32,seed,32) || cr_overlap(pk,32,sk,32)) return SYSCALL_EINVAL;
    memcpy(sk,seed,32); sk[0]&=248;sk[31]=(sk[31]&127)|64;
    mono_crypto_x25519_public_key(pk,sk); return 0;
}
int crypto_x25519_keygen(uint8_t sk[32],uint8_t pk[32]) {
    if(!cr_range(sk,32) || !cr_range(pk,32) || cr_overlap(pk,32,sk,32)) return SYSCALL_EINVAL;
    uint8_t seed[32]={0}; int r=cr_entropy(seed);
    if(!r) r=crypto_x25519_from_seed(seed,sk,pk);
    crypto_wipe(seed,sizeof(seed)); return r;
}
int crypto_x25519(uint8_t shared[32],const uint8_t sk[32],const uint8_t pk[32]) {
    if(!cr_output(shared,32,sk,32) || !cr_output(shared,32,pk,32)) return SYSCALL_EINVAL;
    static const uint8_t zero[32]={0}; uint8_t tmp[32];
    mono_crypto_x25519(tmp,sk,pk);
    int r=mono_crypto_verify32(tmp,zero)==0 ? SYSCALL_EINVAL : 0;
    if(!r) memcpy(shared,tmp,32);
    crypto_wipe(tmp,sizeof(tmp)); return r;
}
