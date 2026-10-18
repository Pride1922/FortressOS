#include "common.h"
#include "../kill_cli.h"

int kill_main(int argc,char **argv) {
    if (argc==2 && tool_equal(argv[1],"--help")) {
        const char help[]="usage: kill PID [SIGNAL] | kill -SIGNAL PID | kill -s SIGNAL PID\n"
                          "Default: TERM. Use KILL (9) for a stuck process. Positive PIDs only.\n";
        return tool_write("kill",help,sizeof(help)-1);
    }
    const char *target;int sig;int64_t pid;
    if (!kill_arguments(argc,(const char *const *)argv,&target,&sig) || !kill_pid(target,&pid)) {
        tool_error("kill","usage: kill PID [SIGNAL] | kill -SIGNAL PID | kill -s SIGNAL PID",NULL);
        return 2;
    }
    long rc=tool_syscall(SYS_KILL,(uintptr_t)pid,(uintptr_t)sig,0);
    return rc<0 ? tool_sys_error("kill","cannot signal process",target,rc) : 0;
}
