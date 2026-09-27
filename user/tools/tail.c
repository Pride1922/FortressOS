#include "common.h"
typedef struct {
    unsigned char data[TAIL_LINE_BYTES + 1];
    size_t length;
    bool overflow;
} tail_line_t;
static union {
    unsigned char bytes[TAIL_MAX_BYTES];
    tail_line_t lines[TAIL_MAX_LINES];
} store;
_Static_assert(TAIL_MAX_LINES * TAIL_LINE_BYTES == 40 * 1024,
               "tail line-content budget must remain 40 KiB");

static int suffix(int fd, const char *label, void *ctx) {
    const tool_options_t *opts = ctx;
    size_t limit = (size_t)opts->count, used = 0, next = 0;
    bool new_line = true;
    for (;;) {
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("tail", "read error", label);
        if (!n) break;
        if (!limit) continue; /* Zero is discard-and-consume, not early exit. */
        for (long i = 0; i < n; i++) {
            unsigned char c = tool_buffer[i];
            if (opts->bytes) {
                store.bytes[next] = c;
                next = (next + 1) % limit;
                if (used < limit) used++;
            } else {
                /* Advance only on the first byte of a real following line. */
                if (new_line) {
                    if (used) next = (next + 1) % limit;
                    if (used < limit) used++;
                    store.lines[next].length = 0;
                    store.lines[next].overflow = false;
                    new_line = false;
                }
                tail_line_t *line = &store.lines[next];
                if (c == '\n') {
                    if (!line->overflow) line->data[line->length++] = c;
                    new_line = true;
                } else if (line->length < TAIL_LINE_BYTES) {
                    line->data[line->length++] = c;
                } else line->overflow = true;
            }
        }
    }
    if (!used) return 0;
    if (opts->bytes) {
        size_t first = used == limit ? next : 0;
        size_t span = limit - first;
        if (span > used) span = used;
        int r = tool_write("tail", store.bytes + first, span);
        if (r) return r;
        return tool_write("tail", store.bytes, used - span);
    }
    size_t first = used == limit ? (next + 1) % limit : 0;
    for (size_t i = 0; i < used; i++)
        if (store.lines[(first + i) % limit].overflow)
            return tool_error("tail", "retained line exceeds 4096 bytes", NULL);
    for (size_t i = 0; i < used; i++) {
        tail_line_t *line = &store.lines[(first + i) % limit];
        int r = tool_write("tail", line->data, line->length);
        if (r) return r;
    }
    return 0;
}
int tail_main(int argc, char **argv) {
    tool_options_t opts;
    int r = tool_options(TOOL_TAIL, argc, argv, &opts);
    if (r) return r == 1 ? tool_help(TOOL_TAIL) : r;
    return tool_inputs("tail", argc, argv, opts.first, suffix, &opts);
}
