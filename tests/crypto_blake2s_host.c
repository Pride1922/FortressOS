#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "syscall_abi.h"
#include "vectors.h"

static unsigned wipes;
void crypto_test_wiped(const void *p, size_t n) {
    const uint8_t *b=p;
    for (size_t i=0;i<n;i++) assert(b[i]==0);
    wipes++;
}
static void zero(const void *p, size_t n) {
    const uint8_t *b=p;
    for (size_t i=0;i<n;i++) assert(b[i]==0);
}
static size_t unhex(uint8_t *out, const char *s) {
    size_t n=strlen(s)/2;
    for (size_t i=0;i<n;i++) {
        unsigned x;
        assert(sscanf(s+2*i,"%2x",&x)==1);
        out[i]=(uint8_t)x;
    }
    return n;
}
static void cases(void) {
    uint8_t input[4098], key[33], expected[32], out[34];
    for (size_t v=0;v<sizeof(vectors)/sizeof(vectors[0]);v++) {
        /* Deliberately unaligned byte inputs, key and output. */
        size_t n=unhex(input+1,vectors[v].in);
        size_t kn=unhex(key+1,vectors[v].key);
        size_t on=unhex(expected,vectors[v].hash);
        memset(out,0xa5,sizeof(out));
        assert(!crypto_blake2s(out+1,on,n ? input+1 : NULL,n,kn ? key+1 : NULL,kn));
        assert(!memcmp(out+1,expected,on));
        assert(out[0]==0xa5 && out[on+1]==0xa5);
        size_t chunks[]={1,2,63,64,65,127,128,129,4097};
        for (size_t j=0;j<sizeof(chunks)/sizeof(chunks[0]);j++) {
            crypto_blake2s_ctx ctx;
            assert(!crypto_blake2s_init(&ctx,on,kn ? key+1 : NULL,kn));
            for (size_t pos=0;pos<n;) {
                size_t take=chunks[j]; if (take>n-pos) take=n-pos;
                assert(!crypto_blake2s_update(&ctx,NULL,0));
                assert(!crypto_blake2s_update(&ctx,input+1+pos,take));
                pos+=take;
            }
            assert(!crypto_blake2s_final(&ctx,out+1));
            assert(!memcmp(out+1,expected,on)); zero(&ctx,sizeof(ctx));
            assert(out[0]==0xa5 && out[on+1]==0xa5);
        }
        /* Every two-way split of all 256 official keyed vectors. */
        if (v<256) for (size_t split=0;split<=n;split++) {
            crypto_blake2s_ctx ctx;
            assert(!crypto_blake2s_init(&ctx,on,key+1,kn));
            assert(!crypto_blake2s_update(&ctx,input+1,split));
            assert(!crypto_blake2s_update(&ctx,input+1+split,n-split));
            assert(!crypto_blake2s_final(&ctx,out+1));
            assert(!memcmp(out+1,expected,on)); zero(&ctx,sizeof(ctx));
        }
    }
}
#define BAD(expr) assert((expr)==SYSCALL_EINVAL)
static void negatives(void) {
    crypto_blake2s_ctx ctx, saved;
    uint8_t out[64], before[64], key[33]={0};
    memset(out,0xa5,sizeof(out)); memcpy(before,out,sizeof(out));
    memset(&ctx,0,sizeof(ctx));
    BAD(crypto_blake2s_update(&ctx,NULL,0));
    BAD(crypto_blake2s_final(&ctx,out));
    assert(!crypto_blake2s_init(&ctx,32,key,32)); saved=ctx;
    BAD(crypto_blake2s_init(NULL,32,NULL,0));
    BAD(crypto_blake2s_init(&ctx,0,NULL,0));
    BAD(crypto_blake2s_init(&ctx,33,NULL,0));
    BAD(crypto_blake2s_init(&ctx,32,key,33));
    BAD(crypto_blake2s_init(&ctx,32,NULL,1));
    BAD(crypto_blake2s_init(&ctx,32,&ctx,1));
    BAD(crypto_blake2s_init(&ctx,32,(void *)(UINTPTR_MAX-1),2));
    BAD(crypto_blake2s_update(NULL,key,1));
    BAD(crypto_blake2s_update(&ctx,NULL,1));
    BAD(crypto_blake2s_update(&ctx,&ctx,1));
    BAD(crypto_blake2s_update(&ctx,(void *)(UINTPTR_MAX-1),2));
    BAD(crypto_blake2s_update(&ctx,key,SIZE_MAX));
    BAD(crypto_blake2s_final(NULL,out));
    BAD(crypto_blake2s_final(&ctx,NULL));
    BAD(crypto_blake2s_final(&ctx,(void *)&ctx));
    BAD(crypto_blake2s_final(&ctx,(void *)(UINTPTR_MAX-1)));
    assert(!memcmp(&ctx,&saved,sizeof(ctx)));
    assert(!memcmp(out,before,sizeof(out)));
    BAD(crypto_blake2s(out,0,key,1,NULL,0));
    BAD(crypto_blake2s(out,33,key,1,NULL,0));
    BAD(crypto_blake2s(out,32,NULL,1,NULL,0));
    BAD(crypto_blake2s(out,32,NULL,0,key,33));
    BAD(crypto_blake2s(out,32,NULL,0,NULL,1));
    BAD(crypto_blake2s(out,32,out+1,1,NULL,0));
    BAD(crypto_blake2s(out,32,NULL,0,out+1,1));
    BAD(crypto_blake2s(NULL,32,NULL,0,NULL,0));
    BAD(crypto_blake2s(out,32,(void *)(UINTPTR_MAX-1),2,NULL,0));
    BAD(crypto_blake2s(out,32,key,SIZE_MAX,NULL,0));
    BAD(crypto_blake2s(out,32,key,CRYPTO_MAX_INPUT,key,1));
    assert(!memcmp(out,before,sizeof(out)));
    assert(!crypto_blake2s_final(&ctx,out)); zero(&ctx,sizeof(ctx));
    BAD(crypto_blake2s_final(&ctx,out));
    BAD(crypto_blake2s_update(&ctx,key,1));
    /* Cumulative count overflow is rejected before reading input. */
    assert(!crypto_blake2s_init(&ctx,32,NULL,0));
    ctx.bytes=CRYPTO_MAX_INPUT-63; ctx.used=63; saved=ctx;
    BAD(crypto_blake2s_update(&ctx,key,1));
    assert(!memcmp(&ctx,&saved,sizeof(ctx)));
    ctx.used=65; BAD(crypto_blake2s_final(&ctx,out));
    crypto_wipe(&ctx,sizeof(ctx));
    assert(!crypto_blake2s_init(&ctx,1,NULL,0));
    assert(!crypto_blake2s_final(&ctx,out)); zero(&ctx,sizeof(ctx));
}
int main(void) {
    cases(); negatives(); assert(wipes>0);
    puts("PASS: BLAKE2s vectors, split/chunk streaming, canaries, invalid arguments, lifecycle, erasure");
    return 0;
}
