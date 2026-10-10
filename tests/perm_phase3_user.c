#include "../user/tools/common.h"
#include "../user/permissions_cli.h"
#include "creds.h"
static int check(bool ok,const char *what) {if (!ok) return tool_error("PHASE3 FAIL",what,0);return 0;}
#define REQUIRE(x) do {if (check((x),#x)) return 1;} while (0)
int phase3_main(int argc,char **argv) {
    uint32_t u,e,s,g,eg,sg,groups[16];
    REQUIRE(!tool_syscall(SYS_GETRESUID,(uintptr_t)&u,(uintptr_t)&e,(uintptr_t)&s));
    REQUIRE(!tool_syscall(SYS_GETRESGID,(uintptr_t)&g,(uintptr_t)&eg,(uintptr_t)&sg));
    if (argc==2 && tool_equal(argv[1],"--root")) {
        REQUIRE(!u && !e && !s && !g && tool_syscall(SYS_CAPGET,0,0,0)==CAP_ALL);
        REQUIRE(permission_call4(SYS_SETRESUID,UINT32_MAX,UINT32_MAX,UINT32_MAX,8)==SYSCALL_EINVAL);
        REQUIRE(permission_call4(SYS_SETRESUID,UINT32_MAX,UINT32_MAX,UINT32_MAX,7)==0);
        REQUIRE(tool_syscall(SYS_SETGROUPS,17,0,0)==SYSCALL_EINVAL);
        REQUIRE(tool_syscall(SYS_SETGROUPS,1,0,0)==SYSCALL_EFAULT);
        REQUIRE(tool_syscall(SYS_SETGROUPS,1,0xffffffff80000000ull,0)==SYSCALL_EFAULT);
        REQUIRE(tool_syscall(SYS_CAPSET,1ull<<63,0,0)==SYSCALL_EINVAL);
        REQUIRE(!tool_syscall(SYS_CAPSET,0,0,0));
        REQUIRE(!tool_syscall(SYS_CAPSET,CAP_ALL,0,0) && !tool_syscall(SYS_CAPGET,0,0,0));
        REQUIRE(permission_call4(SYS_SETRESUID,1000,1000,1000,0)==SYSCALL_EPERM);
        REQUIRE(tool_syscall(SYS_SETGROUPS,0,0,0)==SYSCALL_EPERM);
        return tool_write("probe","PHASE3 ROOT ABI/DROP PASS\n",25);
    }
    REQUIRE(u==1000 && e==1000 && s==1000 && g==1000 && eg==1000 && sg==1000);
    REQUIRE(tool_syscall(SYS_GETGROUPS,16,(uintptr_t)groups,0)==4 && groups[0]==1000 && groups[1]==10 && groups[2]==44 && groups[3]==104);
    REQUIRE(!tool_syscall(SYS_CAPGET,0,0,0));
    REQUIRE(tool_syscall(SYS_OPEN,(uintptr_t)"/etc/shadow",VFS_O_RDONLY,0)==SYSCALL_EACCES);
    REQUIRE(permission_call4(SYS_SETRESUID,0,0,0,0)==SYSCALL_EPERM);
    REQUIRE(permission_call4(SYS_SETRESGID,0,0,0,0)==SYSCALL_EPERM);
    REQUIRE(tool_syscall(SYS_SETGROUPS,0,0,0)==SYSCALL_EPERM);
    REQUIRE(!tool_syscall(SYS_CAPSET,CAP_ALL,0,0) && !tool_syscall(SYS_CAPGET,0,0,0));
    stat_ext_v1_t st;
    REQUIRE(!permission_call4(SYS_STAT_EXT,(uintptr_t)"/run/user/1000",(uintptr_t)&st,sizeof(st),1));
    REQUIRE(st.uid==1000 && st.gid==1000 && st.type==VFS_DIRECTORY && st.mode==(VFS_S_IFDIR|0700));
    if (argc==1) {
        const char *args[]={"/bin/phase3-probe","--child",0};
        long pid=tool_syscall(SYS_SPAWN,(uintptr_t)args[0],(uintptr_t)args,0);REQUIRE(pid>0);
        uint64_t status=1;REQUIRE(!tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0) && !status);
    }
    const char *message="PHASE3 OPERATOR uid=1000 euid=1000 caps=0 groups=1000,10,44,104 PASS\n";
    return tool_write("probe",message,tool_length(message));
}
