#ifndef FORTRESS_EXT4_BITMAP_H
#define FORTRESS_EXT4_BITMAP_H
#include "types.h"
/* Portable scalar population count: no POPCNT/SIMD requirement, lazy state,
 * alignment assumptions or reads beyond the caller's exact bit count. */
static inline unsigned e4_bitmap_used_byte(uint8_t value) {
    unsigned n=value;
    n=n-((n>>1)&0x55u);
    n=(n&0x33u)+((n>>2)&0x33u);
    return (n+(n>>4))&0x0fu;
}
static inline uint32_t e4_bitmap_free_count(const uint8_t *map,uint32_t count) {
    uint32_t used=0,bytes=count/8;
    for (uint32_t i=0;i<bytes;i++) used+=e4_bitmap_used_byte(map[i]);
    unsigned tail=count%8;
    if (tail) used+=e4_bitmap_used_byte(map[bytes]&((1u<<tail)-1u));
    return count-used;
}
#endif
