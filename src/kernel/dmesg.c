#include "dmesg.h"
#include "serial.h"
#include "console.h"

static char   g_buf[DMESG_SIZE];
static size_t g_head;
static size_t g_total;
static bool   g_at_line_start = true;

static inline void raw_append(char c) {
    g_buf[g_head] = c;
    g_head = (g_head + 1) % DMESG_SIZE;
    if (g_total < DMESG_SIZE) g_total++;
}

void dmesg_append_entry(uint8_t level, const char *s, size_t n) {
    if (!s || n == 0) return;
    if (level > 7) level = KLOG_INFO;
    if (g_at_line_start) {
        raw_append('<');
        raw_append((char)('0' + level));
        raw_append('>');
        g_at_line_start = false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        raw_append(c);
        if (c == '\n') {
            g_at_line_start = true;
            if (i + 1 < n) {
                raw_append('<');
                raw_append((char)('0' + level));
                raw_append('>');
                g_at_line_start = false;
            }
        }
    }
}

void dmesg_append(char c) {
    if (g_at_line_start) {
        raw_append('<');
        raw_append((char)('0' + KLOG_INFO));
        raw_append('>');
        g_at_line_start = false;
    }
    raw_append(c);
    if (c == '\n') {
        g_at_line_start = true;
    }
}

void dmesg_append_str(const char *s, size_t n) {
    if (!s || n == 0) return;
    size_t i = 0;
    while (i < n) {
        if (g_at_line_start) {
            uint8_t lvl = KLOG_INFO;
            if (i + 2 < n && s[i] == '<' && s[i + 1] >= '0' && s[i + 1] <= '7' && s[i + 2] == '>') {
                lvl = (uint8_t)(s[i + 1] - '0');
                i += 3;
            }
            raw_append('<');
            raw_append((char)('0' + lvl));
            raw_append('>');
            g_at_line_start = false;
            if (i >= n) break;
        }
        char c = s[i++];
        raw_append(c);
        if (c == '\n') {
            g_at_line_start = true;
        }
    }
}

size_t dmesg_read(char *dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    size_t n = g_total < cap ? g_total : cap;
    size_t start = (g_head + DMESG_SIZE - n) % DMESG_SIZE;
    /* If the buffer wrapped and we start mid-line, skip to start of next line */
    if (g_total == DMESG_SIZE && n == cap) {
        size_t skipped = 0;
        while (skipped < n && g_buf[start] != '\n') {
            start = (start + 1) % DMESG_SIZE;
            skipped++;
        }
        if (skipped < n && g_buf[start] == '\n') {
            start = (start + 1) % DMESG_SIZE;
            skipped++;
        }
        n -= skipped;
    }
    for (size_t i = 0; i < n; i++)
        dst[i] = g_buf[(start + i) % DMESG_SIZE];
    return n;
}

void klog_write(uint8_t level, const char *str, size_t len) {
    if (!str || len == 0) return;
    dmesg_append_entry(level, str, len);
    if (console_is_initialized() && !console_is_quiet()) {
        for (size_t i = 0; i < len; i++) console_putc(str[i]);
    }
    for (size_t i = 0; i < len; i++) {
        serial_raw_putc(str[i]);
    }
}

void klog(uint8_t level, const char *str) {
    if (!str) return;
    size_t len = 0;
    while (str[len]) len++;
    klog_write(level, str, len);
}