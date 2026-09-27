#include "common.h"
static int prefix(int fd, const char *label, void *ctx) {
    const tool_options_t *opts = ctx;
    uint64_t remaining = opts->count;
    while (remaining) {
        size_t size = TOOL_BUFFER_SIZE;
        if (opts->bytes && remaining < size) size = (size_t)remaining;
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, size);
        if (n < 0) return tool_error("head", "read error", label);
        if (!n) break;
        size_t emit = (size_t)n;
        if (opts->bytes) remaining -= emit;
        else {
            for (size_t i = 0; i < (size_t)n; i++) {
                if (tool_buffer[i] == '\n' && --remaining == 0) { emit = i + 1; break; }
            }
        }
        int r = tool_write("head", tool_buffer, emit);
        if (r) return r;
    }
    return 0;
}
int head_main(int argc, char **argv) {
    tool_options_t opts;
    int r = tool_options(TOOL_HEAD, argc, argv, &opts);
    if (r) return r == 1 ? tool_help(TOOL_HEAD) : r;
    return tool_inputs("head", argc, argv, opts.first, prefix, &opts);
}
