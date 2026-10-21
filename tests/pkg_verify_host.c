#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/verify.h"
static fpk_verify_context context;
static uint8_t artifact[8*1024*1024+16384+128], original[4096], original_hash[32];
typedef struct { size_t pos, size, fragment, error_at; int overreport; } reader;
static long read_bytes(void *arg,void *out,size_t n) {
    reader *r=arg;
    if(r->overreport) return (long)n+1;
    if(r->pos>=r->error_at) return -1;
    if(n>r->size-r->pos) n=r->size-r->pos;
    size_t fragment=r->fragment ? r->fragment : 1+(r->pos*37u)%251u;
    if(n>fragment) n=fragment;
    if(n>r->error_at-r->pos) n=r->error_at-r->pos;
    memcpy(out,artifact+r->pos,n); r->pos+=n; return (long)n;
}
/* Reaching any CLI syscall is a test failure: verifier must use callbacks. */
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)nr; (void)a; (void)b; (void)c; assert(!"unexpected syscall"); return -1;
}
int main(void) {
    FILE *f=fopen("build/pkg-format/artifact-vectors.bin","rb"); assert(f);
    unsigned count=0; size_t original_size=0;
    for(;;) {
        uint8_t h[37]; size_t got=fread(h,1,sizeof(h),f);
        if(!got) { assert(!ferror(f)); break; }
        assert(got==sizeof(h));
        uint32_t n=(uint32_t)h[1] | (uint32_t)h[2]<<8 | (uint32_t)h[3]<<16 | (uint32_t)h[4]<<24;
        assert(n<=sizeof(artifact) && fread(artifact,1,n,f)==n);
        for(unsigned mode=0;mode<4;mode++) {
            reader r={0,n,mode==0?1:(mode==1?17:(mode==2?4096:0)),SIZE_MAX,0};
            int result=fpk_verify(&context,read_bytes,&r,n,h+5);
            if((result==0)!=h[0] || context.verified!=h[0]) {
                fprintf(stderr,"artifact vector %u fragment %zu: %d expected %u\n",count,r.fragment,result,h[0]); return 1;
            }
        }
        if(!count) { assert(n<sizeof(original)); memcpy(original,artifact,n); memcpy(original_hash,h+5,32); original_size=n; }
        count++;
    }
    assert(!fclose(f)); memcpy(artifact,original,original_size);
    for(size_t i=0;i<=original_size;i++) {
        reader r={0,i,7,SIZE_MAX,0};
        if(i<original_size) assert(fpk_verify(&context,read_bytes,&r,original_size,original_hash)<0 && !context.verified);
        r=(reader){0,original_size,7,i,0};
        assert(fpk_verify(&context,read_bytes,&r,original_size,original_hash)<0 && !context.verified);
    }
    uint8_t bad_hash[32]={0}; reader r={0,original_size,13,SIZE_MAX,0};
    assert(fpk_verify(&context,read_bytes,&r,original_size,bad_hash)<0 && !context.verified);
    r=(reader){0,original_size,13,SIZE_MAX,1};
    assert(fpk_verify(&context,read_bytes,&r,original_size,original_hash)<0 && !context.verified);
    assert(fpk_verify(&context,NULL,NULL,0,NULL)<0 && !context.verified);
    printf("PKG artifact PASS: %u vectors x4 fragmentation; %zu truncations + %zu read failures; context %zu bytes\n",count,original_size,original_size+1,sizeof(context));
}
