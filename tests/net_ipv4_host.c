#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net_ipv4.h"
#include "ipv4.h"
#include "icmp.h"
#include "checksum.h"
static net_dev_t dev;
bool net_tcp_idle(void) { return true; }
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len) {
    (void)source; (void)data; (void)len;
}
void net_socket_input(uint32_t ip, uint16_t sp, uint16_t dp, const uint8_t *data, size_t len) {
    (void)ip; (void)sp; (void)dp; (void)data; (void)len;
}
static uint64_t ticks;
static unsigned sends, requests;
static uint32_t cached_ip;
static bool fail;
static uint8_t frame[1514]; static size_t frame_len;
static const uint8_t peer[6]={2,3,4,5,6,7};
uint64_t apic_timer_get_bsp_ticks(void) { return ticks; }
uint64_t apic_timer_get_frequency(void) { return 100; }
int net_arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]) { if (ip!=cached_ip) return -1; memcpy(mac,peer,6); return 0; }
int arp_resolve(net_dev_t *d, uint32_t ip, uint8_t mac[ETH_ALEN]) {
    assert(d==&dev); ++requests; return fail ? -1 : net_arp_lookup(ip,mac) ? 1 : 0;
}
static int send_packet(net_dev_t *d, const void *buf, size_t len) {
    assert(d==&dev && len<=1514 && len>=60); ++sends; memcpy(frame,buf,len); frame_len=len; return fail ? -1 : 0;
}
static uint8_t rx[1500];
static void echo(uint32_t src, uint32_t dest, unsigned data_len) {
    memset(rx,0,sizeof(rx));
    assert(!icmp_echo_encode(rx+20,1480,8,0x1234,7,"123456789",data_len));
    assert(!ipv4_encode(rx,1500,src,dest,1,(uint16_t)(8+data_len),64,NULL));
}
int main(void) {
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    dev=(net_dev_t){.mac_addr={0x52,0x54,0,0x12,0x34,0x56},.mtu=1500,.send_packet=send_packet};
    net_ipv4_init(&dev,&cfg); cached_ip=cfg.gateway;
    echo(cfg.gateway,cfg.local_ip,9); net_ipv4_input(rx,37); net_ipv4_tick(0);
    assert(sends==1 && frame_len==60 && !memcmp(frame,peer,6));
    ipv4_header_t ip; const uint8_t *payload; size_t len; icmp_echo_t e;
    assert(!ipv4_decode(frame+14,46,&ip,&payload,&len) && ip.dst_ip==cfg.gateway && ntohs(ip.total_len)==37);
    const uint8_t *data; size_t n;
    assert(!icmp_echo_decode(payload,len,&e,&data,&n) && e.type==0 && e.identifier==0x1234 && e.sequence==7);
    assert(n==9 && !memcmp(data,"123456789",9));
    for (size_t i=51; i<60; ++i) assert(!frame[i]);
    for (unsigned which=0; which<7; ++which) {
        echo(cfg.gateway,cfg.local_ip,0);
        ipv4_header_t *h=(void *)rx;
        if (which==0) rx[22]^=1;
        if (which==1) h->ttl=0;
        if (which==2) h->flags_frag=htons(IPV4_FLAG_MF);
        if (which==3) h->dst_ip=htonl(0xffffffff);
        if (which==4) h->version_ihl=0x46;
        if (which==5) h->src_ip=0;
        if (which==6) h->protocol=17;
        h->checksum=ipv4_calculate_checksum(h);
        net_ipv4_input(rx,60); net_ipv4_tick(1); assert(sends==1);
    }
    cached_ip=0; echo(cfg.gateway,cfg.local_ip,0); net_ipv4_input(rx,28); net_ipv4_input(rx,28);
    for (ticks=0; ticks<=300; ++ticks) net_ipv4_tick(ticks);
    assert(requests==3 && sends==1);
    cached_ip=cfg.gateway;
    net_ping_v1_t request={.version=1,.destination=htonl(0xc0000201),.timeout_seconds=1,.sequence=4};
    assert(net_ipv4_ping_start(&request,55,400)); net_ipv4_tick(400);
    assert(sends==2 && !memcmp(frame,peer,6));
    assert(!ipv4_decode(frame+14,frame_len-14,&ip,&payload,&len) && ip.dst_ip==request.destination);
    memcpy(rx,frame+14,frame_len-14);
    ipv4_header_t *h=(void *)rx; h->src_ip=request.destination; h->dst_ip=cfg.local_ip; h->checksum=ipv4_calculate_checksum(h);
    assert(!icmp_echo_decode(rx+20,40,&e,&data,&n));
    assert(!icmp_echo_encode(rx+20,1480,0,e.identifier,e.sequence,data,n));
    ticks=405; rx[28]^=1; net_ipv4_input(rx,60);
    net_ping_v1_t result; assert(!net_ipv4_ping_take(&result));
    rx[28]^=1; net_ipv4_input(rx,60); assert(net_ipv4_ping_take(&result));
    assert(result.outcome==0 && result.rtt_ticks==5 && result.echoed_bytes==32);
    assert(net_ipv4_ping_start(&request,56,500)); net_ipv4_tick(500); net_ipv4_tick(600);
    assert(net_ipv4_ping_take(&result) && result.outcome==NETPING_ECHO_TIMEOUT);
    fail=true; assert(net_ipv4_ping_start(&request,57,700)); net_ipv4_tick(700);
    assert(net_ipv4_ping_take(&result) && result.outcome==NETPING_TX_FAILED); fail=false;
    request.start_delay_ms=1000; unsigned before=sends;
    assert(net_ipv4_ping_start(&request,58,800)); net_ipv4_tick(899); assert(sends==before);
    net_ipv4_tick(900); assert(sends==before+1); net_ipv4_ping_cancel(58);
    puts("IPv4 stack ASan/UBSan PASS: echo bytes/padding, filters, routes, bounded ARP, saturation, matching, pacing/timeouts/TX failure");
}
