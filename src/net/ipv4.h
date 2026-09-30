#ifndef FORTRESS_NET_IPV4_H
#define FORTRESS_NET_IPV4_H

#include "types.h"

#define IPV4_MIN_HLEN      20
#define IPV4_VERSION_4     4

#define IPV4_PROTO_ICMP    0x01
#define IPV4_PROTO_TCP     0x06
#define IPV4_PROTO_UDP     0x11

#define IPV4_FLAG_DF       0x4000
#define IPV4_FLAG_MF       0x2000
#define IPV4_FRAG_OFF_MASK 0x1FFF

/* Standard 20-byte RFC 791 IPv4 Header */
typedef struct ipv4_header {
    uint8_t  version_ihl;   /* Version (high 4 bits) + IHL in 32-bit words (low 4 bits) */
    uint8_t  tos;           /* Type of Service / DSCP+ECN */
    uint16_t total_len;     /* Total packet length (header + payload) in network byte order */
    uint16_t id;            /* Identification in network byte order */
    uint16_t flags_frag;    /* Flags (3 bits) + Fragment Offset (13 bits) in network byte order */
    uint8_t  ttl;           /* Time to Live */
    uint8_t  protocol;      /* Higher layer protocol */
    uint16_t checksum;      /* Header checksum in network byte order */
    uint32_t src_ip;        /* Source IPv4 address in network byte order */
    uint32_t dst_ip;        /* Destination IPv4 address in network byte order */
} __attribute__((packed)) ipv4_header_t;

/* Header field accessor helpers */
static inline uint8_t ipv4_get_version(const ipv4_header_t *hdr) {
    return hdr ? (hdr->version_ihl >> 4) : 0;
}

static inline uint8_t ipv4_get_ihl_bytes(const ipv4_header_t *hdr) {
    return hdr ? ((hdr->version_ihl & 0x0F) * 4) : 0;
}

/* Calculate RFC 1071 checksum over an IPv4 header (with checksum field treated as 0) */
uint16_t ipv4_calculate_checksum(const ipv4_header_t *hdr);

/* Verify that the IPv4 header checksum is valid */
bool ipv4_verify_checksum(const ipv4_header_t *hdr);

/* Check if packet is a fragment (MF bit set or fragment offset > 0) */
bool ipv4_is_fragment(const ipv4_header_t *hdr);

/*
 * Encode a standard 20-byte IPv4 header (no options).
 * Computes and sets the valid header checksum.
 * Returns 0 on success, negative error code on failure.
 */
int ipv4_encode(void *buf, size_t buf_len, uint32_t src_ip, uint32_t dst_ip,
                uint8_t protocol, uint16_t payload_len, uint8_t ttl, size_t *out_hdr_len);

/*
 * Decode and bounds-check an IPv4 packet.
 * Validates:
 *   - len >= IPV4_MIN_HLEN
 *   - version == 4
 *   - IHL >= 5 (and len >= IHL * 4)
 *   - total_len <= len
 *   - checksum is valid
 *   - unfragmented (rejects fragments in Milestone NET-1)
 * Returns 0 on success, negative error code on failure.
 */
int ipv4_decode(const void *buf, size_t len, ipv4_header_t *out_hdr,
                const uint8_t **out_payload, size_t *out_payload_len);

#endif /* FORTRESS_NET_IPV4_H */
