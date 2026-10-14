#include "common.h"

enum free_unit {
    UNIT_HUMAN,
    UNIT_BYTES,
    UNIT_KIBI,
    UNIT_MEBI,
    UNIT_GIBI
};

static char s_line[256];
static char s_val[32];

static void format_human(char *buf, uint64_t bytes) {
    if (bytes == 0) {
        buf[0] = '0';
        buf[1] = 'B';
        buf[2] = '\0';
        return;
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        uint64_t gib = bytes / (1024ULL * 1024ULL * 1024ULL);
        uint64_t rem = bytes % (1024ULL * 1024ULL * 1024ULL);
        if (gib < 10) {
            uint64_t tenths = (rem * 10ULL + (512ULL * 1024ULL * 1024ULL)) / (1024ULL * 1024ULL * 1024ULL);
            if (tenths >= 10) {
                gib++;
                tenths = 0;
            }
            size_t n = tool_format_u64(buf, gib);
            buf[n++] = '.';
            buf[n++] = (char)('0' + tenths);
            buf[n++] = 'G';
            buf[n++] = 'i';
            buf[n] = '\0';
        } else {
            size_t n = tool_format_u64(buf, gib);
            buf[n++] = 'G';
            buf[n++] = 'i';
            buf[n] = '\0';
        }
    } else if (bytes >= 1024ULL * 1024ULL) {
        uint64_t mib = bytes / (1024ULL * 1024ULL);
        uint64_t rem = bytes % (1024ULL * 1024ULL);
        if (mib < 10) {
            uint64_t tenths = (rem * 10ULL + (512ULL * 1024ULL)) / (1024ULL * 1024ULL);
            if (tenths >= 10) {
                mib++;
                tenths = 0;
            }
            size_t n = tool_format_u64(buf, mib);
            buf[n++] = '.';
            buf[n++] = (char)('0' + tenths);
            buf[n++] = 'M';
            buf[n++] = 'i';
            buf[n] = '\0';
        } else {
            size_t n = tool_format_u64(buf, mib);
            buf[n++] = 'M';
            buf[n++] = 'i';
            buf[n] = '\0';
        }
    } else if (bytes >= 1024ULL) {
        uint64_t kib = bytes / 1024ULL;
        uint64_t rem = bytes % 1024ULL;
        if (kib < 10) {
            uint64_t tenths = (rem * 10ULL + 512ULL) / 1024ULL;
            if (tenths >= 10) {
                kib++;
                tenths = 0;
            }
            size_t n = tool_format_u64(buf, kib);
            buf[n++] = '.';
            buf[n++] = (char)('0' + tenths);
            buf[n++] = 'K';
            buf[n++] = 'i';
            buf[n] = '\0';
        } else {
            size_t n = tool_format_u64(buf, kib);
            buf[n++] = 'K';
            buf[n++] = 'i';
            buf[n] = '\0';
        }
    } else {
        size_t n = tool_format_u64(buf, bytes);
        buf[n++] = 'B';
        buf[n] = '\0';
    }
}

static void format_value(char *buf, uint64_t bytes, enum free_unit unit) {
    if (unit == UNIT_HUMAN) {
        format_human(buf, bytes);
    } else {
        uint64_t val = bytes;
        if (unit == UNIT_KIBI) val = bytes / 1024ULL;
        else if (unit == UNIT_MEBI) val = bytes / (1024ULL * 1024ULL);
        else if (unit == UNIT_GIBI) val = bytes / (1024ULL * 1024ULL * 1024ULL);
        (void)tool_format_u64(buf, val);
    }
}

static void append_col(char *line, size_t *pos, size_t max, const char *val, int width) {
    size_t len = tool_length(val);
    int pad = width - (int)len;
    while (pad > 0 && *pos + 1 < max) {
        line[(*pos)++] = ' ';
        pad--;
    }
    for (size_t i = 0; i < len && *pos + 1 < max; i++) {
        line[(*pos)++] = val[i];
    }
    line[*pos] = '\0';
}

int free_main(int argc, char **argv) {
    enum free_unit unit = UNIT_HUMAN;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help")) {
            static const char help_msg[] =
                "usage: free [-b|-k|-m|-g|-h] [--help]\n\n"
                "Options:\n"
                "  -b, --bytes  show output in bytes\n"
                "  -k, --kibi   show output in kibibytes\n"
                "  -m, --mebi   show output in mebibytes\n"
                "  -g, --gibi   show output in gibibytes\n"
                "  -h, --human  show human-readable output (default)\n"
                "      --help   display this help and exit\n";
            return tool_write("free", help_msg, sizeof(help_msg) - 1);
        } else if (tool_equal(arg, "-b") || tool_equal(arg, "--bytes")) {
            unit = UNIT_BYTES;
        } else if (tool_equal(arg, "-k") || tool_equal(arg, "--kibi")) {
            unit = UNIT_KIBI;
        } else if (tool_equal(arg, "-m") || tool_equal(arg, "--mebi")) {
            unit = UNIT_MEBI;
        } else if (tool_equal(arg, "-g") || tool_equal(arg, "--gibi")) {
            unit = UNIT_GIBI;
        } else if (tool_equal(arg, "-h") || tool_equal(arg, "--human")) {
            unit = UNIT_HUMAN;
        } else {
            return tool_error("free", "invalid option", arg);
        }
    }

    sysinfo_t info;
    if (tool_syscall(SYS_SYSINFO, (uintptr_t)&info, 0, 0) < 0) {
        return tool_error("free", "cannot read system info", NULL);
    }

    uint64_t total = info.total_ram_bytes;
    uint64_t free_ram = info.free_ram_bytes;
    uint64_t used = (total >= free_ram) ? (total - free_ram) : 0;
    uint64_t shared = 0;
    uint64_t buff_cache = 0;
    uint64_t available = free_ram;

    /* 1. Header line */
    static const char header[] =
        "               total        used        free      shared  buff/cache   available\n";
    if (tool_write("free", header, sizeof(header) - 1)) return 1;

    /* 2. Mem line */
    size_t pos = 0;
    const char *mem_pfx = "Mem:    ";
    while (*mem_pfx && pos + 1 < sizeof(s_line)) s_line[pos++] = *mem_pfx++;
    s_line[pos] = '\0';

    format_value(s_val, total, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, used, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, free_ram, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, shared, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, buff_cache, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, available, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    if (pos + 2 < sizeof(s_line)) {
        s_line[pos++] = '\n';
        s_line[pos] = '\0';
    }
    if (tool_write("free", s_line, pos)) return 1;

    /* 3. Swap line */
    pos = 0;
    const char *swap_pfx = "Swap:   ";
    while (*swap_pfx && pos + 1 < sizeof(s_line)) s_line[pos++] = *swap_pfx++;
    s_line[pos] = '\0';

    format_value(s_val, 0, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, 0, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    format_value(s_val, 0, unit);
    append_col(s_line, &pos, sizeof(s_line), s_val, 12);
    if (pos + 2 < sizeof(s_line)) {
        s_line[pos++] = '\n';
        s_line[pos] = '\0';
    }
    if (tool_write("free", s_line, pos)) return 1;

    return 0;
}
