#include "common.h"
#include "syscall_abi.h"

#define LOCKSTAT_BUF_SIZE 8192

static char s_raw_buf[LOCKSTAT_BUF_SIZE];

typedef struct {
    char name[32];
    uint32_t rank;
    uint32_t kind;
    uint64_t acquires;
    uint64_t contentions;
    uint64_t max_spin;
} lock_entry_t;

static lock_entry_t s_entries[64];
static uint32_t s_entry_count = 0;

static void out_str(const char *s) {
    if (!s) return;
    tool_write("lockstat", s, tool_length(s));
}

static void out_u64(uint64_t val) {
    char buf[32];
    tool_format_u64(buf, val);
    out_str(buf);
}

static void copy_str(char *dest, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void out_pad(size_t cur_len, size_t target_width) {
    static const char spaces[] = "                                        ";
    if (cur_len < target_width) {
        size_t pad = target_width - cur_len;
        if (pad > sizeof(spaces) - 1) pad = sizeof(spaces) - 1;
        tool_write("lockstat", spaces, pad);
    }
}

static bool parse_u64(const char *s, size_t *pos, uint64_t *out) {
    uint64_t v = 0;
    size_t i = *pos;
    if (s[i] < '0' || s[i] > '9') return false;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (uint64_t)(s[i] - '0');
        i++;
    }
    *pos = i;
    *out = v;
    return true;
}

static bool parse_field(const char *line, const char *key, char *out_val, size_t max_val) {
    size_t klen = tool_length(key);
    const char *p = line;
    while (*p) {
        bool match = true;
        for (size_t i = 0; i < klen; i++) {
            if (p[i] != key[i]) {
                match = false;
                break;
            }
        }
        if (match && p[klen] == '=') {
            p += klen + 1;
            size_t idx = 0;
            while (*p && *p != ' ' && *p != '\n' && *p != '\r' && idx + 1 < max_val) {
                out_val[idx++] = *p++;
            }
            out_val[idx] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

static void parse_raw_buffer(const char *buf, size_t len) {
    s_entry_count = 0;
    size_t start = 0;
    while (start < len && s_entry_count < 64) {
        size_t end = start;
        while (end < len && buf[end] != '\n') end++;

        // Line is buf[start..end-1]
        char line[256];
        size_t l_len = end - start;
        if (l_len >= sizeof(line)) l_len = sizeof(line) - 1;
        for (size_t i = 0; i < l_len; i++) line[i] = buf[start + i];
        line[l_len] = '\0';

        char field[32];
        if (parse_field(line, "name", field, sizeof(field))) {
            lock_entry_t *e = &s_entries[s_entry_count];
            copy_str(e->name, field, sizeof(e->name));
            e->rank = 0;
            e->kind = 0;
            e->acquires = 0;
            e->contentions = 0;
            e->max_spin = 0;

            if (parse_field(line, "rank", field, sizeof(field))) {
                size_t p = 0; uint64_t v;
                if (parse_u64(field, &p, &v)) e->rank = (uint32_t)v;
            }
            if (parse_field(line, "kind", field, sizeof(field))) {
                size_t p = 0; uint64_t v;
                if (parse_u64(field, &p, &v)) e->kind = (uint32_t)v;
            }
            if (parse_field(line, "acquires", field, sizeof(field))) {
                size_t p = 0; uint64_t v;
                if (parse_u64(field, &p, &v)) e->acquires = v;
            }
            if (parse_field(line, "contentions", field, sizeof(field))) {
                size_t p = 0; uint64_t v;
                if (parse_u64(field, &p, &v)) e->contentions = v;
            }
            if (parse_field(line, "max_spin", field, sizeof(field))) {
                size_t p = 0; uint64_t v;
                if (parse_u64(field, &p, &v)) e->max_spin = v;
            }
            s_entry_count++;
        }

        start = end + 1;
    }
}

int lockstat_main(int argc, char **argv) {
    bool compare_mode = false;

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "-c") || tool_equal(argv[i], "--compare")) {
            compare_mode = true;
        } else if (tool_equal(argv[i], "-h") || tool_equal(argv[i], "--help")) {
            out_str("Usage: lockstat [-c]\n");
            out_str("Dumps kernel spinlock acquisition and contention counters.\n");
            out_str("  -c, --compare    Machine-parseable output\n");
            return 0;
        } else {
            tool_error("lockstat", "unrecognized option", argv[i]);
            return 2;
        }
    }

    long n = tool_syscall(SYS_LOCKSTAT, (uintptr_t)s_raw_buf, sizeof(s_raw_buf) - 1, 0);
    if (n < 0) {
        tool_error("lockstat", "kernel lock statistics unavailable", NULL);
        return 1;
    }
    if (n >= 0 && (size_t)n < sizeof(s_raw_buf)) {
        s_raw_buf[n] = '\0';
    }

    parse_raw_buffer(s_raw_buf, (size_t)n);

    if (compare_mode) {
        for (uint32_t i = 0; i < s_entry_count; i++) {
            lock_entry_t *e = &s_entries[i];
            out_str("lockstat name=");
            out_str(e->name);
            out_str(" rank=");
            out_u64((uint64_t)e->rank);
            out_str(" kind=");
            out_u64((uint64_t)e->kind);
            out_str(" acquires=");
            out_u64(e->acquires);
            out_str(" contentions=");
            out_u64(e->contentions);
            out_str(" max_spin=");
            out_u64(e->max_spin);
            out_str("\n");
        }
    } else {
        out_str("Kernel Spinlock Statistics (lockstat rev=1)\n");
        out_str("------------------------------------------------------------------------------\n");
        out_str("NAME                 RANK  KIND      ACQUIRES   CONTENTIONS      MAX_SPIN\n");
        out_str("------------------------------------------------------------------------------\n");
        for (uint32_t i = 0; i < s_entry_count; i++) {
            lock_entry_t *e = &s_entries[i];
            out_str(e->name);
            out_pad(tool_length(e->name), 20);

            out_u64((uint64_t)e->rank);
            out_pad(1, 6);

            out_u64((uint64_t)e->kind);
            out_pad(1, 10);

            char abuf[32];
            size_t alen = tool_format_u64(abuf, e->acquires);
            out_str(abuf);
            out_pad(alen, 14);

            char cbuf[32];
            size_t clen = tool_format_u64(cbuf, e->contentions);
            out_str(cbuf);
            out_pad(clen, 14);

            out_u64(e->max_spin);
            out_str("\n");
        }
        out_str("------------------------------------------------------------------------------\n");
        out_str("Total tracked locks: ");
        out_u64((uint64_t)s_entry_count);
        out_str("\n");
    }

    return 0;
}
