/* Standalone FortressOS stream filter, deliberately absent from base media. */
#include "common.h"
#ifndef RELEASE
#define RELEASE 1
#endif
static unsigned char buffer[4096];
int rot13_main(int argc, char **argv) {
    if (argc == 2 && tool_equal(argv[1], "--version")) {
#if RELEASE == 1
        return tool_write("rot13", "rot13 1.0\n", 10) ? 1 : 0;
#else
        return tool_write("rot13", "rot13 1.1\n", 10) ? 1 : 0;
#endif
    }
#if RELEASE >= 2
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        static const char help[] = "Usage: rot13 < input > output\nRotate ASCII letters; preserve other bytes.\n";
        return tool_write("rot13", help, sizeof(help)-1) ? 1 : 0;
    }
#endif
    if (argc != 1) return tool_error("rot13", "unexpected argument", argv[1]);
    for (;;) {
        long n = tool_syscall(SYS_READ, 0, (uintptr_t)buffer, sizeof(buffer));
        if (n < 0) return tool_sys_error("rot13", "read failed", 0, n);
        if (!n) return 0;
        for (long i=0; i<n; i++) {
            unsigned char c=buffer[i], lower=c|32;
            if (lower >= 'a' && lower <= 'z') buffer[i]=c+(lower <= 'm' ? 13 : -13);
        }
        if (tool_write("rot13", buffer, (size_t)n)) return 1;
    }
}
