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

static volatile int g_tail_signal = 0;

static void tail_sig_handler(int sig) {
    g_tail_signal = sig;
}

static void tail_diag(const char *label, const char *msg) {
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"tail: ", 6);
    if (label) {
        (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)label, tool_length(label));
        (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)": ", 2);
    }
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)msg, tool_length(msg));
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"\n", 1);
}

static int suffix(int fd, const char *label, void *ctx) {
    const tool_options_t *opts = ctx;
    size_t limit = (size_t)opts->count, used = 0, next = 0;
    bool new_line = true;
    uint64_t offset = 0;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("tail", "read error", label);
        if (!n) break;
        offset += (uint64_t)n;
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

    if (used) {
        if (opts->bytes) {
            size_t first = used == limit ? next : 0;
            size_t span = limit - first;
            if (span > used) span = used;
            int r = tool_write("tail", store.bytes + first, span);
            if (r) return r;
            r = tool_write("tail", store.bytes, used - span);
            if (r) return r;
        } else {
            size_t first = used == limit ? (next + 1) % limit : 0;
            for (size_t i = 0; i < used; i++)
                if (store.lines[(first + i) % limit].overflow)
                    return tool_error("tail", "retained line exceeds 4096 bytes", NULL);
            for (size_t i = 0; i < used; i++) {
                tail_line_t *line = &store.lines[(first + i) % limit];
                int r = tool_write("tail", line->data, line->length);
                if (r) return r;
            }
        }
    }

    if (!opts->follow) return 0;

    /* Follow phase */
    if (!label || tool_equal(label, "-")) {
        tail_diag(label, "cannot follow standard input");
        return 0;
    }

    /* Set up signal handling for clean exit */
    g_tail_signal = 0;
    signal_action_t sa = {.handler = (uintptr_t)tail_sig_handler, .mask = 0, .flags = 0, .reserved = 0};
    tool_syscall(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0);
    tool_syscall(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0);

    for (;;) {
        if (g_tail_signal == SIGINT) return 130;
        if (g_tail_signal == SIGTERM) return 0;

        /* Truncation / deletion detection */
        vfs_stat_t st;
        long stat_res = tool_syscall(SYS_STAT, (uintptr_t)label, (uintptr_t)&st, 0);
        if (stat_res < 0) {
            tail_diag(label, "file has been deleted");
            return 1;
        }

        if (st.size < offset) {
            tail_diag(label, "file truncated");
            tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            long new_fd = tool_syscall(SYS_OPEN, (uintptr_t)label, VFS_O_RDONLY, 0);
            if (new_fd < 0) {
                return tool_sys_error("tail", "cannot reopen", label, new_fd);
            }
            if (new_fd != fd) {
                tool_syscall(SYS_DUP2, (uintptr_t)new_fd, (uintptr_t)fd, 0);
                tool_syscall(SYS_CLOSE, (uintptr_t)new_fd, 0, 0);
            }
            offset = 0;
        }

        /* Read new data */
        for (;;) {
            if (g_tail_signal == SIGINT) return 130;
            if (g_tail_signal == SIGTERM) return 0;

            long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
            if (n < 0) return tool_error("tail", "read error", label);
            if (n == 0) break;

            offset += (uint64_t)n;
            int w = tool_write("tail", tool_buffer, (size_t)n);
            if (w) return w;
        }

        /* Bounded 100ms poll interval */
        timespec_t req = {.tv_sec = 0, .tv_nsec = 100000000L};
        timespec_t rem;
        long ret = tool_syscall(SYS_NANOSLEEP, (uintptr_t)&req, (uintptr_t)&rem, 0);
        if (g_tail_signal == SIGINT) return 130;
        if (g_tail_signal == SIGTERM) return 0;
        if (ret < 0 && ret != SYSCALL_EINTR) {
            return tool_sys_error("tail", "nanosleep failed", NULL, ret);
        }
    }
}

int tail_main(int argc, char **argv) {
    tool_options_t opts;
    int r = tool_options(TOOL_TAIL, argc, argv, &opts);
    if (r) return r == 1 ? tool_help(TOOL_TAIL) : r;
    return tool_inputs("tail", argc, argv, opts.first, suffix, &opts);
}
