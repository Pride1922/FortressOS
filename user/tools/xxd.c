#include "common.h"

#define XXD_MAX_COLS 256

typedef struct {
    uint32_t cols;
    bool cols_specified;
    uint32_t groupsize;
    uint64_t length;
    bool has_length;
    uint64_t seek;
    bool has_seek;
    bool plain;
    bool revert;
    bool uppercase;
    const char *input_file;
    const char *output_file;
} xxd_options_t;

static unsigned char s_row_buf[XXD_MAX_COLS];
static char s_line_buf[2048];

static unsigned char s_revert_out[TOOL_BUFFER_SIZE];
static size_t s_revert_out_pos = 0;

static char s_rev_line[4096];
static size_t s_rev_line_len = 0;

static int write_out(int out_fd, const void *data, size_t n) {
    if (out_fd == 1) return tool_write("xxd", data, n);
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        long w = tool_syscall(SYS_WRITE, (uintptr_t)out_fd, (uintptr_t)p, n);
        if (w <= 0) return tool_error("xxd", "write error", NULL);
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    if (*s == '+') s++;
    uint64_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return false;
        while (*s) {
            char c = *s++;
            unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
            else return false;
            if (val > (UINT64_MAX - d) / 16) return false;
            val = (val << 4) | d;
        }
    } else {
        if (!*s) return false;
        while (*s) {
            char c = *s++;
            if (c < '0' || c > '9') return false;
            unsigned int d = (unsigned int)(c - '0');
            if (val > (UINT64_MAX - d) / 10) return false;
            val = val * 10 + d;
        }
    }
    *out = val;
    return true;
}

static int print_help(void) {
    static const char help_text[] =
        "Usage: xxd [OPTIONS] [INFILE [OUTFILE]]\n"
        "Make a hexdump or do the reverse.\n\n"
        "Options:\n"
        "  -c COLS       Format <COLS> octets per line [1..256] (default: 16, plain: 30)\n"
        "  -g BYTES      Number of octets per group in normal mode (default: 2, 0 to disable)\n"
        "  -l LEN        Stop after writing <LEN> octets\n"
        "  -s SEEK       Start at <SEEK> bytes offset (decimal or 0x hex)\n"
        "  -p            Output plain continuous hex dump style\n"
        "  -r            Reverse operation: convert hex dump back to binary\n"
        "  -u            Use uppercase hex digits\n"
        "  --help        Display this help text and exit\n";
    return tool_write("xxd", help_text, sizeof(help_text) - 1);
}

static int parse_xxd_options(int argc, char **argv, xxd_options_t *opts) {
    for (size_t i = 0; i < sizeof(*opts); i++) ((char *)opts)[i] = 0;
    opts->groupsize = 2;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) return 1;
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] != '-' || !arg[1] || tool_equal(arg, "-")) {
            break;
        }

        if (tool_equal(arg, "-p") || tool_equal(arg, "-ps") || tool_equal(arg, "--plain")) {
            opts->plain = true;
            continue;
        }
        if (tool_equal(arg, "-r") || tool_equal(arg, "-revert") || tool_equal(arg, "--revert")) {
            opts->revert = true;
            continue;
        }

        for (size_t j = 1; arg[j]; j++) {
            char opt = arg[j];
            if (opt == 'p') {
                opts->plain = true;
            } else if (opt == 'r') {
                opts->revert = true;
            } else if (opt == 'u') {
                opts->uppercase = true;
            } else if (opt == 'c' || opt == 'g' || opt == 'l' || opt == 's') {
                const char *val_str = NULL;
                if (arg[j + 1] != '\0') {
                    val_str = &arg[j + 1];
                } else if (i + 1 < argc) {
                    val_str = argv[++i];
                } else {
                    tool_error("xxd", "option requires an argument; use --help", NULL);
                    return 2;
                }

                uint64_t val = 0;
                if (!parse_u64(val_str, &val)) {
                    tool_error("xxd", "invalid number for option; use --help", val_str);
                    return 2;
                }

                if (opt == 'c') {
                    if (val < 1 || val > XXD_MAX_COLS) {
                        tool_error("xxd", "invalid column count (must be 1..256)", NULL);
                        return 2;
                    }
                    opts->cols = (uint32_t)val;
                    opts->cols_specified = true;
                } else if (opt == 'g') {
                    opts->groupsize = (uint32_t)val;
                } else if (opt == 'l') {
                    opts->length = val;
                    opts->has_length = true;
                } else if (opt == 's') {
                    opts->seek = val;
                    opts->has_seek = true;
                }
                break; /* consumed the rest of this arg */
            } else {
                tool_error("xxd", "invalid option; use --help", NULL);
                return 2;
            }
        }
    }

    if (!opts->cols_specified) {
        opts->cols = opts->plain ? 30 : 16;
    }
    if (opts->groupsize > opts->cols) {
        opts->groupsize = opts->cols;
    }

    if (i < argc) {
        opts->input_file = argv[i++];
    }
    if (i < argc) {
        opts->output_file = argv[i++];
    }
    if (i < argc) {
        tool_error("xxd", "extra operand; use --help", argv[i]);
        return 2;
    }

    return 0;
}

static size_t format_addr(char *buf, uint64_t addr, const char *hex_digits) {
    char tmp[16];
    size_t digits = 0;
    do {
        tmp[digits++] = hex_digits[addr & 0xf];
        addr >>= 4;
    } while (addr != 0);

    while (digits < 8) {
        tmp[digits++] = '0';
    }

    size_t pos = 0;
    for (size_t i = 0; i < digits; i++) {
        buf[pos++] = tmp[digits - 1 - i];
    }
    buf[pos++] = ':';
    buf[pos++] = ' ';
    return pos;
}

static int emit_forward_line(int out_fd, const xxd_options_t *opts, const char *hex,
                            uint64_t addr, const unsigned char *row, size_t row_len,
                            size_t total_hex_width) {
    size_t pos = 0;
    if (opts->plain) {
        for (size_t i = 0; i < row_len; i++) {
            s_line_buf[pos++] = hex[(row[i] >> 4) & 0xf];
            s_line_buf[pos++] = hex[row[i] & 0xf];
        }
        s_line_buf[pos++] = '\n';
        return write_out(out_fd, s_line_buf, pos);
    }

    pos = format_addr(s_line_buf, addr, hex);

    size_t hex_start = pos;
    for (size_t i = 0; i < row_len; i++) {
        s_line_buf[pos++] = hex[(row[i] >> 4) & 0xf];
        s_line_buf[pos++] = hex[row[i] & 0xf];
        if (opts->groupsize > 0 && ((i + 1) % opts->groupsize == 0) && (i < opts->cols - 1)) {
            s_line_buf[pos++] = ' ';
        }
    }

    size_t hex_written = pos - hex_start;
    size_t pad_needed = (total_hex_width > hex_written ? total_hex_width - hex_written : 0) + 2;
    for (size_t i = 0; i < pad_needed; i++) {
        s_line_buf[pos++] = ' ';
    }

    for (size_t i = 0; i < row_len; i++) {
        unsigned char b = row[i];
        s_line_buf[pos++] = (b >= 0x20 && b <= 0x7e) ? (char)b : '.';
    }
    s_line_buf[pos++] = '\n';

    return write_out(out_fd, s_line_buf, pos);
}

static int run_forward_dump(int in_fd, int out_fd, const xxd_options_t *opts) {
    const char *hex = opts->uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t current_addr = opts->has_seek ? opts->seek : 0;
    uint64_t remaining = opts->has_length ? opts->length : UINT64_MAX;
    size_t row_len = 0;
    size_t cols = opts->cols;
    size_t groupsize = opts->groupsize;

    size_t total_hex_width = 2 * cols + (groupsize > 0 ? (cols - 1) / groupsize : 0);

    while (remaining > 0) {
        size_t want = TOOL_BUFFER_SIZE;
        if (opts->has_length && (uint64_t)want > remaining) {
            want = (size_t)remaining;
        }
        long n = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)tool_buffer, want);
        if (n < 0) {
            return tool_error("xxd", "read error", opts->input_file);
        }
        if (n == 0) {
            break;
        }

        for (size_t i = 0; i < (size_t)n; i++) {
            s_row_buf[row_len++] = tool_buffer[i];
            remaining--;

            if (row_len == cols) {
                int r = emit_forward_line(out_fd, opts, hex, current_addr, s_row_buf, row_len, total_hex_width);
                if (r) return r;
                current_addr += row_len;
                row_len = 0;
            }

            if (remaining == 0) break;
        }
    }

    if (row_len > 0) {
        int r = emit_forward_line(out_fd, opts, hex, current_addr, s_row_buf, row_len, total_hex_width);
        if (r) return r;
    }

    return 0;
}

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int flush_revert_out(int out_fd) {
    if (s_revert_out_pos == 0) return 0;
    int r = write_out(out_fd, s_revert_out, s_revert_out_pos);
    s_revert_out_pos = 0;
    return r;
}

static inline int emit_revert_byte(int out_fd, unsigned char b) {
    s_revert_out[s_revert_out_pos++] = b;
    if (s_revert_out_pos == TOOL_BUFFER_SIZE) {
        return flush_revert_out(out_fd);
    }
    return 0;
}

static int run_reverse_plain(int in_fd, int out_fd, const xxd_options_t *opts) {
    s_revert_out_pos = 0;
    int high = -1;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("xxd", "read error", opts->input_file);
        if (n == 0) break;

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            int v = hex_val(c);
            if (v < 0) {
                return flush_revert_out(out_fd);
            }
            if (high < 0) {
                high = v;
            } else {
                unsigned char b = (unsigned char)((high << 4) | v);
                int r = emit_revert_byte(out_fd, b);
                if (r) return r;
                high = -1;
            }
        }
    }
    return flush_revert_out(out_fd);
}

static int process_reverse_line(int out_fd, const char *line, size_t len) {
    if (len == 0) return 0;

    size_t colon_pos = 0;
    bool has_colon = false;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == ':') {
            colon_pos = i;
            has_colon = true;
            break;
        }
        if (line[i] == ' ' || line[i] == '\t') continue;
        if (hex_val(line[i]) < 0) {
            break;
        }
    }

    size_t idx = has_colon ? colon_pos + 1 : 0;
    int high = -1;
    size_t consecutive_spaces = 0;

    while (idx < len) {
        char c = line[idx++];
        if (c == ' ' || c == '\t') {
            consecutive_spaces++;
            if (consecutive_spaces >= 2) {
                break;
            }
            continue;
        }

        consecutive_spaces = 0;
        int v = hex_val(c);
        if (v < 0) {
            break;
        }

        if (high < 0) {
            high = v;
        } else {
            unsigned char b = (unsigned char)((high << 4) | v);
            int r = emit_revert_byte(out_fd, b);
            if (r) return r;
            high = -1;
        }
    }

    return 0;
}

static int run_reverse_standard(int in_fd, int out_fd, const xxd_options_t *opts) {
    s_revert_out_pos = 0;
    s_rev_line_len = 0;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("xxd", "read error", opts->input_file);
        if (n == 0) {
            if (s_rev_line_len > 0) {
                int r = process_reverse_line(out_fd, s_rev_line, s_rev_line_len);
                if (r) return r;
                s_rev_line_len = 0;
            }
            break;
        }

        for (size_t i = 0; i < (size_t)n; i++) {
            char c = (char)tool_buffer[i];
            if (c == '\n') {
                int r = process_reverse_line(out_fd, s_rev_line, s_rev_line_len);
                if (r) return r;
                s_rev_line_len = 0;
            } else if (c == '\r') {
                /* Ignore carriage returns */
            } else {
                if (s_rev_line_len + 1 < sizeof(s_rev_line)) {
                    s_rev_line[s_rev_line_len++] = c;
                }
            }
        }
    }

    return flush_revert_out(out_fd);
}

int xxd_main(int argc, char **argv) {
    xxd_options_t opts;
    int opt_res = parse_xxd_options(argc, argv, &opts);
    if (opt_res == 1) return print_help();
    if (opt_res != 0) return opt_res;

    int in_fd = 0;
    bool close_in = false;
    if (opts.input_file && !tool_equal(opts.input_file, "-")) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)opts.input_file, (uintptr_t)&st, 0) < 0) {
            return tool_error("xxd", "cannot open", opts.input_file);
        }
        if (st.type == VFS_DIRECTORY) {
            return tool_error("xxd", "is a directory", opts.input_file);
        }
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.input_file, VFS_O_RDONLY, 0);
        if (fd < 0) {
            return tool_error("xxd", "cannot open", opts.input_file);
        }
        in_fd = (int)fd;
        close_in = true;
    }

    int out_fd = 1;
    bool close_out = false;
    if (opts.output_file && !tool_equal(opts.output_file, "-")) {
        long fd = tool_syscall(SYS_OPEN, (uintptr_t)opts.output_file,
                               VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
        if (fd < 0) {
            if (close_in) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
            return tool_error("xxd", "cannot create", opts.output_file);
        }
        out_fd = (int)fd;
        close_out = true;
    }

    if (opts.has_seek && opts.seek > 0) {
        uint64_t to_skip = opts.seek;
        while (to_skip > 0) {
            size_t chunk = to_skip > TOOL_BUFFER_SIZE ? TOOL_BUFFER_SIZE : (size_t)to_skip;
            long r = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)tool_buffer, chunk);
            if (r < 0) {
                if (close_in) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
                if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);
                return tool_error("xxd", "read error", opts.input_file);
            }
            if (r == 0) {
                break;
            }
            to_skip -= (size_t)r;
        }
    }

    int ret = 0;
    if (opts.revert) {
        if (opts.plain) {
            ret = run_reverse_plain(in_fd, out_fd, &opts);
        } else {
            ret = run_reverse_standard(in_fd, out_fd, &opts);
        }
    } else {
        ret = run_forward_dump(in_fd, out_fd, &opts);
    }

    if (close_in) (void)tool_syscall(SYS_CLOSE, (uintptr_t)in_fd, 0, 0);
    if (close_out) (void)tool_syscall(SYS_CLOSE, (uintptr_t)out_fd, 0, 0);

    return ret;
}
