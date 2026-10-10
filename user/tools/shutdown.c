#include "common.h"

int shutdown_main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) return tool_error("shutdown", "usage: shutdown", NULL);
    /* The kernel enforces CAP_SYS_BOOT and owns sync/freeze ordering. */
    long result = tool_syscall(SYS_REBOOT, 2, 0, 0);
    if (result < 0) return tool_sys_error("shutdown", "shutdown failed", NULL, result);
    return 0;
}
