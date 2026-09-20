#include "dmesg.h"

static char   g_buf[DMESG_SIZE];
static size_t g_head;
static size_t g_total;

void dmesg_append(char c) {
    g_buf[g_head] = c;
    g_head = (g_head + 1) % DMESG_SIZE;
    if (g_total < DMESG_SIZE) g_total++;
}

void dmesg_append_str(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        g_buf[g_head] = s[i];
        g_head = (g_head + 1) % DMESG_SIZE;
        if (g_total < DMESG_SIZE) g_total++;
    }
}

size_t dmesg_read(char *dst, size_t cap) {
    size_t n = g_total < cap ? g_total : cap;
    size_t start = (g_head + DMESG_SIZE - n) % DMESG_SIZE;
    for (size_t i = 0; i < n; i++)
        dst[i] = g_buf[(start + i) % DMESG_SIZE];
    return n;
}