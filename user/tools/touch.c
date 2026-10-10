#include "common.h"

int touch_main(int argc, char **argv) {
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        const char *s = "Usage: touch [-c] FILE...\n";
        return tool_write("touch", s, tool_length(s));
    }
    if (argc < 2) {
        return tool_error("touch", "missing file operand", NULL);
    }

    bool no_create = false;
    int first_file = 1;

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--")) {
            first_file = i + 1;
            break;
        }
        if (argv[i][0] == '-' && argv[i][1]) {
            if (tool_equal(argv[i], "-c") || tool_equal(argv[i], "--no-create")) {
                no_create = true;
            } else {
                return tool_error("touch", "invalid option", argv[i]);
            }
        } else {
            first_file = i;
            break;
        }
    }

    if (first_file >= argc) {
        return tool_error("touch", "missing file operand", NULL);
    }

    int status = 0;
    for (int i = first_file; i < argc; i++) {
        if (no_create) {
            vfs_stat_t st;
            long r = tool_syscall(SYS_STAT, (uintptr_t)argv[i], (uintptr_t)&st, 0);
            if (r < 0) {
                if (r == SYSCALL_ENOENT) continue;
                status = tool_sys_error("touch", "cannot stat", argv[i], r);
                continue;
            }
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)argv[i], VFS_O_WRONLY, 0);
            if (fd < 0) {
                status = tool_sys_error("touch", "cannot touch", argv[i], fd);
            } else {
                tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            }
        } else {
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)argv[i], VFS_O_CREAT | VFS_O_WRONLY, 0);
            if (fd < 0) {
                status = tool_sys_error("touch", "cannot touch", argv[i], fd);
            } else {
                tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            }
        }
    }
    return status;
}
