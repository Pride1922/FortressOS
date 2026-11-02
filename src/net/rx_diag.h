#ifndef FORTRESS_NET_RX_DIAG_H
#define FORTRESS_NET_RX_DIAG_H
#include "types.h"

/* Boot-cumulative, individually atomic samples. Writers remain the existing
 * CPU-0 owner; readers may run on any CPU. No reset, radio access or logging
 * from the packet path. Values contain counts/status metadata, never keys or
 * packet contents. Formatting publishes only complete newline-ended rows. */
static inline void rx_diag_inc(uint64_t *value) {
    __atomic_fetch_add(value, 1, __ATOMIC_RELAXED);
}
static inline void rx_diag_set(uint64_t *value, uint64_t sample) {
    __atomic_store_n(value, sample, __ATOMIC_RELAXED);
}
static inline size_t rx_diag_format(char *out, size_t cap, const char *prefix,
                                   const char *const *names,
                                   const uint64_t *values, size_t count) {
    size_t n = 0;
    for (const char *p = prefix; *p; ++p) {
        if (n >= cap) return 0;
        out[n++] = *p;
    }
    for (size_t i = 0; i < count; ++i) {
        if (n >= cap) return 0;
        out[n++] = ' ';
        for (const char *p = names[i]; *p; ++p) {
            if (n >= cap) return 0;
            out[n++] = *p;
        }
        if (n >= cap) return 0;
        out[n++] = '=';
        char digits[20];
        size_t d = 0;
        uint64_t value = __atomic_load_n(&values[i], __ATOMIC_RELAXED);
        do { digits[d++] = (char)('0' + value % 10); value /= 10; } while (value);
        if (d > cap - n) return 0;
        while (d) out[n++] = digits[--d];
    }
    if (n >= cap) return 0;
    out[n++] = '\n';
    return n;
}
#endif
