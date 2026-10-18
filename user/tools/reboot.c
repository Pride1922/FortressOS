#include "common.h"
#include "shutdown_coord.h"

int reboot_main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) return tool_error("reboot", "usage: reboot", NULL);
    int result = shutdown_coordinate_and_reboot(1 /* REBOOT_CMD_RESTART */);
    return result < 0 ? tool_sys_error("reboot", "operation failed", NULL, result) : 0;
}
