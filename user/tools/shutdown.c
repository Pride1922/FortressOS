#include "common.h"
#include "shutdown_coord.h"

int shutdown_main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) return tool_error("shutdown", "usage: shutdown", NULL);
    int result = shutdown_coordinate_and_reboot(2 /* REBOOT_CMD_POWEROFF */);
    if (result < 0) return tool_sys_error("shutdown", "shutdown failed", NULL, result);
    return 0;
}
