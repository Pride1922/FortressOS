#include "common.h"
static char crossing[8192] __attribute__((aligned(4096)));
static char guard_tail[4096] __attribute__((section(".guardtail"),aligned(4096)));
static char overlong[256];
static const char source[]="/mnt/replace-a/source",target[]="/mnt/replace-b/target";
#define CHECK(x) do {if(!(x)) {say("REPLACE FAIL " #x "\n");return 1;}}while(0)
static void say(const char *s) {tool_write("replace-probe",s,tool_length(s));}
static long replace(uintptr_t a,uintptr_t b,uint64_t flags) {return tool_syscall(SYS_RENAME_REPLACE,a,b,flags);}
static int seed(const char *p,const char *bytes) {
    long f=tool_syscall(SYS_OPEN,(uintptr_t)p,VFS_O_CREAT|VFS_O_RDWR,0);CHECK(f>=0);
    CHECK(tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,3)==3);CHECK(!tool_syscall(SYS_CLOSE,f,0,0));return 0;
}
int replace_probe_main(int argc,char **argv) {
    (void)argc;(void)argv;
    CHECK(replace(0,0,1)==SYSCALL_EINVAL);
    CHECK(replace(0,(uintptr_t)target,0)==SYSCALL_EFAULT);
    CHECK(replace((uintptr_t)source,0,0)==SYSCALL_EFAULT);
    CHECK(replace(0xffffffff80000000ULL,(uintptr_t)target,0)==SYSCALL_EFAULT);
    CHECK(replace(UINTPTR_MAX-4,(uintptr_t)target,0)==SYSCALL_EFAULT);
    CHECK(replace(0x70000000,(uintptr_t)target,0)==SYSCALL_EFAULT);
    for(unsigned i=0;i<sizeof(overlong);i++)overlong[i]='A';
    CHECK(replace((uintptr_t)overlong,(uintptr_t)target,0)==SYSCALL_EINVAL);
    CHECK(replace((uintptr_t)source,(uintptr_t)overlong,0)==SYSCALL_EINVAL);
    guard_tail[4094]='A';guard_tail[4095]='A';
    CHECK(replace((uintptr_t)guard_tail+4094,(uintptr_t)target,0)==SYSCALL_EFAULT);
    CHECK(replace((uintptr_t)source,(uintptr_t)guard_tail+4094,0)==SYSCALL_EFAULT);
    CHECK(!tool_syscall(SYS_MKDIR,(uintptr_t)"/mnt/replace-a",0755,0));
    CHECK(!tool_syscall(SYS_MKDIR,(uintptr_t)"/mnt/replace-b",0755,0));
    CHECK(!seed(source,"NEW") && !seed(target,"OLD"));
    CHECK(tool_syscall(SYS_RENAME,(uintptr_t)source,(uintptr_t)target,0)==SYSCALL_EEXIST);
    CHECK(!replace((uintptr_t)source,(uintptr_t)source,0));
    CHECK(replace((uintptr_t)source,(uintptr_t)"/mnt/absent",0)==SYSCALL_ENOENT);
    CHECK(replace((uintptr_t)source,(uintptr_t)"/mnt/replace-a",0)==SYSCALL_EOPNOTSUPP);
    long src=tool_syscall(SYS_OPEN,(uintptr_t)source,VFS_O_RDONLY,0);CHECK(src>=0);
    CHECK(replace((uintptr_t)source,(uintptr_t)target,0)==SYSCALL_EAGAIN);
    CHECK(!tool_syscall(SYS_CLOSE,src,0,0));
    long old=tool_syscall(SYS_OPEN,(uintptr_t)target,VFS_O_RDONLY,0);CHECK(old>=0);
    long dup=tool_syscall(SYS_DUP,old,0,0);CHECK(dup>=0);
    stat_ext_v1_t before,after;CHECK(!tool_syscall(SYS_FSTAT,old,(uintptr_t)&before,0));
    size_t len=tool_length(source);for(size_t i=0;i<=len;i++)crossing[4094+i]=source[i];
    CHECK(!replace((uintptr_t)crossing+4094,(uintptr_t)target,0));
    CHECK(!tool_syscall(SYS_FSTAT,old,(uintptr_t)&after,0));
    for(size_t i=0;i<sizeof(before);i++)CHECK(((uint8_t *)&before)[i]==((uint8_t *)&after)[i]);
    char bytes[4]={0};CHECK(tool_syscall(SYS_READ,old,(uintptr_t)bytes,3)==3 && tool_equal(bytes,"OLD"));
    CHECK(!tool_syscall(SYS_READ,dup,(uintptr_t)bytes,1));
    long current=tool_syscall(SYS_OPEN,(uintptr_t)target,VFS_O_RDONLY,0);CHECK(current>=0);
    CHECK(tool_syscall(SYS_READ,current,(uintptr_t)bytes,3)==3 && tool_equal(bytes,"NEW"));
    CHECK(tool_syscall(SYS_OPEN,(uintptr_t)source,VFS_O_RDONLY,0)==SYSCALL_ENOENT);
    CHECK(!tool_syscall(SYS_CLOSE,current,0,0));
    CHECK(!tool_syscall(SYS_CLOSE,old,0,0) && !tool_syscall(SYS_CLOSE,dup,0,0));
    CHECK(!tool_syscall(SYS_SYNC,0,0,0));
    say("REPLACE Ring3 PASS\n");return 0;
}
