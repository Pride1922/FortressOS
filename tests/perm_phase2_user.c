#include "syscall_abi.h"
#include "vfs.h"
#include "power.h"
static long call(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    register uintptr_t r10 __asm__("r10")=d;
    __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");return n;
}
static void text(const char *s) {size_t n=0;while(s[n]) n++;call(SYS_WRITE,2,(uintptr_t)s,n,0);}
static void check(bool ok,unsigned line) {
    if (ok) return;
    char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
    text("PERM PHASE2 FAIL ");call(SYS_WRITE,2,(uintptr_t)n,4,0);call(SYS_EXIT,1,0,0,0);__builtin_trap();
}
#define require(x) check((x),__LINE__)
static stat_ext_v1_t stat;
static void metadata(const char *p,uint32_t mode,uint32_t uid,uint32_t gid) {
    require(!call(SYS_STAT_EXT,(uintptr_t)p,(uintptr_t)&stat,sizeof(stat),1));
    require(stat.mode==mode && stat.uid==uid && stat.gid==gid && !stat.reserved);
}
static void create(const char *p) {
    long fd=call(SYS_OPEN,(uintptr_t)p,VFS_O_CREAT|VFS_O_RDWR,0,0);require(fd>=0);
    require(call(SYS_WRITE,fd,(uintptr_t)"abc",3,0)==3);require(!call(SYS_CLOSE,fd,0,0,0));
}
void shell_main(int argc,const char **argv) {
    (void)argc;(void)argv;
    uint32_t r=0xa5a5a5a5,e=r,s=r;
    require(call(SYS_GETRESUID,(uintptr_t)&r,(uintptr_t)&e,0,0)==SYSCALL_EFAULT);
    require(r==0xa5a5a5a5 && e==r && s==r);
    require(!call(SYS_GETRESUID,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s,0));require(!r && !e && !s);
    require(call(SYS_GETGROUPS,0,0,0,0)==0);
    require(call(SYS_GETGROUPS,17,0,0,0)==SYSCALL_EINVAL);
    require(call(SYS_CHOWN,0,0,0,4)==SYSCALL_EINVAL);
    require(call(SYS_CHMOD,0,0644,0,0)==SYSCALL_EFAULT);
    require(call(SYS_FCHMOD,32,0644,0,0)==SYSCALL_EBADF);
    require(!call(SYS_MKDIR,(uintptr_t)"/mnt/public",0777,0,0));
    require(!call(SYS_CHMOD,(uintptr_t)"/mnt/public",0777,0,0));
    require(!call(SYS_MKDIR,(uintptr_t)"/mnt/private",0700,0,0));
    create("/mnt/private/secret");create("/mnt/public/root-secret");
    require(!call(SYS_CHMOD,(uintptr_t)"/mnt/public/root-secret",0600,0,0));
    require(!call(SYS_MKDIR,(uintptr_t)"/mnt/sticky",01777,0,0));
    require(!call(SYS_CHMOD,(uintptr_t)"/mnt/sticky",01777,0,0));create("/mnt/sticky/victim");
    proc_info_t self;require(call(SYS_PROCINFO,PROC_INFO_SELF,(uintptr_t)&self,0,0)==1);
    require(!call(SYS_TEST_SETCREDS,0,0,0,0));
    require(call(SYS_TEST_SETCREDS,0,0,0,0)==SYSCALL_EPERM);
    require(!call(SYS_GETRESUID,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s,0));require(r==1001 && e==r && s==r);
    require(!call(SYS_GETRESGID,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s,0));require(r==1001 && e==r && s==r);
    require(call(SYS_REBOOT,REBOOT_CMD_RESTART,0,0,0)==SYSCALL_EPERM);
    require(call(SYS_KILL,self.ppid,0,0,0)==SYSCALL_EPERM);
    require(call(SYS_OPEN,(uintptr_t)"/mnt/public/root-secret",VFS_O_RDONLY,0,0)==SYSCALL_EACCES);
    require(call(SYS_OPEN,(uintptr_t)"/mnt/private/secret",VFS_O_RDONLY,0,0)==SYSCALL_EACCES);
    require(call(SYS_OPEN,(uintptr_t)"/mnt/private/../public/root-secret",VFS_O_RDONLY,0,0)==SYSCALL_EACCES);
    require(call(SYS_STAT_EXT,(uintptr_t)"/mnt/missing/../public",(uintptr_t)&stat,sizeof(stat),1)==SYSCALL_ENOENT);
    require(call(SYS_CHDIR,(uintptr_t)"/mnt/private/../public",0,0,0)==SYSCALL_EACCES);
    require(call(SYS_OPEN,(uintptr_t)"/dev/sdap1",VFS_O_WRONLY,0,0)==SYSCALL_EACCES);
    require(call(SYS_OPEN,(uintptr_t)"/dev/sdap1",VFS_O_RDONLY,0,0)==SYSCALL_EACCES);
    require(call(SYS_OPEN,(uintptr_t)"/etc/motd",VFS_O_WRONLY,0,0)==SYSCALL_EROFS);
    require(call(SYS_CHOWN,(uintptr_t)"/mnt/public/root-secret",1001,1001,0)==SYSCALL_EPERM);
    require(call(SYS_UNLINK,(uintptr_t)"/mnt/sticky/victim",0,0,0)==SYSCALL_EPERM);
    require(call(SYS_RENAME,(uintptr_t)"/mnt/sticky/victim",(uintptr_t)"/mnt/sticky/victim",0,0)==SYSCALL_EPERM);
    require(call(SYS_UMASK,0027,0,0,0)==0022);
    create("/mnt/public/user-file");metadata("/mnt/public/user-file",VFS_S_IFREG|0640,1001,1001);
    require(!call(SYS_MKDIR,(uintptr_t)"/mnt/public/user-dir",0777,0,0));
    metadata("/mnt/public/user-dir",VFS_S_IFDIR|0750,1001,1001);
    require(call(SYS_OPEN,(uintptr_t)"/mnt/public/user-file/.",VFS_O_RDONLY,0,0)==SYSCALL_ENOTDIR);
    require(call(SYS_OPEN,(uintptr_t)"/mnt/public/user-file/..",VFS_O_RDONLY,0,0)==SYSCALL_ENOTDIR);
    long fd=call(SYS_OPEN,(uintptr_t)"/mnt/public/user-file",VFS_O_RDWR,0,0);require(fd>=0);
    require(!call(SYS_FCHMOD,fd,06770,0,0));
    require(call(SYS_WRITE,fd,(uintptr_t)"X",1,0)==1);
    metadata("/mnt/public/user-file",VFS_S_IFREG|0770,1001,1001);
    require(!call(SYS_FCHMOD,fd,0,0,0));require(call(SYS_WRITE,fd,(uintptr_t)"Y",1,0)==1);
    require(!call(SYS_FCHMOD,fd,06770,0,0));require(!call(SYS_CLOSE,fd,0,0,0));
    fd=call(SYS_OPEN,(uintptr_t)"/mnt/public/user-file",VFS_O_WRONLY|VFS_O_TRUNC,0,0);require(fd>=0);
    require(!call(SYS_CLOSE,fd,0,0,0));metadata("/mnt/public/user-file",VFS_S_IFREG|0770,1001,1001);require(!stat.file_size);
    require(!call(SYS_CHDIR,(uintptr_t)"/mnt/public/../public/./user-dir/..",0,0,0));
    char cwd[256];require(call(SYS_GETCWD,(uintptr_t)cwd,sizeof(cwd),0,0)==12);
    for (unsigned i=0;i<12;i++) require(cwd[i]=="/mnt/public"[i]);
    require(!call(SYS_CHDIR,(uintptr_t)"/mnt/..",0,0,0));
    require(call(SYS_GETCWD,(uintptr_t)cwd,sizeof(cwd),0,0)==2 && cwd[0]=='/' && !cwd[1]);
    require(!call(SYS_SYNC,0,0,0,0));
    text("PERM PHASE2 NONROOT ENFORCEMENT PASS\n");
}
