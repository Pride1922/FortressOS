#ifndef FORTRESS_NET_ETH_H
#define FORTRESS_NET_ETH_H

#include "types.h"

#define ETH_ALEN          6
#define ETH_HDR_LEN       14
#define ETH_MIN_FRAME_LEN 60
#define ETH_MAX_FRAME_LEN 1514

#define ETHERTYPE_IPV4    0x0800
#define ETHERTYPE_ARP     0x0806

/* Standard 14-byte Ethernet II Header */
typedef struct eth_header {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;          /* Stored in network byte order */
} __attribute__((packed)) eth_header_t;

/* Check if MAC is the broadcast address (FF:FF:FF:FF:FF:FF) */
bool eth_is_broadcast(const uint8_t mac[ETH_ALEN]);

/* Compare two MAC addresses for equality */
bool eth_mac_equal(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN]);

/* Check if destination MAC matches host MAC or broadcast */
bool eth_mac_matches(const uint8_t dest_mac[ETH_ALEN], const uint8_t host_mac[ETH_ALEN]);

/*
 * Encode an Ethernet II frame header.
 * Writes 14 bytes into buf.
 * Returns 0 on success, negative error code on failure.
 */
int eth_encode(void *buf, size_t buf_len, const uint8_t dest_mac[ETH_ALEN],
               const uint8_t src_mac[ETH_ALEN], uint16_t ethertype, size_t *out_hdr_len);

/*
 * Decode and bounds-check an Ethernet II frame.
 * out_hdr->ethertype is host-order (the encoded wire header is network-order).
 * Validates length >= ETH_HDR_LEN and <= ETH_MAX_FRAME_LEN.
 * Returns 0 on success, negative error code on failure.
 */
int eth_decode(const void *buf, size_t len, eth_header_t *out_hdr,
               const uint8_t **out_payload, size_t *out_payload_len);

#endif /* FORTRESS_NET_ETH_H */
