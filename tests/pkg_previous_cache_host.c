/* Actual previous-source frontend, mocked files only. The kernel's independent
 * predecessor hash check is exercised by the factory signed-upgrade suite. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/pkg/repository.h"
#include "user/tools/common.h"
#include "crypto.h"
static uint8_t record[8192];static size_t position;static bool rec_open,source_open,available=true,foreign;
static unsigned config_reads,source_opens;
static const char expected[]="/var/lib/pkg/sources/11111111/11111111111111111111111111111111111111111111111111111111.fpk";
long tool_syscall(long op,uintptr_t a,uintptr_t b,uintptr_t c){
    if(op==SYS_OPEN){
        assert(b==(VFS_O_RDONLY|VFS_O_NOFOLLOW)&&!c);const char *path=(void *)a;
        if(!strcmp(path,"/var/lib/pkg/install.v2")){assert(!rec_open);rec_open=true;position=0;return 3;}
        if(!strcmp(path,expected)){source_opens++;if(!available)return SYSCALL_ENOENT;assert(!source_open);source_open=true;return 4;}
        if(!strcmp(path,"/etc/pkg/repository.conf")){config_reads++;return SYSCALL_ENOENT;}
        assert(!strncmp(path,"/var/lib/pkg/install-",21));return SYSCALL_ENOENT;
    }
    if(op==SYS_CLOSE){if(a==3){assert(rec_open);rec_open=false;}else{assert(a==4&&source_open);source_open=false;}return 0;}
    if(op==SYS_FSTAT){assert(a==3||a==4);*(stat_ext_v1_t *)b=(stat_ext_v1_t){.size=sizeof(stat_ext_v1_t),.version=1,.type=VFS_FILE,.mode=0600,.uid=a==4&&foreign?1000:0,.file_size=a==3?8192:256};return 0;}
    if(op==SYS_READ){assert(a==3&&rec_open);size_t take=sizeof record-position;if(take>c)take=c;if(take>71)take=71;memcpy((void *)b,record+position,take);position+=take;return (long)take;}
    if(op==SYS_WRITE){assert(a==1||a==2);return (long)c;}
    assert(!"unexpected network, mutation or admission call");return -1;
}
int main(void){
    memcpy(record,"FORT-PKG-EXEC1",14);record[24]=2;record[40]=1;record[68]=1;
    memcpy(record+320,"fortress-notepad",16);memcpy(record+352,"notepad",7);memset(record+112,0x11,32);
    assert(!crypto_sha256(record,8160,record+8160));uint64_t bytes=0;
    int fd=pkg_previous_source("fortress-notepad",&bytes);assert(fd==4&&bytes==256&&!config_reads&&source_opens==1&&!rec_open&&source_open);
    assert(!tool_syscall(SYS_CLOSE,fd,0,0));
    available=false;assert(pkg_previous_source("fortress-notepad",&bytes)<0&&config_reads==1&&!source_open&&!rec_open);
    available=true;foreign=true;assert(pkg_previous_source("fortress-notepad",&bytes)<0&&config_reads==2&&!source_open&&!rec_open);
    assert(pkg_previous_source("not-installed",&bytes)<0&&config_reads==2&&!source_open&&!rec_open);
    puts("PASS actual previous-source lookup: bounded split-hash cache path, retained fd, no network for factory source, absent/foreign source fallback and cleanup");
}
