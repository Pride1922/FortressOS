/* Acceptance-only Ring 3 probe. Never packaged in release media. */
#include "common.h"
#include "creds.h"
#include "../user/permissions_cli.h"
static int fail(const char *what) {return tool_error("IDENTITY_FAIL",what,NULL);}
static long launch(spawn_opts_v3_t *o) {
    return tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",(uintptr_t)o,sizeof(*o));
}
static bool boundary(void) {
    spawn_opts_v3_t o={.base={.size=sizeof(o),.version=3},.identity={0}};
    if (tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",1,sizeof(o))!=SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",UINT64_MAX-63,sizeof(o))!=SYSCALL_EFAULT ||
        tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",0xffffffff80000000ull,sizeof(o))!=SYSCALL_EFAULT)
        return false;
    if (tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",(uintptr_t)&o,sizeof(o)-1)!=SYSCALL_EINVAL)
        return false;
    o.base.version=2;if (launch(&o)!=SYSCALL_EINVAL) return false;o.base.version=3;
    o.identity.reserved=1;if (launch(&o)!=SYSCALL_EINVAL) return false;o.identity.reserved=0;
    o.identity.ngroups=17;if (launch(&o)!=SYSCALL_EINVAL) return false;o.identity.ngroups=0;
    o.identity.groups[15]=1;if (launch(&o)!=SYSCALL_EINVAL) return false;o.identity.groups[15]=0;
    o.base.reserved0=1;if (launch(&o)!=SYSCALL_EINVAL) return false;o.base.reserved0=0;
    o.base.flags=0x80000000;if (launch(&o)!=SYSCALL_EINVAL) return false;o.base.flags=0;
    long pid=launch(&o);uint64_t status=99;
    if (pid<=0 || tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0) || status) return false;
    spawn_opts_t old={.size=sizeof(old)};
    for (unsigned version=1;version<=2;version++) {
        old.version=version;
        pid=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/id",(uintptr_t)&old,sizeof(old));
        if (pid<=0 || tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0) || status) return false;
    }
    spawn_fd_action_t action={.type=SPAWN_FD_ACTION_OPEN,.dst_fd=0,
        .path=(uintptr_t)"/etc/shadow",.flags=VFS_O_RDONLY};
    o.identity.uid=1000;o.identity.gid=1000;
    o.base.fd_actions=(uintptr_t)&action;o.base.action_count=1;
    return launch(&o)==SYSCALL_EACCES;
}
int identity_probe_main(int argc,char **argv) {
    if (argc!=2) return fail("arguments");
    bool reduced=tool_equal(argv[1],"reduced");
    bool root=tool_equal(argv[1],"root") || reduced, override=tool_equal(argv[1],"override");
    uint32_t uid,euid,suid,gid,egid,sgid,groups[16];
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid) ||
        tool_syscall(SYS_GETRESGID,(uintptr_t)&gid,(uintptr_t)&egid,(uintptr_t)&sgid)) return fail("getres");
    uint32_t want=root ? 0:1000, wantgid=root ? 0:override ? 44:1000;
    if (uid!=want || euid!=want || suid!=want || gid!=wantgid || egid!=wantgid || sgid!=wantgid)
        return fail("IDs");
    long n=tool_syscall(SYS_GETGROUPS,16,(uintptr_t)groups,0);
    const uint32_t normal[]={1000,10,44,104}, changed[]={44,10,104};
    if (n!=(reduced ? 0:root ? 1:override ? 3:4)) return fail("group count");
    for (long i=0;i<n;i++) if (groups[i]!=(root ? 0:override ? changed[i]:normal[i])) return fail("group values");
    if (tool_syscall(SYS_CAPGET,0,0,0)!=(long)(reduced ? CAP_SETUID|CAP_SETGID:root ? CAP_ALL:0)) return fail("capabilities");
    if (root && !reduced) {
        if (!boundary()) return fail("spawn ABI boundaries");
        if (tool_syscall(SYS_CAPSET,CAP_SETUID|CAP_SETGID,0,0)) return fail("drop root caps");
        const char *args[]={"/bin/identity-probe","reduced",NULL};
        spawn_opts_v3_t o={.base={.size=sizeof(o),.version=3,.argv=(uintptr_t)args}};
        long pid=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&o,sizeof(o));
        uint64_t status=99;
        if (pid<=0 || tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0) || status) return fail("root cap inheritance");
        if (tool_syscall(SYS_CAPSET,CAP_SETUID,0,0) || launch(&o)!=SYSCALL_EPERM) return fail("missing SETGID");
    } else if (!root) {
        if (permission_call4(SYS_SETRESUID,0,0,0,0)!=SYSCALL_EPERM ||
            permission_call4(SYS_SETRESGID,0,0,0,0)!=SYSCALL_EPERM ||
            tool_syscall(SYS_SETGROUPS,0,0,0)!=SYSCALL_EPERM) return fail("regain IDs");
        if (tool_syscall(SYS_CAPSET,CAP_ALL,0,0) || tool_syscall(SYS_CAPGET,0,0,0)) return fail("regain caps");
        long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/etc/shadow",VFS_O_RDONLY,0);
        if (fd>=0) return fail("shadow access");
        spawn_opts_v3_t o={.base={.size=sizeof(o),.version=3}};
        if (launch(&o)!=SYSCALL_EPERM) return fail("privileged spawn");
    }
    tool_write("probe","IDENTITY_PASS:",14);
    tool_write("probe",argv[1],tool_length(argv[1]));tool_write("probe","\n",1);
    return 0;
}
