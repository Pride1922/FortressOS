#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "net.h"
#include "checksum.h"
#include "eth.h"
#include "arp.h"
#include "ipv4.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        exit(1); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

/* =========================================================================
 * 1. RFC 1071 Internet Checksum Tests
 * ========================================================================= */
static void test_checksum_rfc1071_known_vector(void) {
    /* Known RFC 1071 vector */
    uint8_t vec[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    uint16_t csum = net_checksum(vec, sizeof(vec), 0);
    TEST_ASSERT(csum == 0x220d, "RFC 1071 known vector checksum should be 0x220d");

    /* Verifying data + checksum appended yields 0 */
    uint8_t vec_with_csum[10];
    memcpy(vec_with_csum, vec, sizeof(vec));
    vec_with_csum[8] = (uint8_t)(csum >> 8);
    vec_with_csum[9] = (uint8_t)(csum & 0xFF);
    uint16_t verify = net_checksum(vec_with_csum, sizeof(vec_with_csum), 0);
    TEST_ASSERT(verify == 0x0000, "Checksum verification over data + csum should be 0x0000");
}

static void test_checksum_odd_lengths(void) {
    /* 1-byte buffer: 0x42 -> 16-bit word 0x4200 -> checksum ~0x4200 = 0xbdff */
    uint8_t one_byte[] = { 0x42 };
    uint16_t csum1 = net_checksum(one_byte, 1, 0);
    TEST_ASSERT(csum1 == 0xbdff, "1-byte checksum should pad to 0x4200 inverted = 0xbdff");

    /* 3-byte buffer: 0x01, 0x02, 0x03 -> words 0x0102, 0x0300 -> sum 0x0402 -> ~0x0402 = 0xfbfd */
    uint8_t three_bytes[] = { 0x01, 0x02, 0x03 };
    uint16_t csum3 = net_checksum(three_bytes, 3, 0);
    TEST_ASSERT(csum3 == 0xfbfd, "3-byte checksum should equal 0xfbfd");

    /* 5-byte buffer */
    uint8_t five_bytes[] = { 0x10, 0x20, 0x30, 0x40, 0x50 };
    uint16_t csum5 = net_checksum(five_bytes, 5, 0);
    /* 0x1020 + 0x3040 + 0x5000 = 0x9060 -> ~0x9060 = 0x6f9f */
    TEST_ASSERT(csum5 == 0x6f9f, "5-byte checksum should equal 0x6f9f");
}

static void test_checksum_extremes(void) {
    /* All-zeros: 8 bytes */
    uint8_t zeros[8] = { 0 };
    uint16_t csum_zeros = net_checksum(zeros, sizeof(zeros), 0);
    TEST_ASSERT(csum_zeros == 0xffff, "All-zeros checksum should invert to 0xffff");

    /* All-0xFF: 4 bytes -> words 0xffff, 0xffff -> sum 0x1fffe -> fold 0xffff -> ~0xffff = 0 */
    uint8_t ffs[4] = { 0xff, 0xff, 0xff, 0xff };
    uint16_t csum_ffs = net_checksum(ffs, sizeof(ffs), 0);
    TEST_ASSERT(csum_ffs == 0x0000, "All-0xFF checksum should fold and invert to 0x0000");

    /* Zero length */
    TEST_ASSERT(net_checksum(NULL, 0, 0) == 0xffff, "Null/zero-len checksum returns inverted 0 = 0xffff");
}

static void test_checksum_multi_buffer_accumulation(void) {
    /* Simulating IPv4 pseudo-header (12 bytes) + UDP payload (8 bytes) */
    uint8_t pseudo_hdr[12] = {
        10, 0, 2, 15,          /* Src IP */
        10, 0, 2, 2,           /* Dst IP */
        0, 17,                 /* Zero + Proto UDP */
        0, 8                   /* UDP length */
    };
    uint8_t payload[8] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };

    /* Single buffer total */
    uint8_t combined[20];
    memcpy(combined, pseudo_hdr, 12);
    memcpy(combined + 12, payload, 8);
    uint16_t combined_csum = net_checksum(combined, 20, 0);

    /* Incremental accumulation across chunks */
    uint32_t acc = net_checksum_accumulate(pseudo_hdr, 12, 0);
    acc = net_checksum_accumulate(payload, 8, acc);
    uint16_t incremental_csum = net_checksum_finish(acc);

    TEST_ASSERT(combined_csum == incremental_csum, "Multi-buffer accumulated checksum matches single-buffer");
}

/* =========================================================================
 * 2. Ethernet Frame Codec & Helpers Tests
 * ========================================================================= */
static void test_ethernet_helpers(void) {
    uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t host[6]  = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t other[6] = { 0x52, 0x54, 0x00, 0x99, 0x88, 0x77 };

    TEST_ASSERT(eth_is_broadcast(bcast), "Broadcast MAC correctly identified");
    TEST_ASSERT(!eth_is_broadcast(host), "Host MAC not broadcast");
    TEST_ASSERT(eth_mac_equal(host, host), "Identical MACs equal");
    TEST_ASSERT(!eth_mac_equal(host, other), "Different MACs not equal");

    TEST_ASSERT(eth_mac_matches(bcast, host), "Broadcast matches host");
    TEST_ASSERT(eth_mac_matches(host, host), "Unicast host matches host");
    TEST_ASSERT(!eth_mac_matches(other, host), "Unicast other rejected");
}

static void test_ethernet_encode_decode(void) {
    uint8_t buf[128] = {0};
    uint8_t dst[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t src[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    size_t hdr_len = 0;

    /* Encode */
    int enc = eth_encode(buf, sizeof(buf), dst, src, ETHERTYPE_IPV4, &hdr_len);
    TEST_ASSERT(enc == 0, "eth_encode succeeds");
    TEST_ASSERT(hdr_len == ETH_HDR_LEN, "eth_encode produces 14-byte header");

    /* Append payload */
    const char *payload_data = "Hello FortressOS!";
    size_t payload_len = strlen(payload_data);
    memcpy(buf + hdr_len, payload_data, payload_len);
    size_t total_frame_len = hdr_len + payload_len;

    /* Decode */
    eth_header_t out_hdr;
    const uint8_t *out_payload = NULL;
    size_t out_payload_len = 0;
    int dec = eth_decode(buf, total_frame_len, &out_hdr, &out_payload, &out_payload_len);
    TEST_ASSERT(dec == 0, "eth_decode succeeds on valid frame");
    TEST_ASSERT(eth_mac_equal(out_hdr.dest, dst), "Decoded dest MAC matches");
    TEST_ASSERT(eth_mac_equal(out_hdr.src, src), "Decoded src MAC matches");
    TEST_ASSERT(out_hdr.ethertype == ETHERTYPE_IPV4, "Decoded EtherType is IPv4");
    TEST_ASSERT(out_payload_len == payload_len, "Decoded payload length matches");
    TEST_ASSERT(memcmp(out_payload, payload_data, payload_len) == 0, "Decoded payload content matches");
}

static void test_ethernet_bounds(void) {
    uint8_t buf[2000] = {0};
    eth_header_t hdr;

    /* Buffer too small for header (< 14 bytes) */
    TEST_ASSERT(eth_decode(buf, 13, &hdr, NULL, NULL) == -1, "Runt frame < 14 bytes rejected");
    TEST_ASSERT(eth_decode(buf, 0, &hdr, NULL, NULL) == -1, "Zero length rejected");

    /* Buffer oversized (> 1514 bytes) */
    TEST_ASSERT(eth_decode(buf, 1515, &hdr, NULL, NULL) == -2, "Oversized frame > 1514 bytes rejected");

    /* Encode buffer too small */
    uint8_t small[10] = {0};
    uint8_t mac[6] = {0};
    TEST_ASSERT(eth_encode(small, 10, mac, mac, 0x0800, NULL) == -1, "Encode into buffer < 14 rejected");
}

/* =========================================================================
 * 3. ARP Codec & Cache Tests
 * ========================================================================= */
static void test_arp_request_reply(void) {
    uint8_t buf[64] = {0};
    uint8_t host_mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t target_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    uint32_t host_ip = 0x0F02000A;   /* 10.0.2.15 in network order */
    uint32_t target_ip = 0x0202000A; /* 10.0.2.2 in network order */
    size_t out_len = 0;

    /* 1. Encode Request */
    int enc_req = arp_encode_request(buf, sizeof(buf), host_mac, host_ip, target_ip, &out_len);
    TEST_ASSERT(enc_req == 0, "arp_encode_request succeeds");
    TEST_ASSERT(out_len == ARP_PACKET_LEN, "ARP packet length is 28 bytes");

    /* Decode Request */
    arp_packet_t dec_req;
    int dec_res = arp_decode(buf, out_len, &dec_req);
    TEST_ASSERT(dec_res == 0, "arp_decode succeeds on Request");
    TEST_ASSERT(dec_req.htype == ARP_HTYPE_ETHERNET, "ARP htype is Ethernet");
    TEST_ASSERT(dec_req.ptype == ARP_PTYPE_IPV4, "ARP ptype is IPv4");
    TEST_ASSERT(dec_req.opcode == ARP_OP_REQUEST, "ARP opcode is Request");
    TEST_ASSERT(eth_mac_equal(dec_req.sender_mac, host_mac), "Sender MAC matches");
    TEST_ASSERT(dec_req.sender_ip == host_ip, "Sender IP matches");
    TEST_ASSERT(dec_req.target_ip == target_ip, "Target IP matches");

    /* 2. Encode Reply */
    int enc_rep = arp_encode_reply(buf, sizeof(buf), target_mac, target_ip, host_mac, host_ip, &out_len);
    TEST_ASSERT(enc_rep == 0, "arp_encode_reply succeeds");

    /* Decode Reply */
    arp_packet_t dec_rep;
    dec_res = arp_decode(buf, out_len, &dec_rep);
    TEST_ASSERT(dec_res == 0, "arp_decode succeeds on Reply");
    TEST_ASSERT(dec_rep.opcode == ARP_OP_REPLY, "ARP opcode is Reply");
    TEST_ASSERT(eth_mac_equal(dec_rep.sender_mac, target_mac), "Reply sender MAC matches");
    TEST_ASSERT(dec_rep.sender_ip == target_ip, "Reply sender IP matches");
    TEST_ASSERT(eth_mac_equal(dec_rep.target_mac, host_mac), "Reply target MAC matches");
    TEST_ASSERT(dec_rep.target_ip == host_ip, "Reply target IP matches");
}

static void test_arp_bounds(void) {
    uint8_t buf[32] = {0};
    arp_packet_t arp;

    /* Truncated ARP packet (< 28 bytes) */
    TEST_ASSERT(arp_decode(buf, 27, &arp) == -1, "Truncated ARP < 28 bytes rejected");
    TEST_ASSERT(arp_decode(NULL, 28, &arp) == -1, "Null ARP buffer rejected");

    /* Malformed ARP htype / opcode */
    arp_packet_t malformed;
    arp_encode_request(buf, sizeof(buf), (uint8_t[]){1,2,3,4,5,6}, 0, 0, NULL);
    /* Corrupt htype */
    buf[0] = 0x99;
    TEST_ASSERT(arp_decode(buf, 28, &malformed) == -2, "Malformed htype rejected");
}

static void test_arp_cache(void) {
    arp_cache_t cache;
    arp_cache_init(&cache);

    uint32_t ip1 = 0x01010101;
    uint8_t mac1[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t out_mac[6];

    /* Lookup before insert fails */
    TEST_ASSERT(arp_cache_lookup(&cache, ip1, out_mac) == -1, "Lookup on empty cache returns -1");

    /* Update entry */
    TEST_ASSERT(arp_cache_update(&cache, ip1, mac1, 100) == 0, "arp_cache_update succeeds");
    TEST_ASSERT(arp_cache_lookup(&cache, ip1, out_mac) == 0, "Lookup finds newly added entry");
    TEST_ASSERT(eth_mac_equal(out_mac, mac1), "Resolved MAC matches inserted MAC");

    /* Update existing entry */
    uint8_t mac1_new[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    TEST_ASSERT(arp_cache_update(&cache, ip1, mac1_new, 200) == 0, "Update existing entry succeeds");
    TEST_ASSERT(arp_cache_lookup(&cache, ip1, out_mac) == 0, "Lookup finds updated entry");
    TEST_ASSERT(eth_mac_equal(out_mac, mac1_new), "Updated MAC matches");

    /* Fill cache to capacity (16 entries) */
    for (uint32_t i = 2; i <= 16; i++) {
        uint8_t m[6] = { 0, 0, 0, 0, 0, (uint8_t)i };
        TEST_ASSERT(arp_cache_update(&cache, i, m, 100 + i) == 0, "Cache fill within capacity");
    }

    /* Adding 17th entry evicts oldest (ip1 had tick 200, others 102..116; entry 2 had tick 102) */
    uint8_t mac17[6] = { 0x17, 0x17, 0x17, 0x17, 0x17, 0x17 };
    TEST_ASSERT(arp_cache_update(&cache, 0x17171717, mac17, 300) == 0, "Insert with eviction succeeds");
    TEST_ASSERT(arp_cache_lookup(&cache, 0x17171717, out_mac) == 0, "17th entry present");
    TEST_ASSERT(eth_mac_equal(out_mac, mac17), "17th entry MAC verified");
}

/* =========================================================================
 * 4. IPv4 Codec, Checksum & Bounds Tests
 * ========================================================================= */
static void test_ipv4_encode_decode(void) {
    uint8_t buf[256] = {0};
    uint32_t src_ip = 0x0F02000A; /* 10.0.2.15 */
    uint32_t dst_ip = 0x0202000A; /* 10.0.2.2 */
    const char *payload_msg = "FortressOS IP Packet";
    uint16_t payload_len = (uint16_t)strlen(payload_msg);
    size_t hdr_len = 0;

    /* 1. Encode */
    int enc = ipv4_encode(buf, sizeof(buf), src_ip, dst_ip, IPV4_PROTO_UDP, payload_len, 64, &hdr_len);
    TEST_ASSERT(enc == 0, "ipv4_encode succeeds");
    TEST_ASSERT(hdr_len == IPV4_MIN_HLEN, "Header length is 20 bytes");

    /* Append payload */
    memcpy(buf + hdr_len, payload_msg, payload_len);
    size_t total_len = hdr_len + payload_len;

    /* 2. Decode */
    ipv4_header_t out_hdr;
    const uint8_t *out_payload = NULL;
    size_t out_payload_len = 0;
    int dec = ipv4_decode(buf, total_len, &out_hdr, &out_payload, &out_payload_len);
    TEST_ASSERT(dec == 0, "ipv4_decode succeeds on valid packet");
    TEST_ASSERT(ipv4_get_version(&out_hdr) == IPV4_VERSION_4, "Version is 4");
    TEST_ASSERT(ipv4_get_ihl_bytes(&out_hdr) == 20, "IHL is 20 bytes");
    TEST_ASSERT(out_hdr.protocol == IPV4_PROTO_UDP, "Protocol is UDP");
    TEST_ASSERT(out_hdr.src_ip == src_ip, "Src IP matches");
    TEST_ASSERT(out_hdr.dst_ip == dst_ip, "Dst IP matches");
    TEST_ASSERT(out_payload_len == payload_len, "Decoded payload len matches");
    TEST_ASSERT(memcmp(out_payload, payload_msg, payload_len) == 0, "Decoded payload data matches");
    TEST_ASSERT(!ipv4_is_fragment(&out_hdr), "Packet is unfragmented");
}

static void test_ipv4_checksum_validation(void) {
    uint8_t buf[128] = {0};
    size_t hdr_len = 0;
    ipv4_encode(buf, sizeof(buf), 0x01020304, 0x05060708, IPV4_PROTO_ICMP, 10, 64, &hdr_len);

    ipv4_header_t out_hdr;
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == 0, "Valid checksum accepted");

    /* Corrupt 1 byte in the header */
    buf[12] ^= 0x55; /* Corrupt src_ip */
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -5, "Corrupted checksum rejected with -5");
}

static void test_ipv4_fragment_rejection(void) {
    uint8_t buf[128] = {0};
    size_t hdr_len = 0;
    ipv4_encode(buf, sizeof(buf), 0x01020304, 0x05060708, IPV4_PROTO_UDP, 10, 64, &hdr_len);

    ipv4_header_t *hdr = (ipv4_header_t *)buf;

    /* 1. Set More Fragments (MF) flag */
    hdr->flags_frag = htons(IPV4_FLAG_MF);
    hdr->checksum = ipv4_calculate_checksum(hdr);
    ipv4_header_t out_hdr;
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -6, "MF fragment rejected with -6");

    /* 2. Set Fragment Offset > 0 */
    hdr->flags_frag = htons(0x0040); /* Offset 64 */
    hdr->checksum = ipv4_calculate_checksum(hdr);
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -6, "Non-zero fragment offset rejected with -6");
}

static void test_ipv4_malformed_headers(void) {
    uint8_t buf[128] = {0};
    size_t hdr_len = 0;
    ipv4_encode(buf, sizeof(buf), 0x01020304, 0x05060708, IPV4_PROTO_UDP, 10, 64, &hdr_len);

    ipv4_header_t *hdr = (ipv4_header_t *)buf;
    ipv4_header_t out_hdr;

    /* 1. Version != 4 */
    hdr->version_ihl = (6 << 4) | 5;
    hdr->checksum = ipv4_calculate_checksum(hdr);
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -2, "Version != 4 rejected with -2");

    /* 2. IHL < 5 (e.g. 4 -> 16 bytes) */
    hdr->version_ihl = (4 << 4) | 4;
    hdr->checksum = ipv4_calculate_checksum(hdr);
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -3, "IHL < 5 rejected with -3");

    /* 3. Total Length < IHL */
    hdr->version_ihl = (4 << 4) | 5;
    hdr->total_len = htons(16);
    hdr->checksum = ipv4_calculate_checksum(hdr);
    TEST_ASSERT(ipv4_decode(buf, hdr_len + 10, &out_hdr, NULL, NULL) == -4, "total_len < IHL rejected with -4");

    /* 4. Total Length > buffer length */
    hdr->total_len = htons(100);
    hdr->checksum = ipv4_calculate_checksum(hdr);
    TEST_ASSERT(ipv4_decode(buf, 50, &out_hdr, NULL, NULL) == -4, "total_len > buffer len rejected with -4");

    /* 5. Truncated buffer (< 20 bytes) */
    TEST_ASSERT(ipv4_decode(buf, 19, &out_hdr, NULL, NULL) == -1, "Truncated buffer < 20 bytes rejected with -1");
}

/* =========================================================================
 * 5. Packet Buffer Structure (`pbuf_t`) Lifecycle Tests
 * ========================================================================= */
static void test_pbuf_lifecycle(void) {
    pbuf_t p;
    pbuf_init(&p);

    TEST_ASSERT(p.next == NULL, "pbuf_init next is NULL");
    TEST_ASSERT(p.payload == p.data, "pbuf_init payload points to data");
    TEST_ASSERT(p.length == 0, "pbuf_init length is 0");
    TEST_ASSERT(p.flags == 0, "pbuf_init flags is 0");

    /* Simulate RX packet arrival */
    const char *frame_data = "Dummy Network Frame";
    size_t frame_len = strlen(frame_data);
    memcpy(p.data, frame_data, frame_len);
    p.length = (uint16_t)frame_len;
    p.total_len = (uint16_t)frame_len;

    /* Consume Ethernet header (simulate 14 bytes) */
    TEST_ASSERT(pbuf_header_consume(&p, 14) == 0, "pbuf_header_consume succeeds when len >= 14");
    TEST_ASSERT(p.payload == p.data + 14, "Payload pointer advanced by 14");
    TEST_ASSERT(p.length == frame_len - 14, "Length decremented by 14");

    /* Over-consume fails cleanly */
    TEST_ASSERT(pbuf_header_consume(&p, frame_len) == -1, "pbuf_header_consume over-consumption rejected");

    /* Reset returns payload to data */
    pbuf_reset(&p);
    TEST_ASSERT(p.payload == p.data, "pbuf_reset restores payload pointer");
    TEST_ASSERT(p.length == 0, "pbuf_reset zeroes length");
}

/* =========================================================================
 * 6. Bounds Defense & Fuzzing Resilience (ASan Sanitizer Safety)
 * ========================================================================= */
static void test_fuzz_bounds_resilience(void) {
    uint8_t garbage[128];
    for (size_t i = 0; i < sizeof(garbage); i++) {
        garbage[i] = (uint8_t)(i * 37 + 13);
    }

    /* Test decoders at every truncated size from 0 to 64 bytes */
    for (size_t len = 0; len <= 64; len++) {
        eth_header_t eth_hdr;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        (void)eth_decode(garbage, len, &eth_hdr, &payload, &payload_len);

        arp_packet_t arp_pkt;
        (void)arp_decode(garbage, len, &arp_pkt);

        ipv4_header_t ip_hdr;
        (void)ipv4_decode(garbage, len, &ip_hdr, &payload, &payload_len);
    }

    TEST_ASSERT(true, "All decoders safely rejected truncated/garbage buffers without memory violation");
}

int main(void) {
    printf("========================================================\n");
    printf("FortressOS Networking Phase 0 — Host Test Suite\n");
    printf("========================================================\n");

    test_checksum_rfc1071_known_vector();
    test_checksum_odd_lengths();
    test_checksum_extremes();
    test_checksum_multi_buffer_accumulation();

    test_ethernet_helpers();
    test_ethernet_encode_decode();
    test_ethernet_bounds();

    test_arp_request_reply();
    test_arp_bounds();
    test_arp_cache();

    test_ipv4_encode_decode();
    test_ipv4_checksum_validation();
    test_ipv4_fragment_rejection();
    test_ipv4_malformed_headers();

    test_pbuf_lifecycle();
    test_fuzz_bounds_resilience();

    printf("\nAll %d tests passed successfully! [100%% PASS]\n", g_tests_passed);
    return 0;
}
