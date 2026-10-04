#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "shell/io.h"
#include "shell/builtin_exec.h"

int dmesg_main(int argc, char **argv) {
    return exec_dmesg(argc, (const char *const *)argv);
}
