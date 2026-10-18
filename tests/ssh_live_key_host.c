/* Actual live provisioner + readiness loader; filesystem/syscalls are adapters. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/tools/ssh_keygen.c"
static long mock7(long,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
#define SSHD_SYSCALL mock7
#define SSHD_READINESS_IO_TEST
#include "user/sshd/sshd_readiness.c"

static bool directory,stage,published,unprivileged,bad_metadata,random_fail,write_fail,rename_fail;
static char stage_bytes[1024],final_bytes[1024],output[4096];
static size_t stage_len,final_len,pos[8],output_len;
static unsigned mask=022,random_calls,legacy_opens;
static long installed=SYSCALL_EOPNOTSUPP;
static uint64_t rng=123;
static void reset(void) {
    directory=stage=published=unprivileged=bad_metadata=random_fail=write_fail=rename_fail=false;
    stage_len=final_len=output_len=0;mask=022;installed=SYSCALL_EOPNOTSUPP;
    memset(pos,0,sizeof(pos));memset(stage_bytes,0,sizeof(stage_bytes));memset(final_bytes,0,sizeof(final_bytes));
    output[0]=0;random_calls=legacy_opens=0;
}
int64_t crypto_test_getrandom(void *out,size_t n,uint64_t flags) {
    assert(!flags);random_calls++;
    if (random_fail) return -1;
    for (size_t i=0;i<n;i++) {rng=rng*6364136223846793005ULL+1;((uint8_t *)out)[i]=(uint8_t)(rng>>32);}
    return (int64_t)n;
}
void crypto_test_wiped(const void *out,size_t n) {
    const uint8_t *p=out;while (n--) assert(*p++==0);
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    switch (nr) {
    case SYS_GETRESUID: *(uint32_t *)a=0;*(uint32_t *)b=unprivileged ? 1000 : 0;*(uint32_t *)c=0;return 0;
    case SYS_CAPGET: return unprivileged ? 0 : CAP_SYS_ADMIN;
    case SYS_HOSTKEY: assert(a==HOSTKEY_READ);return installed;
    case SYS_UMASK: {unsigned old=mask;mask=a;return old;}
    case SYS_MKDIR:
        assert(!strcmp((const char *)a,SSH_LIVE_DIR) && b==0700 && mask==077);
        if (directory) return SYSCALL_EEXIST;
        directory=true;return 0;
    case SYS_OPEN: {
        const char *path=(const char *)a;int fd;
        if (!strcmp(path,SSH_LIVE_DIR)) {if (!directory) return SYSCALL_ENOENT;fd=3;}
        else if (!strcmp(path,SSH_LIVE_STAGE)) {
            if (b & VFS_O_CREAT) {assert(directory && !stage && mask==077);stage=true;}
            if (!stage) return SYSCALL_ENOENT;
            fd=4;
        } else if (!strcmp(path,SSH_LIVE_KEY)) {if (!published) return SYSCALL_ENOENT;fd=5;}
        else {legacy_opens++;return SYSCALL_ENOENT;}
        pos[fd]=0;return fd;
    }
    case SYS_FSTAT: {
        assert(a>=3 && a<=5);
        stat_ext_v1_t *st=(void *)b;
        *st=(stat_ext_v1_t){.size=sizeof(*st),.version=1,.type=a==3 ? VFS_DIRECTORY : VFS_FILE,
            .mode=a==3 ? VFS_S_IFDIR|0700 : VFS_S_IFREG|0600,
            .uid=bad_metadata ? 1000 : 0,.file_size=a==4 ? stage_len : a==5 ? final_len : 0};
        return 0;
    }
    case SYS_WRITE:
        if (a==1 || a==2) {
            assert(output_len+c<sizeof(output));memcpy(output+output_len,(void *)b,c);output_len+=c;output[output_len]=0;return (long)c;
        }
        assert(a==4 && stage && !published);
        if (write_fail && stage_len) return SYSCALL_EIO;
        if (c>7) c=7;
        assert(stage_len+c<sizeof(stage_bytes));memcpy(stage_bytes+stage_len,(void *)b,c);stage_len+=c;return (long)c;
    case SYS_READ: {
        assert(a==4 || a==5);
        size_t len=a==4 ? stage_len : final_len;
        const char *bytes=a==4 ? stage_bytes : final_bytes;
        size_t n=len-pos[a];if (n>c) n=c;if (n>11) n=11;
        memcpy((void *)b,bytes+pos[a],n);pos[a]+=n;return (long)n;
    }
    case SYS_CLOSE: assert(a>=3 && a<=5);return 0;
    case SYS_RENAME:
        assert(!strcmp((const char *)a,SSH_LIVE_STAGE) && !strcmp((const char *)b,SSH_LIVE_KEY));
        assert(stage && !published);
        if (rename_fail) return SYSCALL_EIO;
        memcpy(final_bytes,stage_bytes,stage_len);final_len=stage_len;published=true;stage=false;return 0;
    default: assert(!"unexpected syscall");return -1;
    }
}
static long mock7(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    (void)d;(void)e;(void)f;return tool_syscall(nr,a,b,c);
}
static int invoke(const char *option) {
    char *argv[]={"ssh-keygen","--live",(char *)option,NULL};output_len=0;output[0]=0;
    int rc=ssh_keygen_main(option ? 3 : 2,argv);
    const unsigned char *buffers[]={secret,expected,(const unsigned char *)container,(const unsigned char *)verify};
    const size_t sizes[]={sizeof(secret),sizeof(expected),sizeof(container),sizeof(verify)};
    for (unsigned i=0;i<4;i++) for (size_t j=0;j<sizes[i];j++) assert(!buffers[i][j]);
    assert(!strstr(output,"PRIVATE KEY"));return rc;
}
int main(void) {
    reset();assert(invoke(NULL)==0 && published && !stage && mask==022);
    char first[64];assert(output_len<sizeof(first));memcpy(first,output,output_len+1);unsigned calls=random_calls;
    assert(invoke(NULL)==0 && random_calls==calls && !strcmp(first,output));
    assert(invoke("--public")==0 && !strncmp(output,"ssh-ed25519 ",12));
    assert(invoke("--fingerprint")==0 && !strcmp(first,output));
    uint8_t decoded[64],public_key[32];
    assert(default_load_host_key(decoded,public_key)==0);
    assert(!memcmp(decoded+32,public_key,32));
    installed=SYSCALL_EIO;assert(default_load_host_key(decoded,public_key)!=0);
    assert(invoke(NULL)!=0);installed=SYSCALL_ENOENT;
    assert(default_load_host_key(decoded,public_key)!=0 && invoke(NULL)!=0);
    installed=SYSCALL_EOPNOTSUPP;bad_metadata=true;
    assert(default_load_host_key(decoded,public_key)!=0 && invoke(NULL)!=0);
    reset();unprivileged=true;assert(invoke(NULL)!=0 && !directory && !random_calls);
    reset();random_fail=true;assert(invoke(NULL)!=0 && !directory && !published);
    reset();write_fail=true;assert(invoke(NULL)!=0 && directory && stage && !published && mask==022);
    calls=random_calls;assert(invoke(NULL)!=0 && random_calls==calls);
    assert(default_load_host_key(decoded,public_key)!=0 && !legacy_opens);
    reset();rename_fail=true;assert(invoke(NULL)!=0 && stage && !published && mask==022);
    reset();assert(invoke(NULL)==0 && strcmp(first,output));
    final_bytes[20]^=1;assert(invoke(NULL)!=0);
    memset(decoded,0,sizeof(decoded));memset(final_bytes,0,sizeof(final_bytes));memset(stage_bytes,0,sizeof(stage_bytes));
    puts("PASS live RAM identity: reuse, independent boots, root/metadata gates, short IO, entropy failure, incomplete publication, installed fail-closed and wipes");
    return 0;
}
