#include "arp.h"
#include "net.h"
#include "string.h"

int arp_encode_request(void *buf, size_t buf_len, const uint8_t sender_mac[ETH_ALEN],
                       uint32_t sender_ip, uint32_t target_ip, size_t *out_len) {
    if (!buf || buf_len < ARP_PACKET_LEN || !sender_mac) {
        return -1;
    }

    arp_packet_t *pkt = (arp_packet_t *)buf;
    pkt->htype = htons(ARP_HTYPE_ETHERNET);
    pkt->ptype = htons(ARP_PTYPE_IPV4);
    pkt->hlen = ARP_HLEN_ETHERNET;
    pkt->plen = ARP_PLEN_IPV4;
    pkt->opcode = htons(ARP_OP_REQUEST);

    memcpy(pkt->sender_mac, sender_mac, ETH_ALEN);
    pkt->sender_ip = sender_ip;
    memset(pkt->target_mac, 0, ETH_ALEN);
    pkt->target_ip = target_ip;

    if (out_len) {
        *out_len = ARP_PACKET_LEN;
    }

    return 0;
}

int arp_encode_reply(void *buf, size_t buf_len, const uint8_t sender_mac[ETH_ALEN],
                     uint32_t sender_ip, const uint8_t target_mac[ETH_ALEN],
                     uint32_t target_ip, size_t *out_len) {
    if (!buf || buf_len < ARP_PACKET_LEN || !sender_mac || !target_mac) {
        return -1;
    }

    arp_packet_t *pkt = (arp_packet_t *)buf;
    pkt->htype = htons(ARP_HTYPE_ETHERNET);
    pkt->ptype = htons(ARP_PTYPE_IPV4);
    pkt->hlen = ARP_HLEN_ETHERNET;
    pkt->plen = ARP_PLEN_IPV4;
    pkt->opcode = htons(ARP_OP_REPLY);

    memcpy(pkt->sender_mac, sender_mac, ETH_ALEN);
    pkt->sender_ip = sender_ip;
    memcpy(pkt->target_mac, target_mac, ETH_ALEN);
    pkt->target_ip = target_ip;

    if (out_len) {
        *out_len = ARP_PACKET_LEN;
    }

    return 0;
}

int arp_decode(const void *buf, size_t len, arp_packet_t *out_arp) {
    if (!buf || !out_arp) {
        return -1;
    }

    if (len < ARP_PACKET_LEN) {
        return -1; /* Truncated ARP packet */
    }

    const arp_packet_t *pkt = (const arp_packet_t *)buf;
    uint16_t htype = ntohs(pkt->htype);
    uint16_t ptype = ntohs(pkt->ptype);
    uint16_t opcode = ntohs(pkt->opcode);

    if (htype != ARP_HTYPE_ETHERNET || ptype != ARP_PTYPE_IPV4 ||
        pkt->hlen != ARP_HLEN_ETHERNET || pkt->plen != ARP_PLEN_IPV4 ||
        (opcode != ARP_OP_REQUEST && opcode != ARP_OP_REPLY)) {
        return -2; /* Malformed / unsupported ARP fields */
    }

    out_arp->htype = htype;
    out_arp->ptype = ptype;
    out_arp->hlen = pkt->hlen;
    out_arp->plen = pkt->plen;
    out_arp->opcode = opcode;
    memcpy(out_arp->sender_mac, pkt->sender_mac, ETH_ALEN);
    out_arp->sender_ip = pkt->sender_ip;
    memcpy(out_arp->target_mac, pkt->target_mac, ETH_ALEN);
    out_arp->target_ip = pkt->target_ip;

    return 0;
}

void arp_cache_init(arp_cache_t *cache) {
    if (!cache) return;
    memset(cache, 0, sizeof(*cache));
}

int arp_cache_lookup(const arp_cache_t *cache, uint32_t ip, uint8_t out_mac[ETH_ALEN]) {
    if (!cache || !out_mac) return -1;

    for (size_t i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (cache->entries[i].state == ARP_ENTRY_RESOLVED && cache->entries[i].ip == ip) {
            memcpy(out_mac, cache->entries[i].mac, ETH_ALEN);
            return 0;
        }
    }

    return -1; /* Entry not found in cache */
}

int arp_cache_update(arp_cache_t *cache, uint32_t ip, const uint8_t mac[ETH_ALEN], uint64_t tick) {
    if (!cache || !mac) return -1;

    /* 1. Update existing entry if present */
    for (size_t i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (cache->entries[i].state != ARP_ENTRY_FREE && cache->entries[i].ip == ip) {
            memcpy(cache->entries[i].mac, mac, ETH_ALEN);
            cache->entries[i].state = ARP_ENTRY_RESOLVED;
            cache->entries[i].updated_tick = tick;
            return 0;
        }
    }

    /* 2. Allocate into first free entry */
    for (size_t i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (cache->entries[i].state == ARP_ENTRY_FREE) {
            cache->entries[i].ip = ip;
            memcpy(cache->entries[i].mac, mac, ETH_ALEN);
            cache->entries[i].state = ARP_ENTRY_RESOLVED;
            cache->entries[i].updated_tick = tick;
            return 0;
        }
    }

    /* 3. Evict oldest entry */
    size_t oldest_idx = 0;
    uint64_t oldest_tick = cache->entries[0].updated_tick;
    for (size_t i = 1; i < ARP_CACHE_CAPACITY; i++) {
        if (cache->entries[i].updated_tick < oldest_tick) {
            oldest_tick = cache->entries[i].updated_tick;
            oldest_idx = i;
        }
    }

    cache->entries[oldest_idx].ip = ip;
    memcpy(cache->entries[oldest_idx].mac, mac, ETH_ALEN);
    cache->entries[oldest_idx].state = ARP_ENTRY_RESOLVED;
    cache->entries[oldest_idx].updated_tick = tick;

    return 0;
}
