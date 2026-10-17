#include "internal.h"
void crypto_wipe(void *p,size_t n) {
    mono_crypto_wipe(p,n);
#ifdef CRYPTO_HOST_TEST
    extern void crypto_test_wiped(const void *,size_t);
    crypto_test_wiped(p,n);
#endif
}
/* Fixed-size upstream comparison; disassembly reviewed for supported compiler.
 * Success/failure itself is public. Do not compose this into a secret predicate. */
int crypto_verify32(const uint8_t a[32],const uint8_t b[32]) {
    if(!cr_range(a,32) || !cr_range(b,32)) return SYSCALL_EINVAL;
    return mono_crypto_verify32(a,b);
}
int crypto_sha256_init(crypto_sha256_ctx *c) {
    if(!cr_range(c,sizeof(*c))) return SYSCALL_EINVAL;
    *c=(crypto_sha256_ctx){0}; digest_init(&c->digest,DIGEST_SHA256); c->active=true; return 0;
}
int crypto_sha256_update(crypto_sha256_ctx *c,const void *p,size_t n) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_range(p,n) ||
       cr_overlap(c,sizeof(*c),p,n) || c->digest.bytes>CRYPTO_MAX_INPUT || n>CRYPTO_MAX_INPUT-c->digest.bytes)
        return SYSCALL_EINVAL;
    return digest_update(&c->digest,p,n) ? 0 : SYSCALL_EINVAL;
}
int crypto_sha256_final(crypto_sha256_ctx *c,uint8_t out[32]) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_output(out,32,c,sizeof(*c))) return SYSCALL_EINVAL;
    digest_final(&c->digest,out); crypto_wipe(c,sizeof(*c)); return 0;
}
int crypto_sha256(const uint8_t *p,size_t n,uint8_t out[32]) {
    if(!cr_output(out,32,p,n)) return SYSCALL_EINVAL;
    crypto_sha256_ctx c; crypto_sha256_init(&c);
    int r=crypto_sha256_update(&c,p,n);
    if(!r) r=crypto_sha256_final(&c,out);
    crypto_wipe(&c,sizeof(c)); return r;
}
_Static_assert(sizeof(mono_crypto_sha512_ctx)<=256,"opaque SHA512 storage");
int crypto_sha512_init(crypto_sha512_ctx *c) {
    if(!cr_range(c,sizeof(*c))) return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; mono_crypto_sha512_init(&m);
    *c=(crypto_sha512_ctx){0}; memcpy(c->state,&m,sizeof(m)); c->active=true;
    crypto_wipe(&m,sizeof(m)); return 0;
}
int crypto_sha512_update(crypto_sha512_ctx *c,const void *p,size_t n) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_range(p,n) ||
       cr_overlap(c,sizeof(*c),p,n) || c->bytes>CRYPTO_MAX_INPUT || n>CRYPTO_MAX_INPUT-c->bytes)
        return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; memcpy(&m,c->state,sizeof(m));
    mono_crypto_sha512_update(&m,p,n); memcpy(c->state,&m,sizeof(m)); c->bytes+=n;
    crypto_wipe(&m,sizeof(m)); return 0;
}
int crypto_sha512_final(crypto_sha512_ctx *c,uint8_t out[64]) {
    if(!cr_range(c,sizeof(*c)) || !c->active || !cr_output(out,64,c,sizeof(*c))) return SYSCALL_EINVAL;
    mono_crypto_sha512_ctx m; memcpy(&m,c->state,sizeof(m));
    mono_crypto_sha512_final(&m,out); crypto_wipe(&m,sizeof(m)); crypto_wipe(c,sizeof(*c)); return 0;
}
int crypto_sha512(const uint8_t *p,size_t n,uint8_t out[64]) {
    if(!cr_output(out,64,p,n)) return SYSCALL_EINVAL;
    crypto_sha512_ctx c; crypto_sha512_init(&c);
    int r=crypto_sha512_update(&c,p,n);
    if(!r) r=crypto_sha512_final(&c,out);
    crypto_wipe(&c,sizeof(c)); return r;
}
int crypto_hmac_sha256(const uint8_t *key,size_t kn,const uint8_t *msg,size_t n,uint8_t out[32]) {
    if(!cr_output(out,32,key,kn) || !cr_output(out,32,msg,n)) return SYSCALL_EINVAL;
    uint8_t pad[64]={0},inner[32]={0}; digest_t c;
    int r=0;
    if(kn>64) { r=crypto_sha256(key,kn,pad); if(r) goto done; }
    else if(kn) memcpy(pad,key,kn);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36;
    digest_init(&c,DIGEST_SHA256); digest_update(&c,pad,64);
    if(!digest_update(&c,msg,n)) {r=SYSCALL_EINVAL;goto done;}
    digest_final(&c,inner);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36^0x5c;
    digest_init(&c,DIGEST_SHA256); digest_update(&c,pad,64); digest_update(&c,inner,32);
    digest_final(&c,out);
done:
    crypto_wipe(pad,sizeof(pad)); crypto_wipe(inner,sizeof(inner)); crypto_wipe(&c,sizeof(c)); return r;
}
