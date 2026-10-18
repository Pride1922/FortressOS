#ifndef FORTRESS_USER_DHCP_CODEC_H
#define FORTRESS_USER_DHCP_CODEC_H

#include "types.h"

/* BOOTP / DHCP Message Layout (RFC 2131 §2):
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     op (1)    |   htype (1)   |   hlen (1)    |   hops (1)    |
 * +---------------+---------------+---------------+---------------+
 * |                            xid (4)                            |
 * +---------------+---------------+---------------+---------------+
 * |           secs (2)            |           flags (2)           |
 * +---------------+---------------+---------------+---------------+
 * |                          ciaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          yiaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          siaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          giaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          chaddr  (16)                         |
 * |                                                               |
 * |                                                               |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          sname   (64)                         |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          file    (128)                        |
 * +---------------+---------------+---------------+---------------+
 * |                    magic cookie (4) = 0x63825363              |
 * +---------------+---------------+---------------+---------------+
 * |                          options (variable) ...               |
 * +---------------+---------------+---------------+---------------+
 *
 * Total fixed BOOTP header size: 236 bytes.
 * Magic cookie: 4 bytes (offset 236..239).
 * Options start at offset 240.
 */

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2

#define DHCP_HTYPE_ETHER 1
#define DHCP_HLEN_ETHER  6

#define DHCP_MAGIC_COOKIE 0x63825363u

#define DHCP_OPT_PAD             0
#define DHCP_OPT_SUBNET_MASK     1
#define DHCP_OPT_ROUTER          3
#define DHCP_OPT_DNS             6
#define DHCP_OPT_REQUESTED_IP    50
#define DHCP_OPT_LEASE_TIME      51
#define DHCP_OPT_MSG_TYPE        53
#define DHCP_OPT_SERVER_ID       54
#define DHCP_OPT_PARAM_REQ_LIST  55
#define DHCP_OPT_RENEWAL_TIME    58
#define DHCP_OPT_REBINDING_TIME  59
#define DHCP_OPT_CLIENT_ID       61
#define DHCP_OPT_TZ_POSIX        100
#define DHCP_OPT_TZ_IANA         101
#define DHCP_OPT_END             255

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_DECLINE  4
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6
#define DHCP_MSG_RELEASE  7
#define DHCP_MSG_INFORM   8

#define DHCP_PORT_SERVER 67
#define DHCP_PORT_CLIENT 68

#define DHCP_FLAGS_BROADCAST 0x8000

#define DHCP_FIXED_HDR_LEN   236
#define DHCP_OPTIONS_OFFSET  240
#define DHCP_MIN_PACKET_LEN  300
#define DHCP_MAX_PACKET_LEN  576

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} dhcp_packet_hdr_t;

_Static_assert(sizeof(dhcp_packet_hdr_t) == 240, "dhcp_packet_hdr_t must be 240 bytes");

typedef struct {
    uint32_t xid;
    uint8_t  msg_type;
    uint32_t yiaddr;
    uint32_t server_ip;
    uint32_t subnet_mask;
    uint8_t  prefix;
    uint32_t router;
    uint32_t dns_servers[2];
    int      dns_count;
    uint32_t lease_time;
    uint32_t t1;
    uint32_t t2;
    char timezone_posix[128];
    char timezone_iana[128];
} dhcp_offer_t;

int dhcp_encode_discover(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6], size_t *out_len);

int dhcp_encode_request(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                        uint32_t requested_ip, uint32_t server_ip, size_t *out_len);

static inline int dhcp_encode_request_select(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                                             uint32_t requested_ip, uint32_t server_ip, size_t *out_len) {
    return dhcp_encode_request(buf, max_len, xid, mac, requested_ip, server_ip, out_len);
}

int dhcp_encode_request_renew(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                              uint32_t client_ip, size_t *out_len);

int dhcp_encode_request_rebind(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                               uint32_t client_ip, size_t *out_len);

int dhcp_encode_request_reboot(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                               uint32_t requested_ip, size_t *out_len);

int dhcp_encode_release(uint8_t *buf, size_t max_len, uint32_t xid, const uint8_t mac[6],
                        uint32_t client_ip, uint32_t server_ip, size_t *out_len);

int dhcp_decode_reply(const uint8_t *buf, size_t len, uint32_t expected_xid, const uint8_t mac[6],
                      dhcp_offer_t *out);

uint8_t dhcp_mask_to_prefix(uint32_t mask_net_order);

int dhcp_apply_jitter(uint32_t lease_time, uint32_t *t1, uint32_t *t2, int32_t delta1, int32_t delta2);

#endif
