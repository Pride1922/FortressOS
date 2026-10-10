#include "common.h"
int rm_main(int argc,char **argv) {
    (void)argv;
    if (argc!=2) return tool_error("rm","usage: rm PATH",NULL);
    long result=tool_syscall(SYS_UNLINK,(uintptr_t)argv[1],0,0);
    return result<0 ? tool_sys_error("rm","operation failed",argv[1],result) : 0;
}
