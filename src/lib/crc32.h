#ifndef FORTRESS_CRC32_H
#define FORTRESS_CRC32_H

#include "types.h"

/* Compute IEEE 802.3 32-bit Cyclic Redundancy Check (CRC-32) */
uint32_t crc32(uint32_t init, const void *buf, size_t len);

/* Self-test against IEEE 802.3 reference vector ("123456789" -> 0xCBF43926) */
bool crc32_selftest(void);

#endif /* FORTRESS_CRC32_H */
