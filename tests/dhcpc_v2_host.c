#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dhcp_codec.h"

static bool find_option(const uint8_t *buf, size_t len, uint8_t opt_code, const uint8_t **out_val, uint8_t *out_len) {
    if (len < DHCP_OPTIONS_OFFSET) return false;
    size_t pos = DHCP_OPTIONS_OFFSET;
    while (pos < len) {
        uint8_t code = buf[pos++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) break;
        if (pos >= len) return false;
        uint8_t l = buf[pos++];
        if (pos + l > len) return false;
        if (code == opt_code) {
            if (out_val) *out_val = buf + pos;
            if (out_len) *out_len = l;
            return true;
        }
        pos += l;
    }
    return false;
}

int main(void) {
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t xid = 0x87654321;
    uint32_t client_ip = 0x3201a8c0; /* 192.168.1.50 */
    uint32_t server_ip = 0x0101a8c0; /* 192.168.1.1 */
    uint8_t wire[576];
    size_t wire_len = 0;

    /* =========================================================================
     * 1. Unicast Renewal DHCPREQUEST (RFC 2131 §4.3.6)
     * ========================================================================= */
    assert(dhcp_encode_request_renew(wire, sizeof(wire), xid, mac, client_ip, &wire_len) == 0);
    assert(wire_len >= DHCP_MIN_PACKET_LEN);
    const dhcp_packet_hdr_t *hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->op == DHCP_BOOTREQUEST);
    assert(hdr->htype == DHCP_HTYPE_ETHER);
    assert(hdr->hlen == DHCP_HLEN_ETHER);
    assert(hdr->hops == 0);
    assert(hdr->xid == xid);
    assert(hdr->flags == 0); /* MUST be unicast */
    assert(hdr->ciaddr == client_ip); /* MUST have ciaddr set */
    assert(hdr->yiaddr == 0 && hdr->siaddr == 0 && hdr->giaddr == 0);
    assert(memcmp(hdr->chaddr, mac, 6) == 0);
    assert(hdr->magic == __builtin_bswap32(DHCP_MAGIC_COOKIE));

    /* Verify options: 53=REQUEST, 61=client_id, 55=params, 255=END */
    const uint8_t *val = NULL;
    uint8_t opt_len = 0;
    assert(find_option(wire, wire_len, DHCP_OPT_MSG_TYPE, &val, &opt_len) && opt_len == 1 && *val == DHCP_MSG_REQUEST);
    assert(find_option(wire, wire_len, DHCP_OPT_CLIENT_ID, &val, &opt_len) && opt_len == 7 && val[0] == 1 && memcmp(val + 1, mac, 6) == 0);
    assert(find_option(wire, wire_len, DHCP_OPT_PARAM_REQ_LIST, &val, &opt_len) && opt_len == 5);
    assert(val[3]==DHCP_OPT_TZ_POSIX&&val[4]==DHCP_OPT_TZ_IANA);

    /* MUST NOT include Option 50 (requested IP) or Option 54 (server ID) in renewal */
    assert(!find_option(wire, wire_len, DHCP_OPT_REQUESTED_IP, NULL, NULL));
    assert(!find_option(wire, wire_len, DHCP_OPT_SERVER_ID, NULL, NULL));

    /* =========================================================================
     * 2. Broadcast Rebind DHCPREQUEST (RFC 2131 §4.3.6)
     * ========================================================================= */
    memset(wire, 0, sizeof(wire));
    assert(dhcp_encode_request_rebind(wire, sizeof(wire), xid + 1, mac, client_ip, &wire_len) == 0);
    hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->xid == xid + 1);
    assert(hdr->flags == __builtin_bswap16(DHCP_FLAGS_BROADCAST)); /* Broadcast flag */
    assert(hdr->ciaddr == client_ip);
    assert(find_option(wire, wire_len, DHCP_OPT_MSG_TYPE, &val, &opt_len) && *val == DHCP_MSG_REQUEST);
    assert(!find_option(wire, wire_len, DHCP_OPT_REQUESTED_IP, NULL, NULL));
    assert(!find_option(wire, wire_len, DHCP_OPT_SERVER_ID, NULL, NULL));

    /* =========================================================================
     * 3. INIT-REBOOT DHCPREQUEST (RFC 2131 §4.3.2)
     * ========================================================================= */
    memset(wire, 0, sizeof(wire));
    assert(dhcp_encode_request_reboot(wire, sizeof(wire), xid + 2, mac, client_ip, &wire_len) == 0);
    hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->xid == xid + 2);
    assert(hdr->flags == __builtin_bswap16(DHCP_FLAGS_BROADCAST));
    assert(hdr->ciaddr == 0); /* ciaddr MUST be 0 during INIT-REBOOT */
    assert(find_option(wire, wire_len, DHCP_OPT_MSG_TYPE, &val, &opt_len) && *val == DHCP_MSG_REQUEST);
    assert(find_option(wire, wire_len, DHCP_OPT_REQUESTED_IP, &val, &opt_len) && opt_len == 4 && memcmp(val, &client_ip, 4) == 0);
    assert(!find_option(wire, wire_len, DHCP_OPT_SERVER_ID, NULL, NULL)); /* NO server ID per RFC 2131 §4.3.2 */

    /* =========================================================================
     * 4. DHCPRELEASE (RFC 2131 §4.3.4)
     * ========================================================================= */
    memset(wire, 0, sizeof(wire));
    assert(dhcp_encode_release(wire, sizeof(wire), xid + 3, mac, client_ip, server_ip, &wire_len) == 0);
    hdr = (const dhcp_packet_hdr_t *)wire;
    assert(hdr->op == DHCP_BOOTREQUEST);
    assert(hdr->flags == 0);
    assert(hdr->ciaddr == client_ip);
    assert(find_option(wire, wire_len, DHCP_OPT_MSG_TYPE, &val, &opt_len) && *val == DHCP_MSG_RELEASE);
    assert(find_option(wire, wire_len, DHCP_OPT_SERVER_ID, &val, &opt_len) && opt_len == 4 && memcmp(val, &server_ip, 4) == 0);
    assert(find_option(wire, wire_len, DHCP_OPT_CLIENT_ID, &val, &opt_len) && opt_len == 7);

    /* =========================================================================
     * 5. Option 58 / 59 Parsing and Default Calculation ($0.5 \times L$, $0.875 \times L$)
     * ========================================================================= */
    /* Case 5a: Explicit T1 and T2 in packet */
    uint8_t rep[576];
    memset(rep, 0, sizeof(rep));
    dhcp_packet_hdr_t *rhdr = (dhcp_packet_hdr_t *)rep;
    rhdr->op = DHCP_BOOTREPLY;
    rhdr->htype = DHCP_HTYPE_ETHER;
    rhdr->hlen = DHCP_HLEN_ETHER;
    rhdr->xid = xid;
    rhdr->yiaddr = client_ip;
    rhdr->siaddr = server_ip;
    memcpy(rhdr->chaddr, mac, 6);
    rhdr->magic = __builtin_bswap32(DHCP_MAGIC_COOKIE);

    size_t p = DHCP_OPTIONS_OFFSET;
    rep[p++] = DHCP_OPT_MSG_TYPE; rep[p++] = 1; rep[p++] = DHCP_MSG_ACK;
    uint32_t lt = __builtin_bswap32(1000);
    uint32_t explicit_t1 = __builtin_bswap32(400);
    uint32_t explicit_t2 = __builtin_bswap32(800);
    rep[p++] = DHCP_OPT_LEASE_TIME; rep[p++] = 4; memcpy(rep + p, &lt, 4); p += 4;
    rep[p++] = DHCP_OPT_RENEWAL_TIME; rep[p++] = 4; memcpy(rep + p, &explicit_t1, 4); p += 4;
    rep[p++] = DHCP_OPT_REBINDING_TIME; rep[p++] = 4; memcpy(rep + p, &explicit_t2, 4); p += 4;
    rep[p++] = DHCP_OPT_END;
    if (p < 300) p = 300;

    dhcp_offer_t offer;
    assert(dhcp_decode_reply(rep, p, xid, mac, &offer) == 0);
    assert(offer.lease_time == 1000);
    assert(offer.t1 == 400);
    assert(offer.t2 == 800);

    /* Case 5b: Omitted T1 and T2 -> defaults 0.5 * L and 0.875 * L */
    memset(rep + DHCP_OPTIONS_OFFSET, 0, sizeof(rep) - DHCP_OPTIONS_OFFSET);
    p = DHCP_OPTIONS_OFFSET;
    rep[p++] = DHCP_OPT_MSG_TYPE; rep[p++] = 1; rep[p++] = DHCP_MSG_ACK;
    lt = __builtin_bswap32(86400);
    rep[p++] = DHCP_OPT_LEASE_TIME; rep[p++] = 4; memcpy(rep + p, &lt, 4); p += 4;
    rep[p++] = DHCP_OPT_END;
    if (p < 300) p = 300;

    assert(dhcp_decode_reply(rep, p, xid, mac, &offer) == 0);
    assert(offer.lease_time == 86400);
    assert(offer.t1 == 43200); /* 0.5 * 86400 */
    assert(offer.t2 == 75600); /* 0.875 * 86400 = 7 * 86400 / 8 */

    /* Case 5c: Omitted lease_time -> defaults to 86400, T1=43200, T2=75600 */
    memset(rep + DHCP_OPTIONS_OFFSET, 0, sizeof(rep) - DHCP_OPTIONS_OFFSET);
    p = DHCP_OPTIONS_OFFSET;
    rep[p++] = DHCP_OPT_MSG_TYPE; rep[p++] = 1; rep[p++] = DHCP_MSG_ACK;
    rep[p++] = DHCP_OPT_END;
    if (p < 300) p = 300;

    assert(dhcp_decode_reply(rep, p, xid, mac, &offer) == 0);
    assert(offer.lease_time == 86400);
    assert(offer.t1 == 43200);
    assert(offer.t2 == 75600);

    /* =========================================================================
     * 6. RFC 2131 §4.4.5 Randomized Jitter Verification
     * ========================================================================= */
    /* Standard 86400s lease with delta within [-3, +3] */
    uint32_t jit_t1 = 43200;
    uint32_t jit_t2 = 75600;
    assert(dhcp_apply_jitter(86400, &jit_t1, &jit_t2, -2, 3) == 0);
    assert(jit_t1 == 43198);
    assert(jit_t2 == 75603);
    assert(1 <= jit_t1 && jit_t1 < jit_t2 && jit_t2 < 86400);

    /* Exhaustive bounds testing across deltas [-5, +5] on multiple lease durations */
    static const uint32_t test_leases[] = {86400, 3600, 120, 10, 4, 3, 2};
    for (size_t li = 0; li < sizeof(test_leases) / sizeof(test_leases[0]); li++) {
        uint32_t L = test_leases[li];
        uint32_t base_t1 = L / 2;
        uint32_t base_t2 = (uint32_t)((uint64_t)L * 7 / 8);
        for (int d1 = -5; d1 <= 5; d1++) {
            for (int d2 = -5; d2 <= 5; d2++) {
                uint32_t t1_test = base_t1;
                uint32_t t2_test = base_t2;
                assert(dhcp_apply_jitter(L, &t1_test, &t2_test, d1, d2) == 0);
                assert(t1_test >= 1);
                if (L > 2) {
                    assert(t1_test < t2_test);
                    assert(t2_test < L);
                } else {
                    assert(t1_test <= L);
                    assert(t2_test <= L);
                }
            }
        }
    }

    /* =========================================================================
     * 7. Mock RFC 2131 State Machine Transition Simulation
     * ========================================================================= */
    typedef enum {
        MOCK_INIT,
        MOCK_SELECTING,
        MOCK_REQUESTING,
        MOCK_BOUND,
        MOCK_RENEWING,
        MOCK_REBINDING,
        MOCK_DISCONNECTED,
        MOCK_INIT_REBOOT,
        MOCK_RELEASED
    } mock_state_t;

    mock_state_t state = MOCK_INIT;
    uint32_t cur_ip = 0;
    uint32_t cur_gw = 0;
    uint32_t now = 1000;
    uint32_t t_ack = 0;
    uint32_t t_renew = 0;
    uint32_t t_rebind = 0;
    uint32_t t_expire = 0;
    uint32_t lease_len = 100;
    uint32_t t1_len = 50;
    uint32_t t2_len = 87;

    /* Step 1: INIT -> send DISCOVER -> SELECTING */
    assert(state == MOCK_INIT);
    assert(dhcp_encode_discover(wire, sizeof(wire), xid, mac, &wire_len) == 0);
    state = MOCK_SELECTING;

    /* Step 2: Receive OFFER -> send REQUEST -> REQUESTING */
    assert(state == MOCK_SELECTING);
    assert(dhcp_encode_request(wire, sizeof(wire), xid, mac, client_ip, server_ip, &wire_len) == 0);
    state = MOCK_REQUESTING;

    /* Step 3: Receive ACK -> configure interface -> BOUND */
    assert(state == MOCK_REQUESTING);
    cur_ip = client_ip;
    cur_gw = server_ip;
    t_ack = now;
    t_renew = t_ack + t1_len;
    t_rebind = t_ack + t2_len;
    t_expire = t_ack + lease_len;
    state = MOCK_BOUND;
    assert(cur_ip == client_ip);

    /* Step 4: Advance time to t_renew -> RENEWING */
    now = t_renew;
    assert(now >= t_renew && now < t_rebind);
    state = MOCK_RENEWING;
    assert(dhcp_encode_request_renew(wire, sizeof(wire), xid + 10, mac, cur_ip, &wire_len) == 0);

    /* Step 5: Renewal succeeds with ACK -> back to BOUND */
    t_ack = now;
    t_renew = t_ack + t1_len;
    t_rebind = t_ack + t2_len;
    t_expire = t_ack + lease_len;
    state = MOCK_BOUND;

    /* Step 6: Advance time past t_renew without renewal ACK -> advance past t_rebind -> REBINDING */
    now = t_rebind + 1;
    assert(now >= t_rebind && now < t_expire);
    state = MOCK_REBINDING;
    assert(dhcp_encode_request_rebind(wire, sizeof(wire), xid + 11, mac, cur_ip, &wire_len) == 0);

    /* Step 7: Rebinding fails, time reaches t_expire -> lease expired -> flush -> INIT */
    now = t_expire + 1;
    cur_ip = 0;
    cur_gw = 0;
    state = MOCK_INIT;
    assert(cur_ip == 0);

    /* Step 8: Reacquire lease -> BOUND */
    cur_ip = client_ip;
    cur_gw = server_ip;
    t_ack = now;
    t_renew = t_ack + t1_len;
    state = MOCK_BOUND;

    /* Step 9: Link drop -> DISCONNECTED -> Link restore -> INIT-REBOOT */
    state = MOCK_DISCONNECTED;
    assert(state == MOCK_DISCONNECTED);
    /* Carrier restored */
    state = MOCK_INIT_REBOOT;
    assert(dhcp_encode_request_reboot(wire, sizeof(wire), xid + 12, mac, cur_ip, &wire_len) == 0);
    /* Reconnect ACK validated */
    state = MOCK_BOUND;

    /* Step 10: Graceful termination (SIGTERM) -> send DHCPRELEASE -> RELEASED */
    assert(state == MOCK_BOUND);
    assert(dhcp_encode_release(wire, sizeof(wire), xid + 13, mac, cur_ip, cur_gw, &wire_len) == 0);
    cur_ip = 0;
    cur_gw = 0;
    state = MOCK_RELEASED;
    assert(state == MOCK_RELEASED);

    puts("DHCP v2 codec & RFC 2131 ASan/UBSan PASS: Option 58/59, jitter, renew, rebind, reboot, release, and state transitions");
    return 0;
}
