#include "common.h"
static int copy(int fd, const char *label, void *ctx) {
    (void)ctx;
    for (;;) {
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("cat", "read error", label);
        if (!n) return 0;
        int r = tool_write("cat", tool_buffer, (size_t)n);
        if (r) return r;
    }
}
int cat_main(int argc, char **argv) {
    tool_options_t opts;
    int r = tool_options(TOOL_CAT, argc, argv, &opts);
    if (r) return r == 1 ? tool_help(TOOL_CAT) : r;
    return tool_inputs("cat", argc, argv, opts.first, copy, NULL);
}
