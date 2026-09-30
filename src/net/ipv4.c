#include "ipv4.h"
#include "checksum.h"
#include "net.h"
#include "string.h"

uint16_t ipv4_calculate_checksum(const ipv4_header_t *hdr) {
    if (!hdr) return 0;
    uint8_t ihl = ipv4_get_ihl_bytes(hdr);
    if (ihl < IPV4_MIN_HLEN) return 0;

    /* Accumulate header skipping the checksum field at offset 10..11 */
    uint32_t acc = net_checksum_accumulate(hdr, 10, 0);
    acc = net_checksum_accumulate((const uint8_t *)hdr + 12, ihl - 12, acc);
    return htons(net_checksum_finish(acc));
}

bool ipv4_verify_checksum(const ipv4_header_t *hdr) {
    if (!hdr) return false;
    uint8_t ihl = ipv4_get_ihl_bytes(hdr);
    if (ihl < IPV4_MIN_HLEN) return false;

    /* Computing checksum over the entire header should result in 0 */
    return net_checksum(hdr, ihl, 0) == 0;
}

bool ipv4_is_fragment(const ipv4_header_t *hdr) {
    if (!hdr) return false;
    uint16_t ff = ntohs(hdr->flags_frag);
    return (ff & (IPV4_FLAG_MF | IPV4_FRAG_OFF_MASK)) != 0;
}

int ipv4_encode(void *buf, size_t buf_len, uint32_t src_ip, uint32_t dst_ip,
                uint8_t protocol, uint16_t payload_len, uint8_t ttl, size_t *out_hdr_len) {
    if (!buf || buf_len < IPV4_MIN_HLEN) {
        return -1;
    }

    ipv4_header_t *hdr = (ipv4_header_t *)buf;
    hdr->version_ihl = (IPV4_VERSION_4 << 4) | (IPV4_MIN_HLEN / 4);
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)(IPV4_MIN_HLEN + payload_len));
    hdr->id = 0;
    hdr->flags_frag = htons(IPV4_FLAG_DF);
    hdr->ttl = ttl ? ttl : 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = src_ip;
    hdr->dst_ip = dst_ip;

    hdr->checksum = ipv4_calculate_checksum(hdr);

    if (out_hdr_len) {
        *out_hdr_len = IPV4_MIN_HLEN;
    }

    return 0;
}

int ipv4_decode(const void *buf, size_t len, ipv4_header_t *out_hdr,
                const uint8_t **out_payload, size_t *out_payload_len) {
    if (!buf || !out_hdr) {
        return -1;
    }

    if (len < IPV4_MIN_HLEN) {
        return -1; /* Truncated buffer */
    }

    const ipv4_header_t *hdr = (const ipv4_header_t *)buf;

    if (ipv4_get_version(hdr) != IPV4_VERSION_4) {
        return -2; /* Unsupported IP version */
    }

    uint8_t ihl = ipv4_get_ihl_bytes(hdr);
    if (ihl < IPV4_MIN_HLEN || len < ihl) {
        return -3; /* Invalid IHL or truncated options */
    }

    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len < ihl || total_len > len) {
        return -4; /* Total length mismatch or truncated payload */
    }

    if (!ipv4_verify_checksum(hdr)) {
        return -5; /* Checksum verification failed */
    }

    if (ipv4_is_fragment(hdr)) {
        return -6; /* Fragmented packets rejected in Milestone NET-1 */
    }

    memcpy(out_hdr, hdr, sizeof(ipv4_header_t));

    if (out_payload) {
        *out_payload = (const uint8_t *)buf + ihl;
    }
    if (out_payload_len) {
        *out_payload_len = total_len - ihl;
    }

    return 0;
}
