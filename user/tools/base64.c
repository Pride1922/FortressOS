#include "common.h"

/* Static I/O buffers adhering strictly to Gate G1 (<= 1024B stack frame) */
static uint8_t s_in_buf[TOOL_BUFFER_SIZE];
static uint8_t s_out_buf[TOOL_BUFFER_SIZE];
static size_t s_out_pos = 0;

static const char s_b64_table[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_char_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int flush_out(void) {
    if (s_out_pos > 0) {
        int r = tool_write("base64", s_out_buf, s_out_pos);
        s_out_pos = 0;
        if (r) return r;
    }
    return 0;
}

static int put_out_byte(uint8_t b) {
    s_out_buf[s_out_pos++] = b;
    if (s_out_pos >= sizeof(s_out_buf)) {
        return flush_out();
    }
    return 0;
}

/* =========================================================================
 * Base64 Encoder
 * ========================================================================= */

static int enc_put_char(char c, uint32_t *col, uint32_t wrap) {
    if (wrap > 0 && *col >= wrap) {
        int r = put_out_byte('\n');
        if (r) return r;
        *col = 0;
    }
    int r = put_out_byte((uint8_t)c);
    if (r) return r;
    (*col)++;
    return 0;
}

static int encode_stream(int fd, uint32_t wrap) {
    uint8_t carry[3];
    size_t carry_len = 0;
    uint32_t col = 0;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)s_in_buf, sizeof(s_in_buf));
        if (n < 0) return tool_error("base64", "read error", NULL);
        if (n == 0) break;

        size_t in_pos = 0;
        size_t in_len = (size_t)n;

        /* If we have carryover from previous chunk, try to complete a 3-byte quantum */
        while (carry_len > 0 && carry_len < 3 && in_pos < in_len) {
            carry[carry_len++] = s_in_buf[in_pos++];
            if (carry_len == 3) {
                uint32_t val = ((uint32_t)carry[0] << 16) |
                               ((uint32_t)carry[1] << 8) |
                               (uint32_t)carry[2];
                int r;
                r = enc_put_char(s_b64_table[(val >> 18) & 63], &col, wrap); if (r) return r;
                r = enc_put_char(s_b64_table[(val >> 12) & 63], &col, wrap); if (r) return r;
                r = enc_put_char(s_b64_table[(val >> 6) & 63], &col, wrap);  if (r) return r;
                r = enc_put_char(s_b64_table[val & 63], &col, wrap);         if (r) return r;
                carry_len = 0;
            }
        }

        /* Fast loop over complete 3-byte quanta in s_in_buf */
        while (in_pos + 3 <= in_len) {
            uint32_t val = ((uint32_t)s_in_buf[in_pos] << 16) |
                           ((uint32_t)s_in_buf[in_pos + 1] << 8) |
                           (uint32_t)s_in_buf[in_pos + 2];
            in_pos += 3;

            int r;
            r = enc_put_char(s_b64_table[(val >> 18) & 63], &col, wrap); if (r) return r;
            r = enc_put_char(s_b64_table[(val >> 12) & 63], &col, wrap); if (r) return r;
            r = enc_put_char(s_b64_table[(val >> 6) & 63], &col, wrap);  if (r) return r;
            r = enc_put_char(s_b64_table[val & 63], &col, wrap);         if (r) return r;
        }

        /* Save remaining 1 or 2 bytes into carry */
        while (in_pos < in_len) {
            carry[carry_len++] = s_in_buf[in_pos++];
        }
    }

    /* Process remaining leftover bytes at EOF */
    if (carry_len == 1) {
        uint32_t val = (uint32_t)carry[0] << 16;
        int r;
        r = enc_put_char(s_b64_table[(val >> 18) & 63], &col, wrap); if (r) return r;
        r = enc_put_char(s_b64_table[(val >> 12) & 63], &col, wrap); if (r) return r;
        r = enc_put_char('=', &col, wrap); if (r) return r;
        r = enc_put_char('=', &col, wrap); if (r) return r;
    } else if (carry_len == 2) {
        uint32_t val = ((uint32_t)carry[0] << 16) | ((uint32_t)carry[1] << 8);
        int r;
        r = enc_put_char(s_b64_table[(val >> 18) & 63], &col, wrap); if (r) return r;
        r = enc_put_char(s_b64_table[(val >> 12) & 63], &col, wrap); if (r) return r;
        r = enc_put_char(s_b64_table[(val >> 6) & 63], &col, wrap);  if (r) return r;
        r = enc_put_char('=', &col, wrap); if (r) return r;
    }

    /* Wrap newline if output was produced and wrapping is enabled */
    if (wrap > 0 && col > 0) {
        int r = put_out_byte('\n');
        if (r) return r;
    }

    return flush_out();
}

/* =========================================================================
 * Base64 Decoder
 * ========================================================================= */

static int decode_stream(int fd, bool ignore_garbage) {
    uint8_t carry[4];
    size_t carry_len = 0;

    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)s_in_buf, sizeof(s_in_buf));
        if (n < 0) return tool_error("base64", "read error", NULL);
        if (n == 0) break;

        size_t in_len = (size_t)n;
        for (size_t i = 0; i < in_len; i++) {
            uint8_t c = s_in_buf[i];

            /* Newlines are always ignored */
            if (c == '\n' || c == '\r') {
                continue;
            }

            int val = b64_char_value(c);
            bool is_pad = (c == '=');

            if (val < 0 && !is_pad) {
                if (ignore_garbage) {
                    continue;
                }
                return tool_error("base64", "invalid input", NULL);
            }

            carry[carry_len++] = c;
            if (carry_len == 4) {
                uint8_t c0 = carry[0];
                uint8_t c1 = carry[1];
                uint8_t c2 = carry[2];
                uint8_t c3 = carry[3];

                int d0 = b64_char_value(c0);
                int d1 = b64_char_value(c1);
                int d2 = b64_char_value(c2);
                int d3 = b64_char_value(c3);

                /* c0 and c1 can never be padding */
                if (d0 < 0 || d1 < 0) {
                    return tool_error("base64", "invalid input", NULL);
                }

                if (c2 == '=') {
                    /* Double padding: c3 must also be '=' */
                    if (c3 != '=') {
                        return tool_error("base64", "invalid input", NULL);
                    }
                    /* Lower 4 bits of d1 must be 0 */
                    if ((d1 & 0x0F) != 0) {
                        return tool_error("base64", "invalid input", NULL);
                    }
                    int r = put_out_byte((uint8_t)((d0 << 2) | (d1 >> 4)));
                    if (r) return r;
                } else if (c3 == '=') {
                    /* Single padding: c2 is valid digit */
                    if (d2 < 0) {
                        return tool_error("base64", "invalid input", NULL);
                    }
                    /* Lower 2 bits of d2 must be 0 */
                    if ((d2 & 0x03) != 0) {
                        return tool_error("base64", "invalid input", NULL);
                    }
                    int r = put_out_byte((uint8_t)((d0 << 2) | (d1 >> 4)));
                    if (r) return r;
                    r = put_out_byte((uint8_t)(((d1 & 0x0F) << 4) | (d2 >> 2)));
                    if (r) return r;
                } else {
                    /* Full 4-digit quantum */
                    if (d2 < 0 || d3 < 0) {
                        return tool_error("base64", "invalid input", NULL);
                    }
                    int r = put_out_byte((uint8_t)((d0 << 2) | (d1 >> 4)));
                    if (r) return r;
                    r = put_out_byte((uint8_t)(((d1 & 0x0F) << 4) | (d2 >> 2)));
                    if (r) return r;
                    r = put_out_byte((uint8_t)(((d2 & 0x03) << 6) | d3));
                    if (r) return r;
                }

                carry_len = 0;
            }
        }
    }

    if (carry_len != 0) {
        return tool_error("base64", "invalid input", NULL);
    }

    return flush_out();
}

/* =========================================================================
 * CLI Entry Point & Option Parser
 * ========================================================================= */

static int print_usage(void) {
    const char *help =
        "Usage: base64 [OPTION]... [FILE]\n"
        "Encode or decode FILE, or standard input, to standard output.\n\n"
        "  -d, --decode          decode data\n"
        "  -i, --ignore-garbage  when decoding, ignore non-alphabet characters\n"
        "  -w, --wrap=COLS       wrap encoded lines after COLS character (default 76,\n"
        "                          0 to disable wrapping)\n"
        "  -h, --help            display this help and exit\n\n"
        "With no FILE, or when FILE is -, read standard input.\n";
    return tool_write("base64", help, tool_length(help));
}

static bool parse_uint32(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > UINT32_MAX) return false;
        s++;
    }
    *out = (uint32_t)v;
    return true;
}

int base64_main(int argc, char **argv) {
    s_out_pos = 0;
    bool decode = false;
    bool ignore_garbage = false;
    uint32_t wrap = 76;
    const char *file_arg = NULL;

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--")) {
            i++;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            if (tool_equal(arg, "-h") || tool_equal(arg, "--help")) {
                print_usage();
                return 0;
            }
            if (tool_equal(arg, "-d") || tool_equal(arg, "--decode")) {
                decode = true;
                continue;
            }
            if (tool_equal(arg, "-i") || tool_equal(arg, "--ignore-garbage")) {
                ignore_garbage = true;
                continue;
            }
            if (tool_equal(arg, "-w")) {
                if (++i >= argc) {
                    tool_error("base64", "option requires an argument -- 'w'", NULL);
                    return 1;
                }
                if (!parse_uint32(argv[i], &wrap)) {
                    tool_error("base64", "invalid wrap size", argv[i]);
                    return 1;
                }
                continue;
            }
            if (arg[1] == 'w' && arg[2] != '\0') {
                if (!parse_uint32(arg + 2, &wrap)) {
                    tool_error("base64", "invalid wrap size", arg + 2);
                    return 1;
                }
                continue;
            }
            if (arg[1] == '-' && arg[2] == 'w' && arg[3] == 'r' && arg[4] == 'a' && arg[5] == 'p' && arg[6] == '=') {
                if (!parse_uint32(arg + 7, &wrap)) {
                    tool_error("base64", "invalid wrap size", arg + 7);
                    return 1;
                }
                continue;
            }
            /* Combined short flags like -di or -id */
            bool handled = true;
            for (size_t k = 1; arg[k]; k++) {
                if (arg[k] == 'd') decode = true;
                else if (arg[k] == 'i') ignore_garbage = true;
                else {
                    handled = false;
                    break;
                }
            }
            if (handled) continue;

            tool_error("base64", "unrecognized option", arg);
            return 1;
        }

        if (!file_arg) {
            file_arg = arg;
        } else {
            tool_error("base64", "extra operand", arg);
            return 1;
        }
    }

    for (; i < argc; i++) {
        if (!file_arg) {
            file_arg = argv[i];
        } else {
            tool_error("base64", "extra operand", argv[i]);
            return 1;
        }
    }

    int fd = 0;
    bool is_stdin = (!file_arg || tool_equal(file_arg, "-"));
    if (!is_stdin) {
        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)file_arg, (uintptr_t)&st, 0) < 0) {
            return tool_error("base64", "cannot open", file_arg);
        }
        long opened = tool_syscall(SYS_OPEN, (uintptr_t)file_arg, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
        if (opened < 0) {
            return tool_error("base64", "cannot open", file_arg);
        }
        fd = (int)opened;
    }

    int ret;
    if (decode) {
        ret = decode_stream(fd, ignore_garbage);
    } else {
        ret = encode_stream(fd, wrap);
    }

    if (!is_stdin) {
        tool_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    }

    return ret;
}
