#include "common.h"
int mkdir_main(int argc,char **argv) {
    (void)argv;
    if (argc!=2) return tool_error("mkdir","usage: mkdir PATH",NULL);
    long result=tool_syscall(SYS_MKDIR,(uintptr_t)argv[1],0755,0);
    return result<0 ? tool_sys_error("mkdir","operation failed",argv[1],result) : 0;
}
