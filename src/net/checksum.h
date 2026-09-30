#ifndef FORTRESS_NET_CHECKSUM_H
#define FORTRESS_NET_CHECKSUM_H

#include "types.h"

/*
 * RFC 1071 Internet Checksum
 *
 * Computes 16-bit ones' complement sum of 16-bit words.
 * Supports multi-buffer accumulation (e.g. IPv4 pseudo-header + UDP payload).
 */

/* Accumulate 16-bit words into 32-bit accumulator. Odd-length buffers pad last byte. */
uint32_t net_checksum_accumulate(const void *data, size_t len, uint32_t initial);

/* Fold 32-bit accumulator into 16 bits and invert (returns final checksum). */
uint16_t net_checksum_finish(uint32_t acc);

/* Compute complete ones' complement checksum in a single call. */
uint16_t net_checksum(const void *data, size_t len, uint32_t initial);

#endif /* FORTRESS_NET_CHECKSUM_H */
