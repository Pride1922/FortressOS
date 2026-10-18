#include "dhcp_codec.h"

static void dhcp_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void dhcp_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static int dhcp_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static inline uint16_t dhcp_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t dhcp_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

static inline uint32_t dhcp_ntohl(uint32_t v) {
    return dhcp_htonl(v);
}

uint8_t dhcp_mask_to_prefix(uint32_t mask_net_order) {
    uint32_t m = dhcp_ntohl(mask_net_order);
    uint8_t prefix = 0;
    while (m & 0x80000000u) {
        prefix++;
        m <<= 1;
    }
    return prefix ? prefix : 24;
}

int dhcp_encode_discover(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6], size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = dhcp_htons(DHCP_FLAGS_BROADCAST);
    hdr->ciaddr = 0;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = DISCOVER */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_DISCOVER;

    /* Option 61: Client Identifier (Hardware type 1 + MAC) */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 55: Parameter Request List (1=Subnet Mask, 3=Router, 6=DNS) */
    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST;
    buf[pos++] = 5;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;
    buf[pos++] = DHCP_OPT_TZ_POSIX;
    buf[pos++] = DHCP_OPT_TZ_IANA;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    /* Pad to minimum 300 bytes */
    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;

    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_encode_request(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                        uint32_t requested_ip, uint32_t server_ip, size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = dhcp_htons(DHCP_FLAGS_BROADCAST);
    hdr->ciaddr = 0;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = REQUEST */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_REQUEST;

    /* Option 50: Requested IP */
    buf[pos++] = DHCP_OPT_REQUESTED_IP;
    buf[pos++] = 4;
    dhcp_memcpy(buf + pos, &requested_ip, 4);
    pos += 4;

    /* Option 54: Server Identifier */
    if (server_ip) {
        buf[pos++] = DHCP_OPT_SERVER_ID;
        buf[pos++] = 4;
        dhcp_memcpy(buf + pos, &server_ip, 4);
        pos += 4;
    }

    /* Option 61: Client Identifier (Hardware type 1 + MAC) */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 55: Parameter Request List (1=Subnet Mask, 3=Router, 6=DNS) */
    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST;
    buf[pos++] = 5;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;
    buf[pos++] = DHCP_OPT_TZ_POSIX;
    buf[pos++] = DHCP_OPT_TZ_IANA;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    /* Pad to minimum 300 bytes */
    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;

    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_encode_request_renew(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                              uint32_t client_ip, size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = 0;
    hdr->ciaddr = client_ip;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = REQUEST */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_REQUEST;

    /* Option 61: Client Identifier (Hardware type 1 + MAC) */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 55: Parameter Request List (1=Subnet Mask, 3=Router, 6=DNS) */
    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST;
    buf[pos++] = 5;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;
    buf[pos++] = DHCP_OPT_TZ_POSIX;
    buf[pos++] = DHCP_OPT_TZ_IANA;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;
    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_encode_request_rebind(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                               uint32_t client_ip, size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = dhcp_htons(DHCP_FLAGS_BROADCAST);
    hdr->ciaddr = client_ip;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = REQUEST */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_REQUEST;

    /* Option 61: Client Identifier */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 55: Parameter Request List */
    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST;
    buf[pos++] = 5;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;
    buf[pos++] = DHCP_OPT_TZ_POSIX;
    buf[pos++] = DHCP_OPT_TZ_IANA;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;
    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_encode_request_reboot(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                               uint32_t requested_ip, size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = dhcp_htons(DHCP_FLAGS_BROADCAST);
    hdr->ciaddr = 0;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = REQUEST */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_REQUEST;

    /* Option 50: Requested IP */
    buf[pos++] = DHCP_OPT_REQUESTED_IP;
    buf[pos++] = 4;
    dhcp_memcpy(buf + pos, &requested_ip, 4);
    pos += 4;

    /* Option 61: Client Identifier */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 55: Parameter Request List */
    buf[pos++] = DHCP_OPT_PARAM_REQ_LIST;
    buf[pos++] = 5;
    buf[pos++] = DHCP_OPT_SUBNET_MASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;
    buf[pos++] = DHCP_OPT_TZ_POSIX;
    buf[pos++] = DHCP_OPT_TZ_IANA;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;
    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_encode_release(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                        uint32_t client_ip, uint32_t server_ip, size_t *out_len) {
    if (!buf || max_len < DHCP_MIN_PACKET_LEN || !mac) return -1;
    dhcp_memset(buf, 0, max_len);

    dhcp_packet_hdr_t *hdr = (dhcp_packet_hdr_t *)buf;
    hdr->op = DHCP_BOOTREQUEST;
    hdr->htype = DHCP_HTYPE_ETHER;
    hdr->hlen = DHCP_HLEN_ETHER;
    hdr->hops = 0;
    hdr->xid = xid;
    hdr->secs = 0;
    hdr->flags = 0;
    hdr->ciaddr = client_ip;
    hdr->yiaddr = 0;
    hdr->siaddr = 0;
    hdr->giaddr = 0;
    dhcp_memcpy(hdr->chaddr, mac, 6);
    hdr->magic = dhcp_htonl(DHCP_MAGIC_COOKIE);

    size_t pos = DHCP_OPTIONS_OFFSET;
    /* Option 53: Message Type = RELEASE */
    buf[pos++] = DHCP_OPT_MSG_TYPE;
    buf[pos++] = 1;
    buf[pos++] = DHCP_MSG_RELEASE;

    /* Option 54: Server Identifier */
    if (server_ip) {
        buf[pos++] = DHCP_OPT_SERVER_ID;
        buf[pos++] = 4;
        dhcp_memcpy(buf + pos, &server_ip, 4);
        pos += 4;
    }

    /* Option 61: Client Identifier */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;
    dhcp_memcpy(buf + pos, mac, 6);
    pos += 6;

    /* Option 255: End */
    buf[pos++] = DHCP_OPT_END;

    if (pos < DHCP_MIN_PACKET_LEN) pos = DHCP_MIN_PACKET_LEN;
    if (out_len) *out_len = pos;
    return 0;
}

int dhcp_decode_reply(const uint8_t *buf, size_t len, uint32_t expected_xid, const uint8_t mac[6],
                      dhcp_offer_t *out) {
    if (!buf || len < DHCP_OPTIONS_OFFSET || !out) return -1;

    const dhcp_packet_hdr_t *hdr = (const dhcp_packet_hdr_t *)buf;
    if (hdr->op != DHCP_BOOTREPLY) return -2;
    if (hdr->htype != DHCP_HTYPE_ETHER || hdr->hlen != DHCP_HLEN_ETHER) return -3;
    if (expected_xid && hdr->xid != expected_xid) return -4;
    if (mac && dhcp_memcmp(hdr->chaddr, mac, 6) != 0) return -5;
    if (hdr->magic != dhcp_htonl(DHCP_MAGIC_COOKIE)) return -6;

    dhcp_memset(out, 0, sizeof(*out));
    out->xid = hdr->xid;
    out->yiaddr = hdr->yiaddr;
    out->server_ip = hdr->siaddr;

    size_t pos = DHCP_OPTIONS_OFFSET;
    while (pos < len) {
        uint8_t opt = buf[pos++];
        if (opt == DHCP_OPT_PAD) continue;
        if (opt == DHCP_OPT_END) break;
        if (pos >= len) return -7; /* Truncated option header */
        uint8_t opt_len = buf[pos++];
        if (pos + opt_len > len) return -8; /* Truncated option data */

        if (opt == DHCP_OPT_TZ_POSIX || opt == DHCP_OPT_TZ_IANA) {
            char *dst=opt==DHCP_OPT_TZ_POSIX?out->timezone_posix:out->timezone_iana;
            /* Optional discovery must never reject an otherwise valid lease. */
            if(opt_len && opt_len<128) {
                bool valid=true;
                for(unsigned i=0;i<opt_len;i++)if(buf[pos+i]<33||buf[pos+i]>126)valid=false;
                if(valid){dhcp_memcpy(dst,buf+pos,opt_len);dst[opt_len]=0;}
            }
        } else if (opt == DHCP_OPT_MSG_TYPE && opt_len == 1) {
            out->msg_type = buf[pos];
        } else if (opt == DHCP_OPT_SUBNET_MASK && opt_len == 4) {
            dhcp_memcpy(&out->subnet_mask, buf + pos, 4);
        } else if (opt == DHCP_OPT_ROUTER && opt_len >= 4) {
            dhcp_memcpy(&out->router, buf + pos, 4);
        } else if (opt == DHCP_OPT_DNS && opt_len >= 4) {
            int count = opt_len / 4;
            if (count > 2) count = 2;
            for (int i = 0; i < count; i++) {
                dhcp_memcpy(&out->dns_servers[i], buf + pos + i * 4, 4);
            }
            out->dns_count = count;
        } else if (opt == DHCP_OPT_SERVER_ID && opt_len == 4) {
            dhcp_memcpy(&out->server_ip, buf + pos, 4);
        } else if (opt == DHCP_OPT_LEASE_TIME && opt_len == 4) {
            uint32_t lt;
            dhcp_memcpy(&lt, buf + pos, 4);
            out->lease_time = dhcp_ntohl(lt);
        } else if (opt == DHCP_OPT_RENEWAL_TIME && opt_len == 4) {
            uint32_t t1;
            dhcp_memcpy(&t1, buf + pos, 4);
            out->t1 = dhcp_ntohl(t1);
        } else if (opt == DHCP_OPT_REBINDING_TIME && opt_len == 4) {
            uint32_t t2;
            dhcp_memcpy(&t2, buf + pos, 4);
            out->t2 = dhcp_ntohl(t2);
        }

        pos += opt_len;
    }

    if (out->lease_time == 0) {
        out->lease_time = 86400;
    }
    if (out->t1 == 0) {
        out->t1 = out->lease_time / 2;
    }
    if (out->t2 == 0) {
        out->t2 = (uint32_t)((uint64_t)out->lease_time * 7 / 8);
    }

    out->prefix = dhcp_mask_to_prefix(out->subnet_mask);
    return 0;
}

int dhcp_apply_jitter(uint32_t lease_time, uint32_t *t1, uint32_t *t2, int32_t delta1, int32_t delta2) {
    if (!t1 || !t2 || lease_time == 0) return -1;
    if (lease_time <= 2) {
        *t1 = 1;
        *t2 = 1;
        return 0;
    }
    int64_t new_t1 = (int64_t)(*t1) + delta1;
    if (new_t1 < 1) new_t1 = 1;
    if (new_t1 >= (int64_t)lease_time - 1) new_t1 = (int64_t)lease_time - 2;

    int64_t new_t2 = (int64_t)(*t2) + delta2;
    if (new_t2 <= new_t1) new_t2 = new_t1 + 1;
    if (new_t2 >= (int64_t)lease_time) new_t2 = (int64_t)lease_time - 1;

    if (new_t1 >= new_t2) {
        new_t1 = new_t2 > 1 ? new_t2 - 1 : 1;
    }

    *t1 = (uint32_t)new_t1;
    *t2 = (uint32_t)new_t2;
    return 0;
}

