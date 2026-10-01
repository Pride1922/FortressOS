#ifndef FORTRESS_TCP_H
#define FORTRESS_TCP_H
#include "types.h"

#define TCP_HEADER_MIN 20U
#define TCP_HEADER_MAX 60U
#define TCP_IPV4_SEGMENT_MAX 65515U /* IPv4 minimum header already excluded. */
#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_URG 0x20U
#define TCP_ECE 0x40U
#define TCP_CWR 0x80U

typedef struct {
    uint16_t source, destination;
    uint32_t sequence, acknowledgment;
    uint16_t window, urgent;
    uint8_t flags, header_length;
    bool has_mss, has_wscale;
    uint16_t mss;
    uint8_t wscale; /* Parsed/clamped to 14; first transport will not negotiate. */
} tcp_header_t;

/* Pure IPv4 codec: addresses network-order, fields host-order. Exact bounded
 * IPv4 payload length, excluding Ethernet padding, is required. Checksum is
 * mandatory. Unaligned buffers supported; failures leave outputs untouched.
 * Payload view borrows input lifetime. No option negotiation/state validation. */
int tcp_decode(const void *buf, size_t len, uint32_t source, uint32_t destination,
               tcp_header_t *header, const uint8_t **data, size_t *data_len);
/* Emits minimum header or SYN MSS option, no scaling/SACK/timestamps. Payload
 * may overlap output (memmove); header is snapshotted before output writes.
 * has_wscale=true is rejected rather than silently dropping a requested option.
 * header_length is output-derived; no packed structs or stack packet buffers. */
int tcp_encode(void *buf, size_t capacity, uint32_t source, uint32_t destination,
               const tcp_header_t *header, const void *data, size_t data_len);
#endif
