#include "common.h"
#include "shutdown_coord.h"

int poweroff_main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) return tool_error("poweroff", "usage: poweroff", NULL);
    int result = shutdown_coordinate_and_reboot(2 /* REBOOT_CMD_POWEROFF */);
    return result < 0 ? tool_sys_error("poweroff", "operation failed", NULL, result) : 0;
}
