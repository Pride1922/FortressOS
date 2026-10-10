#include "common.h"
int mv_main(int argc,char **argv) {
    (void)argv;
    if (argc!=3) return tool_error("mv","usage: mv SOURCE DESTINATION",NULL);
    long result=tool_syscall(SYS_RENAME,(uintptr_t)argv[1],(uintptr_t)argv[2],0);
    return result<0 ? tool_sys_error("mv","operation failed",argv[1],result) : 0;
}
