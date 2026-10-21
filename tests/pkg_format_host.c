#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "user/pkg/format.h"
static uint8_t header[128];
static void put32(unsigned at, uint32_t x) {
    for (unsigned i=0;i<4;i++) header[at+i]=(uint8_t)(x>>(8*i));
}
static void valid(void) {
    memset(header,0,sizeof(header));
    memcpy(header,"FORTFPK",7); put32(8,1); put32(12,128);
    put32(20,1); put32(32,1); put32(40,1);
}
static void reject(size_t n) {
    fpk_envelope out, before;
    memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
    assert(fpk_parse_envelope(header,n,&out)==-1);
    assert(!memcmp(&out,&before,sizeof(out)));
}
int main(void) {
    fpk_envelope out;
    valid(); assert(!fpk_parse_envelope(header,128,&out));
    assert(out.container_bytes==130 && out.manifest_bytes==1);
    for (unsigned i=0;i<128;i++) { reject(i); }
    reject(129);
    const unsigned reserved[]={16,24,28,112,127};
    for (unsigned i=0;i<sizeof(reserved)/sizeof(*reserved);i++) {
        valid(); header[reserved[i]]=1; reject(128);
    }
    for (unsigned i=0;i<8;i++) { valid(); header[i]^=1; reject(128); }
    valid(); put32(8,2); reject(128);
    valid(); put32(12,127); reject(128);
    const unsigned offsets[]={20,32,40};
    const uint32_t limits[]={FPK_MANIFEST_MAX,FPK_COMPRESSED_MAX,FPK_EXPANDED_MAX};
    for (unsigned i=0;i<3;i++) {
        valid(); put32(offsets[i],0); reject(128);
        put32(offsets[i],limits[i]); assert(!fpk_parse_envelope(header,128,&out));
        put32(offsets[i],limits[i]+1); reject(128);
    }
    valid(); memset(header+32,255,8); reject(128);
    valid(); memset(header+40,255,8); reject(128);
    assert(fpk_parse_envelope(NULL,128,&out)==-1);
    assert(fpk_parse_envelope(header,128,NULL)==-1);
    const char *bad[]={"","-a","a-","a--b","A","../a","a/b","a_b","a.b"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)
        assert(!fpk_identity_valid((const uint8_t *)bad[i],strlen(bad[i])));
    assert(fpk_identity_valid((const uint8_t *)"net-check2",10));
    uint8_t name[33]; memset(name,'a',sizeof(name));
    assert(fpk_identity_valid(name,32)); assert(!fpk_identity_valid(name,33));
    name[1]=0; assert(!fpk_identity_valid(name,32));
    assert(!fpk_identity_valid(NULL,1));
    puts("PKG-1 envelope/identity PASS (structural only)");
}
