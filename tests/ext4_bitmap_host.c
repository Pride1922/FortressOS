#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext4_bitmap.h"
static uint32_t reference(const uint8_t *map,uint32_t bits) {
 uint32_t free=0;for(uint32_t b=0;b<bits;b++) free+=!(map[b/8]&(1u<<(b%8)));return free;
}
int main(void) {
 for(unsigned value=0;value<256;value++){
  uint8_t byte=(uint8_t)value;assert(e4_bitmap_used_byte(byte)==8-reference(&byte,8));
  for(unsigned bits=0;bits<=8;bits++)assert(e4_bitmap_free_count(&byte,bits)==reference(&byte,bits));
 }
 uint32_t state=0x482156ab;
 for(unsigned trial=0;trial<10000;trial++) {
  state=state*1664525u+1013904223u;uint32_t bits=state%32769;
  size_t bytes=(bits+7)/8;uint8_t *allocation=malloc(bytes+1);assert(allocation);uint8_t *map=allocation+1;
  for(size_t b=0;b<bytes;b++){state=state*1664525u+1013904223u;map[b]=(uint8_t)(state>>24);}
  assert(e4_bitmap_free_count(map,bits)==reference(map,bits));
  if(bits%8){uint32_t before=e4_bitmap_free_count(map,bits);map[bytes-1]^=(uint8_t)(0xffu<<(bits%8));assert(e4_bitmap_free_count(map,bits)==before);}
  free(allocation);
 }
 puts("PASS bitmap counts: exhaustive bytes/tails, 10000 unaligned exact-length random maps, ignored padding bits");return 0;
}
