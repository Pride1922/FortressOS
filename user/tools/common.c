#include "common.h"

unsigned char tool_buffer[TOOL_BUFFER_SIZE];
bool tool_output_failed;
static const char *const names[] = {"cat", "head", "tail", "wc"};

#ifndef TOOL_HOST_TEST
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif
size_t tool_length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
bool tool_equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static long write_all(int fd, const unsigned char *p, size_t n) {
    while (n) {
        size_t chunk = n > TOOL_BUFFER_SIZE ? TOOL_BUFFER_SIZE : n;
        long w = tool_syscall(SYS_WRITE, fd, (uintptr_t)p, chunk);
        if (w < 0) return w;
        if (!w || (size_t)w > chunk) return SYSCALL_EIO;
        p += w; n -= (size_t)w;
    }
    return 0;
}
static bool diagnostic_part(const char *s) {
    return write_all(2, (const unsigned char *)s, tool_length(s)) >= 0;
}
int tool_error(const char *tool, const char *message, const char *operand) {
    /* A broken diagnostic stream must never trigger another diagnostic. */
    if (!diagnostic_part(tool) || !diagnostic_part(": ") || !diagnostic_part(message)) return 1;
    if (operand && (!diagnostic_part(" ") || !diagnostic_part(operand))) return 1;
    (void)diagnostic_part("\n");
    return 1;
}
int tool_write(const char *tool, const void *data, size_t n) {
    long r = write_all(1, data, n);
    if (r >= 0) return 0;
    tool_output_failed = true;
    if (r == SYSCALL_EPIPE) return 141;
    return tool_error(tool, "write error", NULL);
}
static bool decimal(const char *s, uint64_t *out) {
    uint64_t n = 0;
    if (!*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s++ - '0');
        if (n > (UINT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *out = n;
    return true;
}
int tool_options(enum tool_kind kind, int argc, char **argv, tool_options_t *out) {
    tool_output_failed = false;
    *out = (tool_options_t){.first = 1, .count = 10};
    if (argc == 2 && tool_equal(argv[1], "--help")) return 1;
    bool counted = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--")) { i++; break; }
        if (arg[0] != '-' || !arg[1]) break;
        if (kind == TOOL_WC) {
            for (size_t j = 1; arg[j]; j++) {
                if (arg[j] == 'l') out->fields |= WC_LINES;
                else if (arg[j] == 'w') out->fields |= WC_WORDS;
                else if (arg[j] == 'c') out->fields |= WC_BYTES;
                else goto invalid;
            }
        } else if ((kind == TOOL_HEAD || kind == TOOL_TAIL) && !counted &&
                   (tool_equal(arg, "-n") || tool_equal(arg, "-c"))) {
            out->bytes = arg[1] == 'c';
            if (++i == argc || !decimal(argv[i], &out->count)) goto invalid;
            counted = true;
        } else goto invalid;
    }
    out->first = i;
    if (!out->fields) out->fields = WC_LINES | WC_WORDS | WC_BYTES;
    if (kind == TOOL_TAIL && out->count > (out->bytes ? TAIL_MAX_BYTES : TAIL_MAX_LINES)) {
        tool_error("tail", out->bytes ? "maximum byte count is 65536" : "maximum line count is 10", NULL);
        return 2;
    }
    return 0;
invalid:
    tool_error(names[kind], "invalid arguments; use --help", NULL);
    return 2;
}
int tool_help(enum tool_kind kind) {
    static const char *const help[] = {
        "Usage: cat [--] [FILE ...]\nCopy bytes unchanged; no operands or - reads stdin.\n",
        "Usage: head [-n N | -c N] [--] [FILE ...]\nDefault: 10 lines. Zero reads nothing.\n",
        "Usage: tail [-n N | -c N] [--] [FILE ...]\nDefault: 10 lines; max 10 lines, 4096 content bytes per retained line, or 65536 bytes.\nTail always reads to EOF, including count zero; it cannot stop a producer early.\n",
        "Usage: wc [-lwc] [--] [FILE ...]\nCount LF bytes, ASCII-separated words and bytes; default all three.\n"
    };
    return tool_write(names[kind], help[kind], tool_length(help[kind]));
}
int tool_inputs(const char *name, int argc, char **argv, int first,
                int (*consume)(int, const char *, void *), void *ctx) {
    int result = 0;
    int end = first == argc ? first + 1 : argc;
    for (int i = first; i < end; i++) {
        const char *label = i < argc ? argv[i] : NULL;
        bool owned = label && !tool_equal(label, "-");
        long fd = 0;
        if (owned) {
            vfs_stat_t st;
            if (tool_syscall(SYS_STAT, (uintptr_t)label, (uintptr_t)&st, 0) < 0) {
                result = tool_error(name, "cannot open", label); continue;
            }
            if (st.type == VFS_DIRECTORY) {
                result = tool_error(name, "is a directory", label); continue;
            }
            fd = tool_syscall(SYS_OPEN, (uintptr_t)label, VFS_O_RDONLY, 0);
            if (fd < 0) { result = tool_error(name, "cannot open", label); continue; }
        }
        int status = consume((int)fd, label, ctx);
        if (owned && tool_syscall(SYS_CLOSE, fd, 0, 0) < 0) {
            if (!status) status = tool_error(name, "close error", label);
        }
        if (status) result = status;
        if (tool_output_failed) break;
    }
    return result;
}
