/* Actual CLI + verifier/crypto; mocked syscall transport, no guest claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/cli.h"
#include "user/tools/common.h"
static uint8_t artifact[4096],catalog[4096];
static size_t artifact_size,catalog_size,offsets[2];
static int opened[2],opens,fail_read,fail_close,fail_stat,bad_clock,fail_output;
static char output[2048]; static size_t output_size;
static uint64_t le(const uint8_t *p,unsigned n) {
    uint64_t v=0;for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i);return v;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if(nr==SYS_OPEN) {
        assert(b==(VFS_O_RDONLY|VFS_O_NOFOLLOW) && !c);
        int i=tool_equal((const char *)a,"artifact")?0:tool_equal((const char *)a,"catalog")?1:-1;
        if(i<0) return SYSCALL_ENOENT;
        assert(!opened[i]);opened[i]=1;offsets[i]=0;opens++;return i+3;
    }
    if(nr==SYS_CLOCK_GETTIME) {
        assert(a==CLOCK_REALTIME);timespec_t *t=(void *)b;
        *t=(timespec_t){bad_clock?0:1792540800,0};return 0;
    }
    if(nr==SYS_WRITE) {
        assert(a==1 || a==2); /* All filesystem writes are forbidden. */
        if(fail_output) return SYSCALL_EIO;
        size_t n=c>7?7:c;assert(output_size+n<sizeof(output));
        memcpy(output+output_size,(void *)b,n);output_size+=n;output[output_size]=0;return (long)n;
    }
    assert(a>=3 && a<=4 && opened[a-3]);unsigned i=(unsigned)a-3;
    if(nr==SYS_CLOSE) {opened[i]=0;opens--;return fail_close?SYSCALL_EIO:0;}
    if(nr==SYS_FSTAT) {
        stat_ext_v1_t *s=(void *)b;
        *s=(stat_ext_v1_t){sizeof(*s),1,i?catalog_size:artifact_size,fail_stat?VFS_STREAM:VFS_FILE,0644,0,0,0,0};return 0;
    }
    assert(nr==SYS_READ);
    if(fail_read) return SYSCALL_EIO;
    size_t size=i?catalog_size:artifact_size,n=c;
    if(n>size-offsets[i]) n=size-offsets[i];
    if(n>5) n=5;
    memcpy((void *)b,(i?catalog:artifact)+offsets[i],n);offsets[i]+=n;return (long)n;
}
static int invoke(int argc,char **argv,const fpk_catalog_policy *policy) {
    output_size=0;output[0]=0;
    int r=pkg_cli(argc,argv,policy);assert(!opens);return r;
}
int main(void) {
    FILE *f=fopen("build/pkg-format/fixture.fpk","rb");assert(f);
    artifact_size=fread(artifact,1,sizeof(artifact),f);assert(!fclose(f));
    f=fopen("build/pkg-format/catalog-vectors.bin","rb");assert(f);
    uint8_t h[6],rules[336];assert(fread(h,1,6,f)==6 && fread(rules,1,336,f)==336);
    catalog_size=(size_t)le(h+2,4);assert(catalog_size<=sizeof(catalog) && fread(catalog,1,catalog_size,f)==catalog_size);assert(!fclose(f));
    fpk_catalog_policy policy={0};memcpy(policy.repository,rules,16);policy.root_version=le(rules+16,8);
    policy.revision_floor=le(rules+24,8);policy.now=0; /* CLI must use runtime clock. */
    memcpy(policy.floor_sha256,rules+40,32);policy.threshold=(unsigned)le(rules+72,4);
    policy.key_count=(unsigned)le(rules+76,4);memcpy(policy.keys,rules+80,256);
    char *help[]={"pkg","--help"};assert(!invoke(2,help,NULL));
    char *inspect[]={"pkg","inspect","artifact","490","db2ac351543a6f7afc6203c3d72b3478985b0d418c7677c754acd59c501a750b"};
    assert(!invoke(5,inspect,NULL) && strstr(output,"integrity OK; signatures not checked") && strstr(output,"package demo build 1"));
    char *verify[]={"pkg","verify","catalog","demo","artifact"};
    assert(invoke(5,verify,NULL) && strstr(output,"no provisioned"));
    assert(!invoke(5,verify,&policy) && strstr(output,"catalog policy verified"));
    assert(policy.now==0);
    catalog[catalog_size-1]^=1;assert(invoke(5,verify,&policy));catalog[catalog_size-1]^=1;
    artifact[artifact_size-1]^=1;assert(invoke(5,inspect,NULL));artifact[artifact_size-1]^=1;
    fail_read=1;assert(invoke(5,inspect,NULL));assert(invoke(5,verify,&policy));fail_read=0;
    fail_close=1;assert(invoke(5,inspect,NULL));fail_close=0;
    fail_stat=1;assert(invoke(5,inspect,NULL));fail_stat=0;
    bad_clock=1;assert(invoke(5,verify,&policy));bad_clock=0;
    inspect[3]="18446744073709551616";assert(invoke(5,inspect,NULL));inspect[3]="490";
    inspect[4]="a";assert(invoke(5,inspect,NULL));
    char *mutate[]={"pkg","install","catalog","demo","artifact"};assert(invoke(5,mutate,NULL));
    assert(invoke(1,help,NULL));
    fail_output=1;assert(invoke(2,help,NULL));fail_output=0;
    puts("PKG CLI PASS: readonly opens, fd cleanup, integrity, supplied trust, clock and failures (mock syscalls)");
}
