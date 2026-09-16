#include "crc32.h"
#include "string.h"

static uint32_t g_crc32_table[256];
static bool     g_table_initialized = false;

static void crc32_init_table(void) {
    if (g_table_initialized) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) {
                c = 0xEDB88320U ^ (c >> 1);
            } else {
                c >>= 1;
            }
        }
        g_crc32_table[i] = c;
    }
    g_table_initialized = true;
}

uint32_t crc32(uint32_t init, const void *buf, size_t len) {
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = init ^ 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        c = g_crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

bool crc32_selftest(void) {
    const char *test_str = "123456789";
    uint32_t computed = crc32(0, test_str, 9);
    return (computed == 0xCBF43926U);
}
