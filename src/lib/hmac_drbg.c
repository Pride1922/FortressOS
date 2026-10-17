#include "hmac_drbg.h"
#include "random.h"
#include "../../user/tools/digest.h"

/* All lengths internal and <=128 bytes, so digest_update cannot overflow.
 * Output may alias key or input: publish only after both SHA passes. */
static void hmac(const uint8_t key[32], const void *a, size_t an,
                 const void *b, size_t bn, const void *c, size_t cn,
                 uint8_t out[32]) {
    uint8_t pad[64], inner[32];
    digest_t ctx;
    for (unsigned i=0;i<64;i++) pad[i]=(i<32 ? key[i] : 0)^0x36;
    digest_init(&ctx,DIGEST_SHA256);
    digest_update(&ctx,pad,64);
    digest_update(&ctx,a,an); digest_update(&ctx,b,bn); digest_update(&ctx,c,cn);
    digest_final(&ctx,inner);
    for (unsigned i=0;i<64;i++) pad[i]=(i<32 ? key[i] : 0)^0x5c;
    digest_init(&ctx,DIGEST_SHA256);
    digest_update(&ctx,pad,64); digest_update(&ctx,inner,32);
    digest_final(&ctx,out);
    random_wipe(&ctx,sizeof(ctx)); random_wipe(pad,sizeof(pad));
    random_wipe(inner,sizeof(inner));
}
static void update(hmac_drbg_t *d, const uint8_t *seed, size_t n) {
    uint8_t sep=0;
    hmac(d->k,d->v,32,&sep,1,seed,n,d->k);
    hmac(d->k,d->v,32,0,0,0,0,d->v);
    if (n) {
        sep=1;
        hmac(d->k,d->v,32,&sep,1,seed,n,d->k);
        hmac(d->k,d->v,32,0,0,0,0,d->v);
    }
}
void hmac_drbg_init(hmac_drbg_t *d, const uint8_t *seed, size_t n) {
    for (unsigned i=0;i<32;i++) { d->k[i]=0; d->v[i]=1; }
    update(d,seed,n); d->requests=1;
}
void hmac_drbg_reseed(hmac_drbg_t *d, const uint8_t *seed, size_t n) {
    update(d,seed,n); d->requests=1;
}
bool hmac_drbg_generate(hmac_drbg_t *d, uint8_t *out, size_t n) {
    if (!n || n>256 || !d->requests || d->requests>HMAC_DRBG_INTERVAL) return false;
    while (n) {
        hmac(d->k,d->v,32,0,0,0,0,d->v);
        size_t take=n<32 ? n : 32;
        for (size_t i=0;i<take;i++) out[i]=d->v[i];
        out+=take; n-=take;
    }
    update(d,0,0); d->requests++;
    return true;
}
