#include "checksum.h"

uint32_t net_checksum_accumulate(const void *data, size_t len, uint32_t initial) {
    if (!data || len == 0) return initial;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = initial;

    while (len >= 2) {
        uint16_t word = ((uint16_t)p[0] << 8) | (uint16_t)p[1];
        sum += word;
        p += 2;
        len -= 2;
    }

    if (len == 1) {
        sum += ((uint16_t)p[0] << 8);
    }

    return sum;
}

uint16_t net_checksum_finish(uint32_t acc) {
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return (uint16_t)~acc;
}

uint16_t net_checksum(const void *data, size_t len, uint32_t initial) {
    uint32_t acc = net_checksum_accumulate(data, len, initial);
    return net_checksum_finish(acc);
}
