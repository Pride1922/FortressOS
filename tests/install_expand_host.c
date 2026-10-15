#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "install_payload.h"
static int fd;static unsigned writes,fail_write,flushes;
static int rd(void *ctx,uint64_t l,uint32_t n,void *p) {
    (void)ctx;assert(l>=INSTALL_ROOT_LBA);return pread(fd,p,n*512,(l-INSTALL_ROOT_LBA)*512)==n*512 ? 0 : -5;
}
static int wr(void *ctx,uint64_t l,uint32_t n,const void *p) {
    (void)ctx;assert(l>=INSTALL_ROOT_LBA);writes++;if (writes==fail_write) return -5;
    return pwrite(fd,p,n*512,(l-INSTALL_ROOT_LBA)*512)==n*512 ? 0 : -5;
}
static int fl(void *ctx) {(void)ctx;flushes++;return fsync(fd) ? -5 : 0;}
int main(int argc,char **argv) {
    assert(argc==3);uint64_t bytes=strtoull(argv[2],NULL,10);assert(bytes>=INSTALL_ROOT_BYTES);
    fd=open(argv[1],O_RDWR|O_NOFOLLOW);struct stat st;assert(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode));
    assert(!ftruncate(fd,(off_t)bytes));uint64_t sectors=bytes/512;
    install_target_t target={.sector_size=512,.sectors=sectors+INSTALL_ROOT_LBA+33,.read=rd,.write=wr,.flush=fl};
    install_expand_work_t *work=calloc(1,sizeof(*work));assert(work);
    assert(install_root_expand(&target,(uint64_t)INSTALL_ROOT_MAX_BLOCKS*8+8,work)==-22 && !writes);
    target.sectors=(uint64_t)INSTALL_ROOT_MAX_BLOCKS*8+INSTALL_ROOT_LBA+41;
    assert(install_root_expand(&target,(uint64_t)INSTALL_ROOT_MAX_BLOCKS*8+8,work)==-27 && !writes);
    target.sectors=sectors+INSTALL_ROOT_LBA+33;
    fail_write=1;assert(install_root_expand(&target,sectors,work)==-5 && writes==1 && !flushes);
    writes=fail_write=0;assert(!install_root_expand(&target,sectors,work));assert(writes && flushes==2);
    free(work);close(fd);puts("PASS actual exclusive expansion, geometry bounds, write failure without retry and readback");
}
