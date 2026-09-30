#include "eth.h"
#include "net.h"
#include "string.h"

bool eth_is_broadcast(const uint8_t mac[ETH_ALEN]) {
    if (!mac) return false;
    for (int i = 0; i < ETH_ALEN; i++) {
        if (mac[i] != 0xFF) return false;
    }
    return true;
}

bool eth_mac_equal(const uint8_t a[ETH_ALEN], const uint8_t b[ETH_ALEN]) {
    if (!a || !b) return false;
    return memcmp(a, b, ETH_ALEN) == 0;
}

bool eth_mac_matches(const uint8_t dest_mac[ETH_ALEN], const uint8_t host_mac[ETH_ALEN]) {
    if (!dest_mac) return false;
    if (eth_is_broadcast(dest_mac)) return true;
    if (host_mac && eth_mac_equal(dest_mac, host_mac)) return true;
    return false;
}

int eth_encode(void *buf, size_t buf_len, const uint8_t dest_mac[ETH_ALEN],
               const uint8_t src_mac[ETH_ALEN], uint16_t ethertype, size_t *out_hdr_len) {
    if (!buf || buf_len < ETH_HDR_LEN || !dest_mac || !src_mac) {
        return -1;
    }

    eth_header_t *hdr = (eth_header_t *)buf;
    memcpy(hdr->dest, dest_mac, ETH_ALEN);
    memcpy(hdr->src, src_mac, ETH_ALEN);
    hdr->ethertype = htons(ethertype);

    if (out_hdr_len) {
        *out_hdr_len = ETH_HDR_LEN;
    }

    return 0;
}

int eth_decode(const void *buf, size_t len, eth_header_t *out_hdr,
               const uint8_t **out_payload, size_t *out_payload_len) {
    if (!buf || !out_hdr) {
        return -1;
    }

    if (len < ETH_HDR_LEN) {
        return -1; /* Runt / truncated header */
    }

    if (len > ETH_MAX_FRAME_LEN) {
        return -2; /* Exceeds maximum Ethernet frame bounds */
    }

    const eth_header_t *hdr = (const eth_header_t *)buf;
    memcpy(out_hdr->dest, hdr->dest, ETH_ALEN);
    memcpy(out_hdr->src, hdr->src, ETH_ALEN);
    out_hdr->ethertype = ntohs(hdr->ethertype);

    if (out_payload) {
        *out_payload = (const uint8_t *)buf + ETH_HDR_LEN;
    }
    if (out_payload_len) {
        *out_payload_len = len - ETH_HDR_LEN;
    }

    return 0;
}
