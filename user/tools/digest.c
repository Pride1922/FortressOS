#include "digest.h"

static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }
static uint32_t ror(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
static uint32_t word(const uint8_t *p, bool big) {
    if (big) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint8_t md5_s[16] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
static void md5_block(digest_t *ctx) {
    uint32_t w[16];
    for (unsigned i = 0; i < 16; i++) w[i] = word(ctx->block + i * 4, false);
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f; unsigned g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t next = b + rol(a + f + md5_k[i] + w[g], md5_s[(i / 16) * 4 + i % 4]);
        a = d; d = c; c = b; b = next;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
}
static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static void sha_block(digest_t *ctx) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) w[i] = word(ctx->block + i * 4, true);
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (ror(x,7) ^ ror(x,18) ^ (x >> 3)) + w[i-7]
             + (ror(y,17) ^ ror(y,19) ^ (y >> 10));
    }
    uint32_t a=ctx->state[0], b=ctx->state[1], c=ctx->state[2], d=ctx->state[3];
    uint32_t e=ctx->state[4], f=ctx->state[5], g=ctx->state[6], h=ctx->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e,6) ^ ror(e,11) ^ ror(e,25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void compress(digest_t *ctx) {
    if (ctx->kind == DIGEST_MD5) md5_block(ctx); else sha_block(ctx);
}
void digest_init(digest_t *ctx, digest_kind_t kind) {
    static const uint32_t sha_initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    *ctx = (digest_t){.kind = kind};
    if (kind == DIGEST_MD5) {
        ctx->state[0]=0x67452301; ctx->state[1]=0xefcdab89;
        ctx->state[2]=0x98badcfe; ctx->state[3]=0x10325476;
    } else for (unsigned i=0; i<8; i++) ctx->state[i]=sha_initial[i];
}
bool digest_update(digest_t *ctx, const void *data, size_t size) {
    if (ctx->bytes > UINT64_MAX / 8 || size > UINT64_MAX / 8 - ctx->bytes) return false;
    const uint8_t *p = data;
    ctx->bytes += size;
    while (size) {
        size_t n = 64 - ctx->used;
        if (n > size) n = size;
        for (size_t i=0; i<n; i++) ctx->block[ctx->used+i]=p[i];
        ctx->used += n; p += n; size -= n;
        if (ctx->used == 64) { compress(ctx); ctx->used=0; }
    }
    return true;
}
size_t digest_size(digest_kind_t kind) { return kind == DIGEST_MD5 ? 16 : 32; }
void digest_final(digest_t *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->bytes * 8;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < 64) ctx->block[ctx->used++]=0;
        compress(ctx); ctx->used=0;
    }
    while (ctx->used < 56) ctx->block[ctx->used++]=0;
    for (unsigned i=0; i<8; i++) ctx->block[56+i] = (uint8_t)(bits >> ((ctx->kind == DIGEST_MD5 ? i : 7-i) * 8));
    compress(ctx);
    for (size_t i=0; i<digest_size(ctx->kind); i++) {
        unsigned shift = (ctx->kind == DIGEST_MD5 ? i%4 : 3-i%4) * 8;
        out[i]=(uint8_t)(ctx->state[i/4] >> shift);
    }
}
