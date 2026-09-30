#ifndef FORTRESS_NET_H
#define FORTRESS_NET_H

#include "types.h"

/* Network Byte Order (Big-Endian) Conversion Primitives for x86_64 */
static inline uint16_t htons(uint16_t hostshort) {
    return __builtin_bswap16(hostshort);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return __builtin_bswap16(netshort);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return __builtin_bswap32(hostlong);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return __builtin_bswap32(netlong);
}

/* Device State Flags */
#define NET_UP       (1U << 0)
#define NET_RUNNING  (1U << 1)

/* Packet Buffer Capacity (2048 bytes accommodates standard MTU 1500 + headers) */
#define PBUF_CAPACITY 2048

/* Packet Buffer Flags */
#define PBUF_FLAG_DMA_BACKED (1U << 0)
#define PBUF_FLAG_ALLOCATED  (1U << 1)

/* Bounded Packet Buffer Structure (NET_PLAN.md §3.1) */
typedef struct pbuf {
    struct pbuf *next;                   /* Intrusive queue link */
    uint8_t     *payload;                /* Pointer into data buffer (advances past headers) */
    uint16_t     length;                 /* Current layer payload length */
    uint16_t     total_len;              /* Total packet length */
    uint32_t     flags;                  /* PBUF_FLAG_* */
    uint8_t      data[PBUF_CAPACITY] __attribute__((aligned(16))); /* Backing memory buffer */
} pbuf_t;

/* Reset packet buffer to initial empty state */
static inline void pbuf_init(pbuf_t *p) {
    if (!p) return;
    p->next = NULL;
    p->payload = p->data;
    p->length = 0;
    p->total_len = 0;
    p->flags = 0;
}

/* Reset payload pointer and length for reuse */
static inline void pbuf_reset(pbuf_t *p) {
    if (!p) return;
    p->next = NULL;
    p->payload = p->data;
    p->length = 0;
    p->total_len = 0;
}

/* Advance payload pointer past consumed header */
static inline int pbuf_header_consume(pbuf_t *p, size_t header_size) {
    if (!p || p->length < header_size) return -1;
    p->payload += header_size;
    p->length -= (uint16_t)header_size;
    return 0;
}

/* Uniform Network Device Interface (NET_PLAN.md §3, mirroring block_dev_t) */
typedef struct net_dev {
    char        name[16];                /* e.g. "eth0" */
    uint8_t     mac_addr[6];             /* Hardware MAC Address */
    uint32_t    mtu;                     /* Maximum Transmission Unit (default 1500) */
    uint32_t    flags;                   /* NET_UP, NET_RUNNING */
    int       (*send_packet)(struct net_dev *dev, const void *buf, size_t len);
    /* Returned packet is caller-owned until recycle_rx; NULL means no packet.
     * Callbacks require unlocked thread context. */
    pbuf_t   *(*poll_rx)(struct net_dev *dev);
    void      (*recycle_rx)(struct net_dev *dev, pbuf_t *packet);
    void       *priv;                    /* Controller-specific private state */
} net_dev_t;

#endif /* FORTRESS_NET_H */
