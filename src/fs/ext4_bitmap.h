#ifndef FORTRESS_EXT4_BITMAP_H
#define FORTRESS_EXT4_BITMAP_H
#include "types.h"
static inline bool e4_bitmap_equal_bytes(const uint8_t *a,const uint8_t *b,size_t bytes) {
    size_t i=0;
    for (; bytes-i>=8; i+=8) {
        uint64_t left,right;
        __builtin_memcpy(&left,a+i,sizeof(left));
        __builtin_memcpy(&right,b+i,sizeof(right));
        if (left!=right) return false;
    }
    for (; i<bytes; ++i) if (a[i]!=b[i]) return false;
    return true;
}
/* Portable scalar population count: no POPCNT/SIMD requirement, lazy state,
 * alignment assumptions or reads beyond the caller's exact bit count. */
static inline unsigned e4_bitmap_used_byte(uint8_t value) {
    unsigned n=value;
    n=n-((n>>1)&0x55u);
    n=(n&0x33u)+((n>>2)&0x33u);
    return (n+(n>>4))&0x0fu;
}
static inline uint32_t e4_bitmap_free_count(const uint8_t *map,uint32_t count) {
    uint32_t used=0,bytes=count/8,i=0;
    /* memcpy permits unaligned input and reads exactly eight available bytes.
     * Scalar SWAR counts all 64 bits without requiring POPCNT or SIMD. */
    for (; bytes-i>=8; i+=8) {
        uint64_t word;
        __builtin_memcpy(&word,map+i,sizeof(word));
        word-=((word>>1)&0x5555555555555555ULL);
        word=(word&0x3333333333333333ULL)+((word>>2)&0x3333333333333333ULL);
        word=(word+(word>>4))&0x0f0f0f0f0f0f0f0fULL;
        used+=(uint32_t)((word*0x0101010101010101ULL)>>56);
    }
    for (; i<bytes; i++) used+=e4_bitmap_used_byte(map[i]);
    unsigned tail=count%8;
    if (tail) used+=e4_bitmap_used_byte(map[bytes]&((1u<<tail)-1u));
    return count-used;
}
#endif
