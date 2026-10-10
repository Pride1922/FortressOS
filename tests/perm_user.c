#include "syscall_abi.h"
#include "vfs.h"
static long call(long n,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
 register uintptr_t r10 __asm__("r10")=d;
 __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory","cc");return n;
}
static void text(const char *s){size_t n=0;while(s[n])n++;call(SYS_WRITE,2,(uintptr_t)s,n,0);}
static void check(bool ok,unsigned line){if(ok)return;char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};text("PERM PHASE0 FAIL ");call(SYS_WRITE,2,(uintptr_t)n,4,0);call(SYS_EXIT,1,0,0,0);__builtin_trap();}
#define require(x) check((x),__LINE__)
static struct {stat_ext_v1_t value;uint64_t canary;} ext;
static struct {vfs_stat_t value;uint64_t canary;} old;
static void number(unsigned n){char b[12];unsigned i=0;do{b[i++]='0'+n%10;n/=10;}while(n);while(i){char c=b[--i];call(SYS_WRITE,2,(uintptr_t)&c,1,0);}text(" ");}
static void stat_check(const char *path,unsigned mode,unsigned uid,unsigned gid) {
 ext.canary=old.canary=0x1122334455667788ull;
 require(!call(SYS_STAT_EXT,(uintptr_t)path,(uintptr_t)&ext.value,sizeof(ext.value),1));
 text(path);text(" ");number(ext.value.size);number(ext.value.version);number(ext.value.mode);number(ext.value.uid);number(ext.value.gid);text("\n");
 require(ext.value.size==40 && ext.value.version==1 && !ext.value.reserved && ext.value.mode==mode && ext.value.uid==uid && ext.value.gid==gid);
 require(!call(SYS_STAT,(uintptr_t)path,(uintptr_t)&old.value,0,0));
 require(old.value.size==ext.value.file_size && old.value.mode==mode && old.value.type==ext.value.type);
 require(ext.canary==0x1122334455667788ull && old.canary==ext.canary);
}
static void usb_read(unsigned rounds) {
 static unsigned char bytes[1082];
 for(unsigned i=0;i<rounds;i++) {
  long fd=call(SYS_OPEN,(uintptr_t)"/dev/sdap1",VFS_O_RDONLY,0,0);require(fd>=0);
  require(call(SYS_READ,fd,(uintptr_t)bytes,sizeof(bytes),0)==sizeof(bytes));
  require(bytes[1080]==0x53 && bytes[1081]==0xef);require(!call(SYS_CLOSE,fd,0,0,0));
 }
}
static const char *args[]={"/bin/perm-probe","child",0};
static uint64_t status;
void shell_main(int argc,const char **argv) {
 if(argc>1 && argv[1][0]=='a'){require(call(SYS_WRITE,1,(uintptr_t)"actor-action",12,0)==12);return;}
 if(argc>1 && argv[1][0]=='u' && argv[1][3]=='-'){usb_read(64);text("PERM PHASE0 CHILD PASS\n");return;}
 if(argc>1 && argv[1][0]=='c'){text("PERM PHASE0 CHILD PASS\n");return;}
 stat_check("/bin/perm-probe",VFS_S_IFREG|0755,0,0);
 stat_check("/etc/motd",VFS_S_IFREG|0644,0,0);
 stat_check("/dev/tty",VFS_S_IFCHR|0666,0,5);stat_check("/dev/console",VFS_S_IFCHR|0620,0,5);
 stat_check("/run",VFS_S_IFDIR|0755,0,0);stat_check("/tmp",VFS_S_IFDIR|01777,0,0);
 require(call(SYS_STAT_EXT,0,0,39,1)==SYSCALL_EINVAL);
 require(call(SYS_STAT_EXT,0,0,40,2)==SYSCALL_EINVAL);
 require(call(SYS_STAT_EXT,0,(uintptr_t)&ext.value,40,1)==SYSCALL_EFAULT);
 require(call(SYS_STAT_EXT,(uintptr_t)"/run",0,40,1)==SYSCALL_EFAULT);
 require(call(SYS_STAT_EXT,(uintptr_t)"/missing",(uintptr_t)&ext.value,40,1)==SYSCALL_ENOENT);
 require(!call(SYS_MKDIR,(uintptr_t)"/run/test",02750,0,0));stat_check("/run/test",VFS_S_IFDIR|02750,0,0);
 long fd=call(SYS_OPEN,(uintptr_t)"/run/test/file",VFS_O_CREAT|VFS_O_RDWR,0,0);require(fd>=0);
 require(call(SYS_WRITE,fd,(uintptr_t)"abc",3,0)==3);require(!call(SYS_CLOSE,fd,0,0,0));stat_check("/run/test/file",VFS_S_IFREG|0644,0,0);
 require(!call(SYS_CHDIR,(uintptr_t)"/run",0,0,0));
 fd=call(SYS_OPEN,(uintptr_t)"test/file",VFS_O_RDONLY,0,0);require(fd>=0);require(!call(SYS_CLOSE,fd,0,0,0));
 require(!call(SYS_CHDIR,(uintptr_t)"/",0,0,0));
 fd=call(SYS_OPEN,(uintptr_t)"/run/test",VFS_O_RDONLY,0,0);require(fd>=0);
 static vfs_dirent_t dent;require(call(SYS_READDIR,fd,(uintptr_t)&dent,0,0)==1);require(!call(SYS_CLOSE,fd,0,0,0));
 require(call(SYS_RENAME,(uintptr_t)"/run/test/file",(uintptr_t)"/run/test/renamed",0,0)==SYSCALL_EROFS);
 require(!call(SYS_KILL,0,0,0,0));
 const char *action_args[]={"/bin/perm-probe","action-child",0};
 spawn_fd_action_t action={.type=SPAWN_FD_ACTION_OPEN,.dst_fd=1,.flags=VFS_O_CREAT|VFS_O_WRONLY|VFS_O_TRUNC,.mode=06754,.path=(uintptr_t)"/run/action"};
 spawn_opts_t options={.size=sizeof(options),.version=1,.argv=(uintptr_t)action_args,.fd_actions=(uintptr_t)&action,.action_count=1,.cwd=(uintptr_t)"/run"};
 options.cwd=(uintptr_t)"/missing-cwd";
 require(call(SYS_SPAWN_EXT,(uintptr_t)action_args[0],(uintptr_t)&options,sizeof(options),0)==SYSCALL_ENOENT);
 options.cwd=(uintptr_t)"/etc/motd";
 require(call(SYS_SPAWN_EXT,(uintptr_t)action_args[0],(uintptr_t)&options,sizeof(options),0)==SYSCALL_ENOTDIR);
 options.cwd=(uintptr_t)"run"; /* relative to the current root cwd */
 long action_pid=call(SYS_SPAWN_EXT,(uintptr_t)action_args[0],(uintptr_t)&options,sizeof(options),0);require(action_pid>0);
 require(call(SYS_WAITPID,action_pid,(uintptr_t)&status,0,0)==action_pid && WIFEXITED(status) && !WEXITSTATUS(status));
 stat_check("/run/action",VFS_S_IFREG|06754,0,0);
 fd=call(SYS_OPEN,(uintptr_t)"/run/action",VFS_O_RDONLY,0,0);require(fd>=0);
 char action_bytes[12];require(call(SYS_READ,fd,(uintptr_t)action_bytes,12,0)==12);
 for(unsigned i=0;i<12;i++)require(action_bytes[i]=="actor-action"[i]);
 require(!call(SYS_CLOSE,fd,0,0,0));require(!call(SYS_UNLINK,(uintptr_t)"/run/action",0,0,0));
 text("PERM PHASE1 ACTOR PATHS/ACTIONS/SIGNAL PASS\n");
 require(!call(SYS_UNLINK,(uintptr_t)"/run/test/file",0,0,0));require(!call(SYS_UNLINK,(uintptr_t)"/run/test",0,0,0));
 if(argc>1 && (argv[1][0]=='m' || argv[1][0]=='u')) {
  stat_check("/mnt/mode",VFS_S_IFREG|07777,0x12345678,0x87654321);
  fd=call(SYS_OPEN,(uintptr_t)"/mnt/phase1-a",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_TRUNC,0,0);require(fd>=0);
  require(call(SYS_WRITE,fd,(uintptr_t)"rename-actor",12,0)==12);require(!call(SYS_CLOSE,fd,0,0,0));
  require(!call(SYS_RENAME,(uintptr_t)"/mnt/phase1-a",(uintptr_t)"/mnt/phase1-b",0,0));
  stat_check("/mnt/phase1-b",VFS_S_IFREG|0644,0,0);require(!call(SYS_UNLINK,(uintptr_t)"/mnt/phase1-b",0,0,0));
 }
 bool usb=argc>1 && argv[1][0]=='u';long writer=-1;
 if(usb) {
  stat_check("/dev/sdap1",VFS_S_IFBLK|0660,0,6);require(ext.value.mnt_flags==VFS_MNT_RDONLY);
  require(call(SYS_OPEN,(uintptr_t)"/dev/sdap1",VFS_O_WRONLY,0,0)==SYSCALL_EROFS);
  args[1]="usb-child";writer=call(SYS_OPEN,(uintptr_t)"/mnt/perm-io",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_TRUNC,0,0);require(writer>=0);
 }
 long pid=call(SYS_SPAWN,(uintptr_t)args[0],(uintptr_t)args,0,0);require(pid>0);
 if(usb) {
  for(unsigned i=0;i<64;i++)require(call(SYS_WRITE,writer,(uintptr_t)"raw-reader-peer!",16,0)==16);
  require(!call(SYS_CLOSE,writer,0,0,0));
 }
 require(call(SYS_WAITPID,pid,(uintptr_t)&status,0,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
 if(usb) {
  stat_check("/mnt/perm-io",VFS_S_IFREG|0644,0,0);require(ext.value.file_size==1024);
  require(!call(SYS_UNLINK,(uintptr_t)"/mnt/perm-io",0,0,0));text("PERM PHASE0 USB RAW READ/FILESYSTEM PEER PASS\n");
 }
 text("PERM PHASE0 STAT ABI/NAMESPACES/SPAWN PASS\n");
}
