#include "common.h"
int poweroff_main(int argc,char **argv) {
    (void)argv;
    if (argc!=1) return tool_error("poweroff","usage: poweroff",NULL);
    long result=tool_syscall(SYS_REBOOT,2,0,0);
    return result<0 ? tool_sys_error("poweroff","operation failed",NULL,result) : 0;
}
