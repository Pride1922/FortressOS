#include "common.h"
static uint8_t bytes[65536];
static mount_info_t m;
#define CHECK(x) do {if(!(x)) {say("HEALTH FAIL " #x "\n");return 1;}}while(0)
static void say(const char *s) {tool_write("health-probe",s,tool_length(s));}
static long open_file(const char *p) {return tool_syscall(SYS_OPEN,(uintptr_t)p,VFS_O_CREAT|VFS_O_RDWR,0);}
static long close_file(long fd) {return tool_syscall(SYS_CLOSE,fd,0,0);}
static long unlink_file(const char *p) {return tool_syscall(SYS_UNLINK,(uintptr_t)p,0,0);}
static long metrics(void) {return tool_syscall(SYS_MOUNTINFO,1,(uintptr_t)&m,0);}
static void name(char *p,unsigned n) {
    const char *prefix="/mnt/inode-";unsigned i=0;while(prefix[i]) {p[i]=prefix[i];i++;}tool_format_u64(p+i,n);
}
int health_probe_main(int argc,char **argv) {
    (void)argc;(void)argv;CHECK(metrics()==1 && m.fs_type==VFS_FS_EXT4 && m.flags==MOUNT_FLAGS_RW);
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]='H';
    long f=open_file("/mnt/health-short");CHECK(f>=0);
    long count=tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,sizeof(bytes));CHECK(count==SYSCALL_EINVAL);
    count=tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,MAX_SYSCALL_WRITE_LEN);CHECK(count==MAX_SYSCALL_WRITE_LEN);
    uint64_t done=(uint64_t)count;
    while(done<sizeof(bytes)) {count=tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,MAX_SYSCALL_WRITE_LEN);CHECK(count>0);done+=(uint64_t)count;}
    CHECK(!close_file(f));
    long source=open_file("/mnt/health-source"),old=open_file("/mnt/health-target");CHECK(source>=0 && old>=0);
    CHECK(tool_syscall(SYS_WRITE,source,(uintptr_t)"NEW",3)==3 && !close_file(source));
    CHECK(tool_syscall(SYS_WRITE,old,(uintptr_t)"OLD",3)==3);
    CHECK(!tool_syscall(SYS_RENAME_REPLACE,(uintptr_t)"/mnt/health-source",(uintptr_t)"/mnt/health-target",0));
    CHECK(metrics()==1 && (m.flags & MOUNT_FLAGS_CLEANUP_PENDING));
    CHECK(!tool_syscall(SYS_SYNC,0,0,0));CHECK(metrics()==1 && (m.flags & MOUNT_FLAGS_CLEANUP_PENDING));
    CHECK(!close_file(old));CHECK(metrics()==1 && !(m.flags & MOUNT_FLAGS_CLEANUP_PENDING));
    /* One pre-I/O cleanup failure, compiled only into this fixture kernel.
     * Close itself succeeds; future mutation must expose recovery-required. */
    f=open_file("/mnt/health-recovery");CHECK(f>=0);
    CHECK(tool_syscall(SYS_WRITE,f,(uintptr_t)"OLD",3)==3);
    CHECK(!unlink_file("/mnt/health-recovery") && metrics()==1);
    CHECK((m.flags & MOUNT_FLAGS_CLEANUP_PENDING) && !(m.flags & MOUNT_FLAGS_RECOVERY_REQUIRED));
    CHECK(!close_file(f) && metrics()==1);
    CHECK((m.flags & (MOUNT_FLAGS_RECOVERY_REQUIRED|MOUNT_FLAGS_CLEANUP_ERROR))==
          (MOUNT_FLAGS_RECOVERY_REQUIRED|MOUNT_FLAGS_CLEANUP_ERROR));
    CHECK(!(m.flags & (MOUNT_FLAGS_TAINTED|MOUNT_FLAGS_NAMESPACE_UNCERTAIN)));
    CHECK(open_file("/mnt/health-blocked")==SYSCALL_EAGAIN);
    CHECK(metrics()==1 && (m.flags & MOUNT_FLAGS_RECOVERY_REQUIRED));
    /* Exactly one drain; the saved error acknowledges the earlier close.
     * Requery distinguishes that error from a currently tainted mount. */
    CHECK(tool_syscall(SYS_SYNC,0,0,0)==SYSCALL_ENOMEM);
    CHECK(metrics()==1 && !(m.flags & (MOUNT_FLAGS_RECOVERY_REQUIRED|MOUNT_FLAGS_CLEANUP_ERROR|MOUNT_FLAGS_CLEANUP_PENDING)));
    f=open_file("/mnt/health-blocked");CHECK(f>=0 && !close_file(f));
    CHECK(!unlink_file("/mnt/health-blocked"));
    uint64_t free_inodes=m.free_inodes;unsigned made=0;
    for(;made<512;made++) {
        char path[64];name(path,made);f=open_file(path);
        if(f<0) {CHECK(f==SYSCALL_ENOSPC);break;}CHECK(!close_file(f));
    }
    CHECK(made<512 && metrics()==1 && !m.free_inodes && m.flags==MOUNT_FLAGS_RW);
    for(unsigned i=0;i<made;i++) {char path[64];name(path,i);CHECK(!unlink_file(path));}
    CHECK(!tool_syscall(SYS_SYNC,0,0,0) && metrics()==1 && m.free_inodes==free_inodes);
    f=open_file("/mnt/fill");long tail=open_file("/mnt/fill-tail");CHECK(f>=0 && tail>=0);
    count=1;while(count>0)count=tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,MAX_SYSCALL_WRITE_LEN);CHECK(count==SYSCALL_ENOSPC);
    count=1;while(count>0)count=tool_syscall(SYS_WRITE,tail,(uintptr_t)bytes,m.block_size);CHECK(count==SYSCALL_ENOSPC);
    CHECK(metrics()==1 && !m.free_blocks && m.flags==MOUNT_FLAGS_RW);
    CHECK(!close_file(f) && !close_file(tail));CHECK(!unlink_file("/mnt/fill") && !unlink_file("/mnt/fill-tail"));
    CHECK(!tool_syscall(SYS_SYNC,0,0,0) && metrics()==1 && m.flags==MOUNT_FLAGS_RW && m.free_inodes==free_inodes);
    say("HEALTH Ring3 PASS\n");return 0;
}
