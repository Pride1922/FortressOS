#ifndef FORTRESS_NET_ARP_H
#define FORTRESS_NET_ARP_H

#include "types.h"
#include "eth.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4     0x0800
#define ARP_HLEN_ETHERNET  6
#define ARP_PLEN_IPV4      4

#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2
#define ARP_PACKET_LEN     28

/* Standard 28-byte RFC 826 ARP Packet for Ethernet + IPv4 */
typedef struct arp_packet {
    uint16_t htype;                      /* Hardware Type (stored in network order on wire) */
    uint16_t ptype;                      /* Protocol Type (stored in network order on wire) */
    uint8_t  hlen;                       /* Hardware Address Length (6) */
    uint8_t  plen;                       /* Protocol Address Length (4) */
    uint16_t opcode;                     /* Opcode: 1=Request, 2=Reply */
    uint8_t  sender_mac[ETH_ALEN];       /* Sender Hardware Address */
    uint32_t sender_ip;                  /* Sender Protocol Address (network byte order) */
    uint8_t  target_mac[ETH_ALEN];       /* Target Hardware Address */
    uint32_t target_ip;                  /* Target Protocol Address (network byte order) */
} __attribute__((packed)) arp_packet_t;

/* Encode an ARP Request packet */
int arp_encode_request(void *buf, size_t buf_len, const uint8_t sender_mac[ETH_ALEN],
                       uint32_t sender_ip, uint32_t target_ip, size_t *out_len);

/* Encode an ARP Reply packet */
int arp_encode_reply(void *buf, size_t buf_len, const uint8_t sender_mac[ETH_ALEN],
                     uint32_t sender_ip, const uint8_t target_mac[ETH_ALEN],
                     uint32_t target_ip, size_t *out_len);

/* Decode and validate: htype/ptype/opcode become host-order; IPs stay network-order. */
int arp_decode(const void *buf, size_t len, arp_packet_t *out_arp);

/* ARP Cache Data Structures & Pure Helpers (Phase 0 foundation) */
typedef enum {
    ARP_ENTRY_FREE = 0,
    ARP_ENTRY_RESOLVING = 1,
    ARP_ENTRY_RESOLVED  = 2
} arp_entry_state_t;

typedef struct {
    uint32_t          ip;                /* IPv4 address (network byte order) */
    uint8_t           mac[ETH_ALEN];     /* Resolved MAC address */
    arp_entry_state_t state;             /* Current entry state */
    uint64_t          updated_tick;      /* Timestamp tick of last update */
} arp_entry_t;

#define ARP_CACHE_CAPACITY 16

typedef struct {
    arp_entry_t entries[ARP_CACHE_CAPACITY];
} arp_cache_t;

void arp_cache_init(arp_cache_t *cache);
int  arp_cache_lookup(const arp_cache_t *cache, uint32_t ip, uint8_t out_mac[ETH_ALEN]);
int  arp_cache_update(arp_cache_t *cache, uint32_t ip, const uint8_t mac[ETH_ALEN], uint64_t tick);

#endif /* FORTRESS_NET_ARP_H */
