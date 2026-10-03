#ifndef FORTRESS_ICMP_H
#define FORTRESS_ICMP_H
#include "types.h"
#define ICMP_ECHO_HEADER 8u
#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQUEST 8u
#define ICMP_DEST_UNREACHABLE 3u
#define ICMP_TIME_EXCEEDED 11u
typedef struct {
    uint8_t type, code;
    uint32_t source, destination; /* network order, from quoted header */
    uint16_t identifier, sequence, ip_identifier; /* host order */
} icmp_quote_t;
/* Bounded RFC 792 quote, not a full datagram decoder. Failure leaves out intact. */
int icmp_quote_decode(const void *buf, size_t len, icmp_quote_t *out);
typedef struct { uint8_t type; uint16_t identifier, sequence; } icmp_echo_t;
/* Decoded fields host-order, payload borrowed; checksum covers exactly len. */
int icmp_echo_decode(const void *buf, size_t len, icmp_echo_t *out,
                     const uint8_t **data, size_t *data_len);
int icmp_echo_encode(void *buf, size_t cap, uint8_t type, uint16_t identifier,
                     uint16_t sequence, const void *data, size_t len);
#endif
