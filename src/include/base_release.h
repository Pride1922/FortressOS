#ifndef FORTRESS_BASE_RELEASE_H
#define FORTRESS_BASE_RELEASE_H
#include "types.h"
#include "string.h"
#define BASE_RELEASE_BYTES 64u
#define BASE_LAYOUT 1u
static inline uint64_t base_release_u64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static inline int base_release_parse(const void *data,size_t n,uint64_t *build,uint64_t *abi,uint8_t platform[32]){
    const uint8_t *p=data;bool nonzero=false;
    if(!p||!build||!abi||!platform||n!=BASE_RELEASE_BYTES||memcmp(p,"FOSBASE1",8)||
       !base_release_u64(p+8)||!base_release_u64(p+16)||base_release_u64(p+56)!=BASE_LAYOUT)return -1;
    for(unsigned i=0;i<32;i++)nonzero|=p[24+i]!=0;
    if(!nonzero)return -1;
    *build=base_release_u64(p+8);*abi=base_release_u64(p+16);memcpy(platform,p+24,32);return 0;
}
#endif
