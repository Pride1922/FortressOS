#include "../user/tools/common.h"
#include "../user/permissions_cli.h"
#include "../user/entry_security.h"
#include "creds.h"
#include "../src/drivers/power.h"
#define REQUIRE(x) do {if (!(x)) return tool_error("PHASE4 FAIL",#x,0);} while (0)
static int run(const char *path,const char *arg,const spawn_fd_action_t *actions,unsigned count,bool staged) {
    const char *args[]={path,arg,NULL},*env[]={"PATH=/evil","HOME=/evil","TERM=test",NULL};
    spawn_opts_t opts={.size=sizeof(opts),.version=staged ? 2:1,.flags=staged ? SPAWN_STAGED|SPAWN_SETPGROUP:0,
        .argv=(uintptr_t)args,.envp=(uintptr_t)env,.fd_actions=(uintptr_t)actions,.action_count=count};
    long pid=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)path,(uintptr_t)&opts,sizeof(opts));
    if (pid<0) return (int)pid;
    if (staged) {
        if (tool_syscall(SYS_UMASK,0077,0,0)<0 || tool_syscall(SYS_GROUP_RELEASE,pid,GROUP_RELEASE,0)<0) return -1;
    }
    uint64_t status=1;
    return tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0)<0 ? -1:(int)status;
}
int phase4_main(int argc,char **argv,const char *const *envp) {
    uint32_t u,e,s,g,eg,sg;
    REQUIRE(!tool_syscall(SYS_GETRESUID,(uintptr_t)&u,(uintptr_t)&e,(uintptr_t)&s));
    REQUIRE(!tool_syscall(SYS_GETRESGID,(uintptr_t)&g,(uintptr_t)&eg,(uintptr_t)&sg));
    long caps=tool_syscall(SYS_CAPGET,0,0,0);unsigned char byte;
    if (argc==2 && (tool_equal(argv[1],"--secure") || tool_equal(argv[1],"--mapped"))) {
        REQUIRE(u==1000 && !e && !s && g==1000 && eg==1000 && caps==CAP_ALL && user_entry_secure(envp));
        REQUIRE(tool_syscall(SYS_READ,31,(uintptr_t)&byte,1)==SYSCALL_EBADF);
        REQUIRE(tool_syscall(SYS_UMASK,UMASK_QUERY,0,0)==0022);
        if (tool_equal(argv[1],"--mapped")) REQUIRE(tool_syscall(SYS_READ,9,(uintptr_t)&byte,1)==1);
        else REQUIRE(tool_syscall(SYS_READ,9,(uintptr_t)&byte,1)==SYSCALL_EBADF);
        return tool_write("probe","PHASE4 SECURE PASS\n",19);
    }
    if (argc==2 && tool_equal(argv[1],"--sgid")) {
        REQUIRE(u==1000 && e==1000 && caps==0 && eg==44 && sg==44 && user_entry_secure(envp));
        REQUIRE(tool_syscall(SYS_READ,31,(uintptr_t)&byte,1)==SYSCALL_EBADF);
        return tool_write("probe","PHASE4 SGID PASS\n",17);
    }
    if (argc==2 && tool_equal(argv[1],"--nosuid")) {
        REQUIRE(u==1000 && e==1000 && s==1000 && eg==1000 && !caps && !user_entry_secure(envp));
        return tool_write("probe","PHASE4 NOSUID PASS\n",19);
    }
    if (argc==2 && tool_equal(argv[1],"--root-child")) {
        REQUIRE(!u && !e && !caps && user_entry_secure(envp));
        REQUIRE(tool_syscall(SYS_DMESG,(uintptr_t)&byte,1,0)>=0);
        return tool_write("probe","PHASE4 ROOT DROP PASS\n",22);
    }
    if (argc==2 && tool_equal(argv[1],"--root-drop")) {
        REQUIRE(!u && !e && caps==CAP_ALL);REQUIRE(!tool_syscall(SYS_CAPSET,0,0,0));
        REQUIRE(!run("/bin/suid-probe","--root-child",NULL,0,false));return 0;
    }
    REQUIRE(u==1000 && e==1000 && caps==0);
    byte=0xa5;
    REQUIRE(tool_syscall(SYS_DMESG,(uintptr_t)&byte,1,0)==SYSCALL_EPERM && byte==0xa5);
    REQUIRE(tool_syscall(SYS_DMESG,0,0,0)==SYSCALL_EPERM);
    /* Ordinary processes remain unpinned: preserve the network BSP fence.
     * The actual admitted IFSET handler has separate host authority gates. */
    long net_result=tool_syscall(SYS_NETCTL,NETCTL_IFSET,0,0);
    REQUIRE(net_result==SYSCALL_EPERM || net_result==SYSCALL_EOPNOTSUPP);
    REQUIRE(tool_syscall(SYS_REBOOT,REBOOT_CMD_POWEROFF,0,0)==SYSCALL_EPERM);
    proc_info_t info;
    REQUIRE(tool_syscall(SYS_PROCINFO,0,(uintptr_t)&info,0)==1);
    sysinfo_mem_t before,after;
    REQUIRE(!tool_syscall(SYS_MEMINFO,(uintptr_t)&before,sizeof(before),0));
    unsigned rng=5590;
    for (unsigned i=0;i<256;i++) {
        rng=rng*1664525+1013904223;
        spawn_fd_action_t action={.type=SPAWN_FD_ACTION_OPEN,.dst_fd=9,.flags=VFS_O_RDONLY,.path=(uintptr_t)"/etc/shadow",.reserved=rng|1};
        spawn_opts_t bad={.size=sizeof(bad),.version=1,.action_count=1,.fd_actions=(uintptr_t)&action};
        if (i&1) {bad.action_count=0;bad.fd_actions=0;bad.reserved1=rng|1;}
        REQUIRE(tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/suid-probe",(uintptr_t)&bad,sizeof(bad))==SYSCALL_EINVAL);
    }
    REQUIRE(!tool_syscall(SYS_MEMINFO,(uintptr_t)&after,sizeof(after),0));
    REQUIRE(before.heap_used_bytes==after.heap_used_bytes && before.vmm_table_frames==after.vmm_table_frames);
    REQUIRE(tool_syscall(SYS_CAPGET,0,0,0)==0);
    tool_write("probe","PHASE5 DENIAL/FUZZ PASS\n",23);
    REQUIRE(tool_syscall(SYS_OPEN,(uintptr_t)"/etc/shadow",VFS_O_RDONLY,0)==SYSCALL_EACCES);
    int fd=tool_syscall(SYS_OPEN,(uintptr_t)"/etc/passwd",VFS_O_RDONLY,0);REQUIRE(fd>=3);
    REQUIRE(tool_syscall(SYS_DUP2,fd,31,0)==31);
    REQUIRE(!run("/bin/suid-probe","--secure",NULL,0,false));
    spawn_fd_action_t dup={.type=SPAWN_FD_ACTION_DUP2,.src_fd=31,.dst_fd=9};
    REQUIRE(!run("/bin/suid-probe","--mapped",&dup,1,false));
    spawn_fd_action_t open={.type=SPAWN_FD_ACTION_OPEN,.dst_fd=9,.flags=VFS_O_RDONLY,.path=(uintptr_t)"/etc/shadow"};
    REQUIRE(run("/bin/suid-probe","--mapped",&open,1,false)==SYSCALL_EACCES);
    REQUIRE(!run("/bin/sgid-probe","--sgid",NULL,0,false));
    REQUIRE(!run("/bin/suid-probe","--secure",NULL,0,true));
    REQUIRE(!run("/mnt/nosuid-probe","--nosuid",NULL,0,false));
    tool_syscall(SYS_CLOSE,fd,0,0);tool_syscall(SYS_CLOSE,31,0,0);
    return tool_write("probe","PHASE4 SPAWN/FDS/STAGED/NOSUID PASS\n",36);
}
