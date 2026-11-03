#include <assert.h>
#include <stdio.h>
#include "crc32c.h"
static uint32_t reference(uint32_t c,const void *bytes,size_t n){const uint8_t *p=bytes;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=(c>>1)^(0x82f63b78u&(0u-(c&1)));}return c;}
int main(void){
 assert(crc32c_raw(0,NULL,0)==0);assert(crc32c_raw(UINT32_MAX,NULL,0)==UINT32_MAX);
 assert((~crc32c_raw(UINT32_MAX,"123456789",9))==0xe3069283u);
 uint8_t bytes[8200];uint32_t rng=0x79ab5321;
 for(unsigned i=0;i<sizeof bytes;i++){rng=rng*1664525+1013904223;bytes[i]=(uint8_t)(rng>>24);}
 for(unsigned i=0;i<3000;i++){
  rng=rng*1664525+1013904223;size_t n=rng%8193,off=(rng>>24)%8,split=n?(rng>>8)%(n+1):0;
  uint32_t expected=reference(rng,bytes+off,n);
  assert(crc32c_raw(rng,bytes+off,n)==expected);
  assert(crc32c_raw(crc32c_raw(rng,bytes+off,split),bytes+off+split,n-split)==expected);
 }
 puts("PASS CRC32C: standard vector, empty/raw seeds, 3000 random unaligned/chained cases");return 0;
}
