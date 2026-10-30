#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user/pkg/verify.h"
static fpk_verify_context context;
static long reader(void *arg,void *bytes,size_t n) {
    if(n>613)n=613;
    size_t got=fread(bytes,1,n,arg);
    return ferror((FILE *)arg)?-1:(long)got;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)nr;(void)a;(void)b;(void)c;assert(!"unexpected host syscall");return -1;
}
int main(int argc,char **argv) {
    assert(argc==3 && strlen(argv[2])==64);
    uint8_t hash[32];
    for(unsigned i=0;i<32;i++){char pair[3]={argv[2][i*2],argv[2][i*2+1],0};hash[i]=(uint8_t)strtoul(pair,NULL,16);}
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>0);rewind(f);
    int result=fpk_verify(&context,reader,f,(uint64_t)length,hash);fclose(f);
    if(result || !context.verified)return 1;
    printf("Verified package %.*s command %.*s version %.*s build %llu files %u\n",
           context.manifest.package.length, context.manifest.package.bytes,
           context.manifest.command.length, context.manifest.command.bytes,
           context.manifest.version.length, context.manifest.version.bytes,
           (unsigned long long)context.manifest.build, context.manifest.file_count);
    return 0;
}
