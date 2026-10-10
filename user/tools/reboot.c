#include "common.h"
int reboot_main(int argc,char **argv) {
    (void)argv;
    if (argc!=1) return tool_error("reboot","usage: reboot",NULL);
    long result=tool_syscall(SYS_REBOOT,1,0,0);
    return result<0 ? tool_sys_error("reboot","operation failed",NULL,result) : 0;
}
