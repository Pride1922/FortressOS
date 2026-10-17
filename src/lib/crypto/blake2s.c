#include "internal.h"

/* RFC 7693, sequential BLAKE2s (fanout=depth=1, no salt/personalization). */
static const uint32_t iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};
static const uint8_t sigma[10][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0}
};
static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32-n));
}
static void mix(uint32_t v[16], unsigned a, unsigned b, unsigned c,
                unsigned d, uint32_t x, uint32_t y) {
    v[a] += v[b] + x; v[d] = rotr(v[d] ^ v[a],16);
    v[c] += v[d]; v[b] = rotr(v[b] ^ v[c],12);
    v[a] += v[b] + y; v[d] = rotr(v[d] ^ v[a],8);
    v[c] += v[d]; v[b] = rotr(v[b] ^ v[c],7);
}
static void compress(crypto_blake2s_ctx *ctx, bool last) {
    uint32_t m[16], v[16];
    for (unsigned i=0;i<16;i++) {
        const uint8_t *p = ctx->buffer + 4*i;
        m[i] = (uint32_t)p[0] | (uint32_t)p[1]<<8 |
               (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
    }
    for (unsigned i=0;i<8;i++) { v[i]=ctx->h[i]; v[i+8]=iv[i]; }
    v[12] ^= (uint32_t)ctx->bytes;
    v[13] ^= (uint32_t)(ctx->bytes >> 32);
    if (last) v[14] = ~v[14];
    for (unsigned r=0;r<10;r++) {
        const uint8_t *s = sigma[r];
        mix(v,0,4,8,12,m[s[0]],m[s[1]]);
        mix(v,1,5,9,13,m[s[2]],m[s[3]]);
        mix(v,2,6,10,14,m[s[4]],m[s[5]]);
        mix(v,3,7,11,15,m[s[6]],m[s[7]]);
        mix(v,0,5,10,15,m[s[8]],m[s[9]]);
        mix(v,1,6,11,12,m[s[10]],m[s[11]]);
        mix(v,2,7,8,13,m[s[12]],m[s[13]]);
        mix(v,3,4,9,14,m[s[14]],m[s[15]]);
    }
    for (unsigned i=0;i<8;i++) ctx->h[i] ^= v[i] ^ v[i+8];
    crypto_wipe(m,sizeof(m));
    crypto_wipe(v,sizeof(v));
}
static bool valid(const crypto_blake2s_ctx *ctx) {
    return cr_range(ctx,sizeof(*ctx)) && ctx->active &&
           ctx->out_len >= 1 && ctx->out_len <= CRYPTO_BLAKE2S_MAX_OUT &&
           ctx->used <= 64 && ctx->bytes <= CRYPTO_MAX_INPUT &&
           ctx->used <= CRYPTO_MAX_INPUT-ctx->bytes;
}
int crypto_blake2s_init(crypto_blake2s_ctx *ctx, size_t out_len,
                        const void *key, size_t key_len) {
    if (!cr_range(ctx,sizeof(*ctx)) || out_len<1 || out_len>32 ||
        key_len>32 || !cr_range(key,key_len) ||
        cr_overlap(ctx,sizeof(*ctx),key,key_len)) return SYSCALL_EINVAL;
    crypto_wipe(ctx,sizeof(*ctx));
    for (unsigned i=0;i<8;i++) ctx->h[i]=iv[i];
    ctx->h[0] ^= UINT32_C(0x01010000) ^ (uint32_t)(key_len<<8) ^ (uint32_t)out_len;
    ctx->out_len=out_len;
    ctx->active=true;
    if (key_len) { memcpy(ctx->buffer,key,key_len); ctx->used=64; }
    return 0;
}
int crypto_blake2s_update(crypto_blake2s_ctx *ctx, const void *in, size_t in_len) {
    if (!valid(ctx) || !cr_range(in,in_len) ||
        cr_overlap(ctx,sizeof(*ctx),in,in_len) ||
        in_len>CRYPTO_MAX_INPUT-ctx->bytes-ctx->used) return SYSCALL_EINVAL;
    const uint8_t *p=in;
    while (in_len) {
        /* Keep the last full block until more data arrives or final is called. */
        if (ctx->used==64) {
            ctx->bytes+=64;
            compress(ctx,false);
            crypto_wipe(ctx->buffer,sizeof(ctx->buffer));
            ctx->used=0;
        }
        size_t take=64-ctx->used;
        if (take>in_len) take=in_len;
        memcpy(ctx->buffer+ctx->used,p,take);
        ctx->used+=take; p+=take; in_len-=take;
    }
    return 0;
}
int crypto_blake2s_final(crypto_blake2s_ctx *ctx, uint8_t *out) {
    if (!valid(ctx) || !cr_output(out,ctx->out_len,ctx,sizeof(*ctx)))
        return SYSCALL_EINVAL;
    ctx->bytes+=ctx->used;
    for (size_t i=ctx->used;i<64;i++) ctx->buffer[i]=0;
    compress(ctx,true);
    for (size_t i=0;i<ctx->out_len;i++) out[i]=(uint8_t)(ctx->h[i/4] >> (8*(i%4)));
    crypto_wipe(ctx,sizeof(*ctx));
    return 0;
}
int crypto_blake2s(uint8_t *out, size_t out_len, const void *in, size_t in_len,
                   const void *key, size_t key_len) {
    if (out_len<1 || out_len>32 || key_len>32 ||
        !cr_output(out,out_len,in,in_len) || !cr_output(out,out_len,key,key_len) ||
        in_len>CRYPTO_MAX_INPUT-(key_len ? 64 : 0)) return SYSCALL_EINVAL;
    crypto_blake2s_ctx ctx;
    int rc=crypto_blake2s_init(&ctx,out_len,key,key_len);
    if (!rc) rc=crypto_blake2s_update(&ctx,in,in_len);
    if (!rc) rc=crypto_blake2s_final(&ctx,out);
    crypto_wipe(&ctx,sizeof(ctx));
    return rc;
}
