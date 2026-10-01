#ifndef FORTRESS_UDP_H
#define FORTRESS_UDP_H
#include "types.h"
#include "socket_abi.h"
typedef struct { uint16_t source, destination, length; } udp_header_t;
/* IPs are network-order; decoded ports/length are host-order. IPv4 only.
 * Payload pointers exclude trailing IP bytes; no unaligned word accesses. */
int udp_decode(const void *buf, size_t len, uint32_t src, uint32_t dst,
               udp_header_t *header, const uint8_t **data, size_t *data_len);
int udp_encode(void *buf, size_t capacity, uint32_t src, uint32_t dst,
               uint16_t sport, uint16_t dport, const void *data, size_t len);
#endif
