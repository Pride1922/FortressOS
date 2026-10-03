/* Actual tools/codecs; host syscall adapters do not claim live kernel behavior. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "common.h"
#include "digest.h"

typedef struct { bool open; const uint8_t *data; size_t size, pos; } fd_t;
static fd_t fds[32];
static uint8_t data[8193];
static char manifest[2048], output[8192], errors[8192];
static size_t out_size, err_size, manifest_len;
static int opens, closes, reads;
static bool read_error, write_error, close_error, open_error, zero_write;
static void copy_text(char *dest, const char *src) { memcpy(dest,src,strlen(src)+1); }
static void reset(void) {
    memset(fds,0,sizeof(fds));
    fds[0]=(fd_t){true,(const uint8_t *)"abc",3,0};
    for (size_t i=0;i<sizeof(data);i++) data[i]=(uint8_t)i;
    out_size=err_size=manifest_len=0; output[0]=errors[0]=0;
    opens=closes=reads=0;
    read_error=write_error=close_error=open_error=zero_write=false;
}
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr==SYS_STAT) {
        const char *p=(const char *)a;
        if (!strcmp(p,"missing")) return SYSCALL_ENOENT;
        ((vfs_stat_t *)b)->type=!strcmp(p,"dir") ? VFS_DIRECTORY : VFS_FILE;
        return 0;
    }
    if (nr==SYS_OPEN) {
        assert(b==(VFS_O_RDONLY|VFS_O_CLOEXEC));
        if (open_error) return SYSCALL_EMFILE;
        const char *p=(const char *)a;
        if (!strcmp(p,"missing")) return SYSCALL_ENOENT;
        for (int i=3;i<32;i++) if (!fds[i].open) {
            const uint8_t *d=data; size_t size=sizeof(data);
            if (!strcmp(p,"abc")) { d=(const uint8_t *)"abc"; size=3; }
            if (!strcmp(p,"manifest")) { d=(const uint8_t *)manifest; size=manifest_len ? manifest_len : strlen(manifest); }
            if (!strcmp(p,"empty")) size=0;
            fds[i]=(fd_t){true,d,size,0}; opens++; return i;
        }
        return SYSCALL_EMFILE;
    }
    if (nr==SYS_CLOSE) {
        assert(a<32 && fds[a].open); fds[a].open=false; closes++;
        return close_error ? SYSCALL_EIO : 0;
    }
    if (nr==SYS_READ) {
        assert(a<32 && fds[a].open && c<=4096); reads++;
        if (read_error && reads>1) return SYSCALL_EINTR;
        fd_t *f=&fds[a]; size_t n=f->size-f->pos;
        if (n>c) n=c;
        if (n>17) n=17;
        memcpy((void *)b,f->data+f->pos,n); f->pos+=n;
        return (long)n;
    }
    if (nr==SYS_WRITE) {
        assert(a==1 || a==2);
        if (a==1 && write_error) return SYSCALL_EPIPE;
        if (a==1 && zero_write) return 0;
        size_t n=c>3 ? 3:c;
        char *p=a==1 ? output:errors;
        size_t *size=a==1 ? &out_size:&err_size;
        assert(*size+n<8192); memcpy(p+*size,(const void *)b,n); *size+=n; p[*size]=0;
        return (long)n;
    }
    assert(!"unexpected syscall"); return -1;
}
static void no_leaks(void) {
    assert(opens==closes);
    for (int i=3;i<32;i++) assert(!fds[i].open);
    assert(fds[0].open);
}
static int run(bool sha, int argc, char **argv) {
    int r=sha ? sha256sum_main(argc,argv):md5sum_main(argc,argv);
    no_leaks(); return r;
}
static void vector(digest_kind_t kind,const char *text,const char *hex) {
    digest_t ctx; uint8_t result[32];
    digest_init(&ctx,kind);
    for (size_t i=0;i<strlen(text);i++) assert(digest_update(&ctx,text+i,1));
    digest_final(&ctx,result);
    for (size_t i=0;i<digest_size(kind);i++) {
        char pair[3]; snprintf(pair,sizeof(pair),"%02x",result[i]);
        assert(!strncmp(pair,hex+i*2,2));
    }
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--stream")) {
        digest_t ctx; uint8_t result[32], buf[4096];
        digest_init(&ctx,DIGEST_SHA256);
        size_t n;
        while ((n=fread(buf,1,sizeof(buf),stdin))) assert(digest_update(&ctx,buf,n));
        assert(!ferror(stdin)); digest_final(&ctx,result);
        for (size_t i=0;i<32;i++) printf("%02x",result[i]);
        puts(""); return 0;
    }
    vector(DIGEST_MD5,"","d41d8cd98f00b204e9800998ecf8427e");
    vector(DIGEST_MD5,"abc","900150983cd24fb0d6963f7d28e17f72");
    vector(DIGEST_SHA256,"","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    vector(DIGEST_SHA256,"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest_t ctx, before;
    digest_init(&ctx,DIGEST_SHA256); ctx.bytes=UINT64_MAX/8;
    before=ctx; assert(!digest_update(&ctx,"a",1)); assert(!memcmp(&ctx,&before,sizeof(ctx)));
    char *plain[]={"sum","abc"};
    reset(); assert(run(true,2,plain)==0);
    assert(!strcmp(output,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n"));
    reset(); assert(run(false,2,plain)==0); assert(!strcmp(output,"900150983cd24fb0d6963f7d28e17f72  abc\n"));
    char *stream[]={"sum"}; reset(); assert(run(true,1,stream)==0); assert(strstr(output,"  -\n"));
    char *check[]={"sum","-c","manifest"};
    /* md5sum -c tests: two spaces, asterisk, wrong hash */
    reset(); copy_text(manifest,"900150983cd24fb0d6963f7d28e17f72  abc\n900150983cd24fb0d6963f7d28e17f72 *abc");
    assert(run(false,3,check)==0); assert(!strcmp(output,"abc: OK\nabc: OK\n"));
    reset(); copy_text(manifest,"900150983cd24fb0d6963f7d28e17f72  abc\n");
    assert(run(false,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"900150983cd24fb0d6963f7d28e17f72 *abc\n");
    assert(run(false,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"900150983cd24fb0d6963f7d28e17f72  abc\n");
    manifest_len=512; memset(manifest+strlen(manifest),0,512-strlen(manifest));
    assert(run(false,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"000150983cd24fb0d6963f7d28e17f72  abc\n900150983cd24fb0d6963f7d28e17f72  abc\n");
    assert(run(false,3,check)==1); assert(!strcmp(output,"abc: FAILED\nabc: OK\n"));

    /* sha256sum -c tests: two spaces, asterisk, wrong hash, trailing padding */
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    assert(run(true,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad *abc\n");
    assert(run(true,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad *abc\n");
    assert(run(true,3,check)==0); assert(!strcmp(output,"abc: OK\nabc: OK\n"));
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    manifest_len=512; memset(manifest+strlen(manifest),0,512-strlen(manifest));
    assert(run(true,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n\n");
    assert(run(true,3,check)==0); assert(!strcmp(output,"abc: OK\n"));
    reset(); copy_text(manifest,"007816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    assert(run(true,3,check)==1); assert(!strcmp(output,"abc: FAILED\n"));
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  missing\n");
    assert(run(true,3,check)==1); assert(!strcmp(output,"missing: FAILED\n"));
    reset(); copy_text(manifest,"za7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    assert(run(true,3,check)==1); assert(out_size==0);
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad abc\n");
    assert(run(true,3,check)==1); assert(out_size==0);
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\tabc\n");
    assert(run(true,3,check)==1); assert(out_size==0);
    reset(); copy_text(manifest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    manifest[20]=0; manifest_len=strlen("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc\n");
    assert(run(true,3,check)==1); assert(out_size==0);

    const char *invalid[]={"","\n","xyz\n","900150983cd24fb0d6963f7d28e17f72  missing\n",
        "900150983cd24fb0d6963f7d28e17f72  dir\n","900150983cd24fb0d6963f7d28e17f72  -\n",
        "900150983cd24fb0d6963f7d28e17f7z  abc\n","900150983cd24fb0d6963f7d28e17f72  a\\b\n",
        "900150983cd24fb0d6963f7d28e17f72 abc\n","900150983cd24fb0d6963f7d28e17f72\tabc\n"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset(); copy_text(manifest,invalid[i]); assert(run(false,3,check)==1);
    }
    reset(); memset(manifest,'a',1000); manifest[1000]=0; assert(run(true,3,check)==1);
    char *binary[]={"sum","binary"}; reset(); assert(run(true,2,binary)==0); assert(reads>400);
    reset(); read_error=true; assert(run(true,2,binary)==1); assert(out_size==0);
    reset(); close_error=true; assert(run(true,2,plain)==1); assert(out_size==0);
    reset(); open_error=true; assert(run(true,2,plain)==1);
    reset(); write_error=true; assert(run(true,2,plain)==1);
    reset(); zero_write=true; assert(run(true,2,plain)==1);
    char *bad_option[]={"sum","-x"}; reset(); assert(run(true,2,bad_option)==2);
    puts("checksum host CLI/failure/ownership tests PASS");
    return 0;
}
