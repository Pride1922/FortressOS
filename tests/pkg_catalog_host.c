#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/catalog.h"
static uint8_t data[FPK_CATALOG_MAX],good[1024],artifact[4096];
static fpk_catalog_policy policy,good_policy;
static fpk_verify_context context;
static uint64_t le(const uint8_t *p,unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}
static size_t artifact_size,position;
static long read_bytes(void *arg,void *out,size_t n) {
    (void)arg;
    if(n>artifact_size-position) n=artifact_size-position;
    if(n>7) n=7;
    memcpy(out,artifact+position,n); position+=n; return (long)n;
}
long tool_syscall(long a,uintptr_t b,uintptr_t c,uintptr_t d) {
    (void)a;(void)b;(void)c;(void)d;assert(!"unexpected syscall");return -1;
}
int main(void) {
    FILE *a=fopen("build/pkg-format/fixture.fpk","rb"); assert(a);
    artifact_size=fread(artifact,1,sizeof(artifact),a); assert(artifact_size<sizeof(artifact) && !ferror(a)); assert(!fclose(a));
    FILE *f=fopen("build/pkg-format/catalog-vectors.bin","rb"); assert(f);
    unsigned count=0,chain_count=0; size_t good_size=0;
    for(;;) {
        uint8_t h[6],rules[336]; size_t got=fread(h,1,sizeof(h),f);
        if(!got) { assert(!ferror(f)); break; }
        assert(got==sizeof(h) && fread(rules,1,sizeof(rules),f)==sizeof(rules));
        size_t n=(size_t)le(h+2,4); assert(n<=sizeof(data) && fread(data,1,n,f)==n);
        memcpy(policy.repository,rules,16); policy.root_version=le(rules+16,8);
        policy.revision_floor=le(rules+24,8); policy.now=le(rules+32,8);
        memcpy(policy.floor_sha256,rules+40,32); policy.threshold=(unsigned)le(rules+72,4);
        policy.key_count=(unsigned)le(rules+76,4); memcpy(policy.keys,rules+80,256);
        fpk_catalog out,before; memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        int result=fpk_catalog_verify(data,n,&policy,&out);
        if((result==0)!=h[0]) { fprintf(stderr,"catalog vector %u result %d expected %u\n",count,result,h[0]);return 1; }
        if(result) assert(!memcmp(&out,&before,sizeof(out)));
        if(h[1]!=2) {
            position=0;
            const uint8_t *name=data+73;
            size_t name_n=data[72];
            result=fpk_verify_authorized(&context,data,n,&policy,name,name_n,read_bytes,NULL);
            if((result==0)!=h[1] || context.verified!=h[1]) {
                fprintf(stderr,"chain vector %u result %d expected %u\n",count,result,h[1]);return 1;
            }
            chain_count++;
        }
        if(!count) { assert(n<sizeof(good));memcpy(good,data,n);good_size=n;good_policy=policy; }
        count++;
    }
    assert(!fclose(f));
    fpk_catalog out;
    for(size_t i=0;i<good_size;i++) assert(fpk_catalog_verify(good,i,&good_policy,&out)<0);
    assert(fpk_catalog_verify(NULL,0,&good_policy,&out)<0);
    assert(fpk_catalog_verify(good,good_size,NULL,&out)<0);
    printf("PKG catalog PASS: %u vectors, %zu truncations, %u artifact chains\n",count,good_size,chain_count);
}
