#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dhcp_codec.h"

int main(void) {
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint32_t xid = 0x1a2b3c4d;
    uint8_t wire[576];
    size_t wire_len = 0;

    /* 1. DISCOVER encoding */
    assert(dhcp_encode_discover(wire, sizeof(wire), xid, mac, &wire_len) == 0);
    assert(wire_len >= 300);
    const dhcp_packet_hdr_t *hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->op == DHCP_BOOTREQUEST);
    assert(hdr->htype == DHCP_HTYPE_ETHER);
    assert(hdr->hlen == DHCP_HLEN_ETHER);
    assert(hdr->hops == 0);
    assert(hdr->xid == xid);
    assert(hdr->flags == __builtin_bswap16(DHCP_FLAGS_BROADCAST));
    assert(hdr->ciaddr == 0 && hdr->yiaddr == 0 && hdr->siaddr == 0 && hdr->giaddr == 0);
    assert(memcmp(hdr->chaddr, mac, 6) == 0);
    assert(hdr->magic == __builtin_bswap32(DHCP_MAGIC_COOKIE));

    /* Verify options in DISCOVER */
    assert(wire[240] == DHCP_OPT_MSG_TYPE && wire[241] == 1 && wire[242] == DHCP_MSG_DISCOVER);
    assert(wire[243] == DHCP_OPT_CLIENT_ID && wire[244] == 7 && wire[245] == 1 && memcmp(wire + 246, mac, 6) == 0);
    assert(wire[252] == DHCP_OPT_PARAM_REQ_LIST && wire[253] == 5);
    assert(wire[254] == DHCP_OPT_SUBNET_MASK && wire[255] == DHCP_OPT_ROUTER && wire[256] == DHCP_OPT_DNS);
    assert(wire[257] == DHCP_OPT_TZ_POSIX && wire[258] == DHCP_OPT_TZ_IANA);
    assert(wire[259] == DHCP_OPT_END);

    /* 2. REQUEST encoding */
    uint32_t req_ip = 0x0f02000a; /* 10.0.2.15 in net order */
    uint32_t srv_ip = 0x0202000a; /* 10.0.2.2 in net order */
    assert(dhcp_encode_request(wire, sizeof(wire), xid, mac, req_ip, srv_ip, &wire_len) == 0);
    assert(wire_len >= 300);
    hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->xid == xid);
    assert(wire[240] == DHCP_OPT_MSG_TYPE && wire[241] == 1 && wire[242] == DHCP_MSG_REQUEST);
    assert(wire[243] == DHCP_OPT_REQUESTED_IP && wire[244] == 4 && memcmp(wire + 245, &req_ip, 4) == 0);
    assert(wire[249] == DHCP_OPT_SERVER_ID && wire[250] == 4 && memcmp(wire + 251, &srv_ip, 4) == 0);

    /* 3. Synthetic OFFER decoding */
    uint8_t offer_pkt[576];
    memset(offer_pkt, 0, sizeof(offer_pkt));
    dhcp_packet_hdr_t *off_hdr = (dhcp_packet_hdr_t *)offer_pkt;
    off_hdr->op = DHCP_BOOTREPLY;
    off_hdr->htype = DHCP_HTYPE_ETHER;
    off_hdr->hlen = DHCP_HLEN_ETHER;
    off_hdr->xid = xid;
    off_hdr->yiaddr = req_ip;
    off_hdr->siaddr = srv_ip;
    memcpy(off_hdr->chaddr, mac, 6);
    off_hdr->magic = __builtin_bswap32(DHCP_MAGIC_COOKIE);

    size_t p = DHCP_OPTIONS_OFFSET;
    /* Msg type = OFFER */
    offer_pkt[p++] = DHCP_OPT_MSG_TYPE; offer_pkt[p++] = 1; offer_pkt[p++] = DHCP_MSG_OFFER;
    /* Subnet mask = 255.255.255.0 */
    uint32_t mask = 0x00ffffff; /* 255.255.255.0 in net order */
    offer_pkt[p++] = DHCP_OPT_SUBNET_MASK; offer_pkt[p++] = 4; memcpy(offer_pkt + p, &mask, 4); p += 4;
    /* Router = 10.0.2.2 */
    offer_pkt[p++] = DHCP_OPT_ROUTER; offer_pkt[p++] = 4; memcpy(offer_pkt + p, &srv_ip, 4); p += 4;
    /* DNS = 10.0.2.3 */
    uint32_t dns = 0x0302000a;
    offer_pkt[p++] = DHCP_OPT_DNS; offer_pkt[p++] = 4; memcpy(offer_pkt + p, &dns, 4); p += 4;
    /* Lease time = 86400 */
    uint32_t lease = __builtin_bswap32(86400);
    offer_pkt[p++] = DHCP_OPT_LEASE_TIME; offer_pkt[p++] = 4; memcpy(offer_pkt + p, &lease, 4); p += 4;
    /* Server ID = 10.0.2.2 */
    offer_pkt[p++] = DHCP_OPT_SERVER_ID; offer_pkt[p++] = 4; memcpy(offer_pkt + p, &srv_ip, 4); p += 4;
    offer_pkt[p++] = DHCP_OPT_END;
    if (p < 300) p = 300;

    dhcp_offer_t offer;
    assert(dhcp_decode_reply(offer_pkt, p, xid, mac, &offer) == 0);
    assert(offer.msg_type == DHCP_MSG_OFFER);
    assert(offer.yiaddr == req_ip);
    assert(offer.server_ip == srv_ip);
    assert(offer.subnet_mask == mask);
    assert(offer.prefix == 24);
    assert(offer.router == srv_ip);
    assert(offer.dns_count == 1);
    assert(offer.dns_servers[0] == dns);
    assert(offer.lease_time == 86400);

    /* 4. Edge cases and hostile input */
    /* Wrong XID */
    assert(dhcp_decode_reply(offer_pkt, p, xid + 1, mac, &offer) == -4);
    /* Wrong MAC */
    uint8_t wrong_mac[6] = {1, 2, 3, 4, 5, 6};
    assert(dhcp_decode_reply(offer_pkt, p, xid, wrong_mac, &offer) == -5);
    /* Wrong Op */
    off_hdr->op = DHCP_BOOTREQUEST;
    assert(dhcp_decode_reply(offer_pkt, p, xid, mac, &offer) == -2);
    off_hdr->op = DHCP_BOOTREPLY;
    /* Bad magic cookie */
    off_hdr->magic = 0;
    assert(dhcp_decode_reply(offer_pkt, p, xid, mac, &offer) == -6);
    off_hdr->magic = __builtin_bswap32(DHCP_MAGIC_COOKIE);
    /* Truncated packet < 240 */
    assert(dhcp_decode_reply(offer_pkt, 239, xid, mac, &offer) == -1);
    /* Truncated option header */
    offer_pkt[240] = 53;
    assert(dhcp_decode_reply(offer_pkt, 241, xid, mac, &offer) == -7);
    /* Truncated option data */
    offer_pkt[240] = 53; offer_pkt[241] = 5;
    assert(dhcp_decode_reply(offer_pkt, 244, xid, mac, &offer) == -8);

    /* 5. Subnet mask prefix calculation */
    assert(dhcp_mask_to_prefix(0x00ffffff) == 24); /* 255.255.255.0 */
    assert(dhcp_mask_to_prefix(0x0000ffff) == 16); /* 255.255.0.0 */
    assert(dhcp_mask_to_prefix(0x000000ff) == 8);  /* 255.0.0.0 */
    assert(dhcp_mask_to_prefix(0x80ffffff) == 25); /* 255.255.255.128 */
    assert(dhcp_mask_to_prefix(0x00000000) == 24); /* fallback to 24 */

    memset(offer_pkt+DHCP_OPTIONS_OFFSET,0,sizeof(offer_pkt)-DHCP_OPTIONS_OFFSET);
    p=DHCP_OPTIONS_OFFSET;
    offer_pkt[p++]=DHCP_OPT_MSG_TYPE;offer_pkt[p++]=1;offer_pkt[p++]=DHCP_MSG_ACK;
    offer_pkt[p++]=DHCP_OPT_TZ_IANA;offer_pkt[p++]=15;
    memcpy(offer_pkt+p,"Europe/Brussels",15);p+=15;
    offer_pkt[p++]=DHCP_OPT_TZ_POSIX;offer_pkt[p++]=5;
    memcpy(offer_pkt+p,"UTC-2",5);p+=5;offer_pkt[p++]=DHCP_OPT_END;
    assert(dhcp_decode_reply(offer_pkt,p,xid,mac,&offer)==0);
    assert(!strcmp(offer.timezone_iana,"Europe/Brussels")&&!strcmp(offer.timezone_posix,"UTC-2"));
    offer_pkt[DHCP_OPTIONS_OFFSET+5]='\n';
    assert(dhcp_decode_reply(offer_pkt,p,xid,mac,&offer)==0&&offer.timezone_iana[0]==0);
    puts("DHCP codec ASan/UBSan PASS: encoding, decoding, bounds and RFC 4833 timezone discovery");
    return 0;
}
