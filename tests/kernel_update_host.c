#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "kernel_update.h"
static uint8_t *disk_bytes,*initial,*prefix;
static kernel_update_work_t work,saved;
static size_t disk_size;
static unsigned event,cut;
static bool allowed(uint64_t lba,unsigned n) {
    uint64_t at=(lba-saved.esp)*512,bytes=(uint64_t)n*512;
    if (lba<saved.esp) return false;
    unsigned inactive=saved.active^1;
    if (at>=saved.kernel[inactive] && at+bytes<=saved.kernel[inactive]+KUPDATE_BANK_BYTES) return true;
    if (at==saved.readiness[inactive] && bytes==512) return true;
    for (unsigned i=0;i<4;i++) if (at==saved.configs[i] && bytes==512) return true;
    return false;
}
static int rd(void *ctx,uint64_t lba,void *b,unsigned n) {
    (void)ctx;assert(n && n<=8 && lba<disk_size/512 && n<=disk_size/512-lba);memcpy(b,disk_bytes+lba*512,n*512);return 0;
}
static int wr(void *ctx,uint64_t lba,const void *b,unsigned n) {
    (void)ctx;assert(allowed(lba,n));event++;
    if (event==cut) {memcpy(disk_bytes+lba*512,b,n*256);return -1;}
    memcpy(disk_bytes+lba*512,b,n*512);return 0;
}
static int flush(void *ctx) {(void)ctx;return ++event==cut ? -1 : 0;}
static void *load(const char *name,size_t *size) {
    FILE *f=fopen(name,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    void *p=malloc((size_t)n);assert(p && fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return p;
}
int main(int argc,char **argv) {
    assert(argc==5);int fd=open(argv[1],O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st) && S_ISREG(st.st_mode));
    disk_size=(size_t)st.st_size;disk_bytes=mmap(NULL,disk_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(disk_bytes!=MAP_FAILED);close(fd);
    prefix=malloc(KUPDATE_PREFIX_BYTES);initial=malloc(KUPDATE_PREFIX_BYTES);assert(prefix && initial);
    size_t size;void *candidate=load(argv[2],&size);
    kernel_update_io_t io={.sectors=disk_size/512,.read=rd,.write=wr,.flush=flush};
    assert(!kernel_update_prepare(&io,prefix,KUPDATE_PREFIX_BYTES,&work,argv[3]));saved=work;
    memcpy(initial,disk_bytes+saved.esp*512,KUPDATE_PREFIX_BYTES);
    uint64_t bad[]={512+16,(disk_size/512-1)*512+16,
        saved.esp*512+saved.fat.fat+saved.fat.kernel_first*4,
        saved.esp*512+saved.configs[1]+15,
        saved.esp*512+saved.readiness[0]+16,
        saved.root*512+1024+104};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        disk_bytes[bad[i]]^=1;event=0;
        assert(kernel_update_prepare(&io,prefix,KUPDATE_PREFIX_BYTES,&work,argv[3])<0);
        assert(!event);disk_bytes[bad[i]]^=1;
    }
    assert(!kernel_update_prepare(&io,prefix,KUPDATE_PREFIX_BYTES,&work,argv[3]));
    assert(kernel_update_prepare(&io,prefix,KUPDATE_PREFIX_BYTES,&work,"0000000000000000000000000000000000000000000000000000000000000000")<0);
    unsigned cuts[]={1,2,1024,2048,2049,2050,2051,2052,2053,2054,2055,2056,2057,2058,2059};
    for (unsigned i=0;i<sizeof(cuts)/sizeof(cuts[0]);i++) {
        memcpy(disk_bytes+saved.esp*512,initial,KUPDATE_PREFIX_BYTES);memcpy(prefix,initial,KUPDATE_PREFIX_BYTES);work=saved;event=0;cut=cuts[i];
        int result=kernel_update_apply(&io,prefix,&work,candidate,size);assert(result<0);
        uint8_t *esp=disk_bytes+saved.esp*512;unsigned active=saved.active;
        assert(!memcmp(esp+saved.kernel[active],initial+saved.kernel[active],KUPDATE_BANK_BYTES));
        assert(!memcmp(esp+saved.readiness[active],initial+saved.readiness[active],512));
        assert(!memcmp(esp+saved.bootstrap,initial+saved.bootstrap,saved.bootstrap_size));
        for (unsigned m=0;m<4;m++) if (esp[saved.configs[m]+15]!=(active ? '3' : '1')) {
            assert(!memcmp(esp+saved.kernel[active^1],candidate,size));
            for (size_t k=size;k<KUPDATE_BANK_BYTES;k++) assert(!esp[saved.kernel[active^1]+k]);
            assert(!memcmp(esp+saved.readiness[active^1],work.ready,512));
        }
    }
    memcpy(disk_bytes+saved.esp*512,initial,KUPDATE_PREFIX_BYTES);memcpy(prefix,initial,KUPDATE_PREFIX_BYTES);work=saved;event=0;cut=0;
    assert(!kernel_update_apply(&io,prefix,&work,candidate,size));
    assert(kernel_update_apply(&io,prefix,&work,candidate,size)<0); /* no implicit retry */
    FILE *out=fopen(argv[4],"wb");assert(out);assert(fwrite(disk_bytes+saved.esp*512,1,INSTALL_FAT_BYTES,out)==INSTALL_FAT_BYTES);fclose(out);
    assert(kernel_update_prepare(&io,prefix,KUPDATE_PREFIX_BYTES,&work,argv[3])<0); /* exact predecessor binding */
    printf("PASS 6 malformed GPT/FAT/menu/readiness/root cases, 15 interrupted write/flush boundaries, write-range oracle, old-kernel preservation, exact predecessor and repeat refusal\n");
    free(candidate);free(prefix);free(initial);munmap(disk_bytes,disk_size);return 0;
}
