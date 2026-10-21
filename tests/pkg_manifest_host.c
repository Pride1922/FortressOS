#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/format.h"
static uint8_t bytes[FPK_MANIFEST_MAX];
static fpk_manifest output, before;
int main(void) {
    FILE *f=fopen("build/pkg-format/manifest-vectors.bin","rb"); assert(f);
    unsigned count=0;
    for (;;) {
        uint8_t h[5]; size_t got=fread(h,1,5,f);
        if(!got) { assert(!ferror(f)); break; }
        assert(got==5);
        uint32_t n=(uint32_t)h[1] | (uint32_t)h[2]<<8 | (uint32_t)h[3]<<16 | (uint32_t)h[4]<<24;
        assert(n<=sizeof(bytes) && fread(bytes,1,n,f)==n);
        memset(&output,0xa5,sizeof(output)); memcpy(&before,&output,sizeof(output));
        int r=fpk_parse_manifest(bytes,n,&output);
        if((r==0)!=h[0]) { fprintf(stderr,"vector %u result %d expected %u\n",count,r,h[0]); return 1; }
        if(r) assert(!memcmp(&output,&before,sizeof(output)));
        else {
            assert(output.build==1 && output.base_min==1 && output.base_max==3);
            assert(output.package.length==4 && !memcmp(output.package.bytes,"demo",4));
            assert(output.file_count>=1 && output.file_count<=16);
            assert(output.files[0].role==1 && output.files[0].mode==0755);
            assert(output.files[0].sha256[31]==31);
            if(count==0) assert(output.archive_bytes==3072);
        }
        count++;
    }
    assert(!fclose(f));
    assert(fpk_parse_manifest(NULL,0,&output)==-1);
    assert(fpk_parse_manifest(bytes,sizeof(bytes)+1,&output)==-1);
    assert(fpk_parse_manifest(bytes,0,NULL)==-1);
    /* Deterministic malformed-input stress, separately from golden vectors.
     * This is a memory-safety/output-publication check, not a validity oracle. */
    uint32_t seed=0x504b4731u;
    for(unsigned trial=0;trial<10000;trial++) {
        seed=seed*1664525u+1013904223u;
        size_t n=seed%sizeof(bytes);
        for(size_t i=0;i<n;i++) {
            seed=seed*1664525u+1013904223u; bytes[i]=(uint8_t)(seed>>24);
        }
        memset(&output,0xa5,sizeof(output)); memcpy(&before,&output,sizeof(output));
        int r=fpk_parse_manifest(bytes,n,&output);
        if(r) assert(!memcmp(&output,&before,sizeof(output)));
    }
    printf("PKG-1 manifest PASS (%u vectors; syntax only)\n",count);
}
