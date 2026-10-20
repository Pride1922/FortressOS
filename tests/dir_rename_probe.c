#include "common.h"
static const char payload[]="DIRECTORY-RENAME-PAYLOAD";
static char bytes[64];
static stat_ext_v1_t metadata;
static void say(const char *s){tool_write("dir-rename-probe",s,tool_length(s));}
#define CHECK(x) do{if(!(x)){say("DIR_RENAME FAIL " #x "\n");return 1;}}while(0)
static long mkdir_path(const char *p){return tool_syscall(SYS_MKDIR,(uintptr_t)p,0755,0);}
static long rename_path(const char *a,const char *b){return tool_syscall(SYS_RENAME,(uintptr_t)a,(uintptr_t)b,0);}
static long open_path(const char *p,unsigned flags){return tool_syscall(SYS_OPEN,(uintptr_t)p,flags,0);}
static long close_fd(long f){return tool_syscall(SYS_CLOSE,f,0,0);}
static int write_payload(const char *p){long f=open_path(p,VFS_O_CREAT|VFS_O_RDWR);CHECK(f>=0);
    CHECK(tool_syscall(SYS_WRITE,f,(uintptr_t)payload,sizeof(payload)-1)==sizeof(payload)-1);CHECK(!close_fd(f));return 0;}
int dir_rename_probe_main(int argc,char **argv){
    (void)argv;
    if(argc>1){
        long f=open_path("/var/moveddir/sub/nested/data",VFS_O_RDONLY);CHECK(f>=0);
        CHECK(tool_syscall(SYS_READ,f,(uintptr_t)bytes,sizeof(bytes))==sizeof(payload)-1);
        for(unsigned i=0;i<sizeof(payload)-1;i++)CHECK(bytes[i]==payload[i]);
        CHECK(!close_fd(f));say("DIR_RENAME REBOOT PASS\n");return 0;
    }
    CHECK(!mkdir_path("/opt/testdir")&&!mkdir_path("/opt/testdir/sub")&&!mkdir_path("/opt/testdir/sub/nested"));
    CHECK(!write_payload("/opt/testdir/sub/nested/data"));
    long file=open_path("/opt/testdir/sub/nested/data",VFS_O_RDWR),dir=open_path("/opt/testdir",VFS_O_RDONLY);CHECK(file>=0&&dir>=0);
    CHECK(!rename_path("/opt/testdir","/opt/renameddir"));
    CHECK(open_path("/opt/testdir",VFS_O_RDONLY)==SYSCALL_ENOENT);
    long fresh=open_path("/opt/renameddir/sub/nested/data",VFS_O_RDONLY);CHECK(fresh>=0&&!close_fd(fresh));
    CHECK(!rename_path("/opt/renameddir/","/var/moveddir/"));
    CHECK(tool_syscall(SYS_READ,file,(uintptr_t)bytes,sizeof(bytes))==sizeof(payload)-1);
    for(unsigned i=0;i<sizeof(payload)-1;i++)CHECK(bytes[i]==payload[i]);
    CHECK(!tool_syscall(SYS_FSTAT,file,(uintptr_t)&metadata,0)&&metadata.file_size==sizeof(payload)-1);
    CHECK(!tool_syscall(SYS_FSTAT,dir,(uintptr_t)&metadata,0)&&metadata.type==VFS_DIRECTORY);
    CHECK(rename_path("/var/moveddir","/var/moveddir/sub/loop")==SYSCALL_EINVAL);
    CHECK(rename_path("/var/moveddir","/var/moveddir/sub")==SYSCALL_EINVAL);
    CHECK(!mkdir_path("/opt/existing"));CHECK(rename_path("/var/moveddir","/opt/existing")==SYSCALL_EEXIST);
    CHECK(!rename_path("/var/moveddir","/var/moveddir"));
    CHECK(!close_fd(file)&&!close_fd(dir));
    /* Load and validate .. after relocation through ordinary pathname lookup. */
    fresh=open_path("/var/moveddir/sub/../sub/nested/data",VFS_O_RDONLY);CHECK(fresh>=0&&!close_fd(fresh));
    CHECK(!mkdir_path("/opt/tool-dir")&&!mkdir_path("/opt/tool-dir/sub"));CHECK(!write_payload("/opt/tool-dir/sub/data"));
    CHECK(!tool_syscall(SYS_SYNC,0,0,0));say("DIR_RENAME Ring3 PASS\n");return 0;
}
