#include "common.h"

int nologin_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "This account is currently not available.\n";
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)msg, sizeof(msg) - 1);
    return 1;
}
