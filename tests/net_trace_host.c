/* Actual mailbox + IPv4 + ICMP; fake BSP clock/NIC/ARP and lock adapters.
 * Peer bytes/checksums below are independent of production encoders. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net_ping.h"
#include "net_ipv4.h"
#include "icmp.h"
#include "syscall_abi.h"

void net_test_assert_unheld(void);
static net_dev_t dev;
static net_config_t cfg;
static uint64_t ticks, hz=100;
static unsigned sends, wakes, arp;
static bool cached=true, send_fail;
static uint8_t frame[1514], previous[1514], rx[1500];
static const uint8_t peer[6]={2,3,4,5,6,7};
uint64_t apic_timer_get_bsp_ticks(void) { return ticks; }
uint64_t apic_timer_get_frequency(void) { return hz; }
void sched_wake_all(const void *channel) {
    net_test_assert_unheld(); assert(channel==&g_net_ping_channel); wakes++;
}
bool net_tcp_idle(void) { return true; }
void net_tcp_input(uint32_t ip,const uint8_t *p,size_t n) { (void)ip; (void)p; (void)n; }
void net_socket_input(uint32_t ip,uint16_t sp,uint16_t dp,const uint8_t *p,size_t n) {
    (void)ip; (void)sp; (void)dp; (void)p; (void)n;
}
int net_arp_lookup(uint32_t ip,uint8_t mac[6]) {
    net_test_assert_unheld(); assert(ip==cfg.gateway);
    if (!cached) return -1;
    memcpy(mac,peer,6); return 0;
}
int arp_resolve(net_dev_t *d,uint32_t ip,uint8_t mac[6]) {
    assert(d==&dev); arp++; return net_arp_lookup(ip,mac) ? 1:0;
}
static int send(net_dev_t *d,const void *p,size_t n) {
    net_test_assert_unheld(); assert(d==&dev && n==74); sends++;
    memcpy(frame,p,n); return send_fail ? -1:0;
}
static uint16_t sum(const uint8_t *p,size_t n) {
    uint32_t s=0;
    for (size_t i=0;i<n;i+=2) s+=(uint32_t)p[i]<<8 | (i+1<n ? p[i+1]:0);
    while (s>>16) s=(s&65535)+(s>>16);
    return (uint16_t)~s;
}
static void put(uint8_t *p,uint16_t n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static void checksum(uint8_t *p,size_t n,size_t offset) {
    p[offset]=p[offset+1]=0; put(p+offset,sum(p,n));
}
static void outer(uint32_t source,size_t payload) {
    memset(rx,0,20); rx[0]=0x45; put(rx+2,(uint16_t)(payload+20));
    rx[6]=0x40; rx[8]=64; rx[9]=1;
    memcpy(rx+12,&source,4); memcpy(rx+16,&cfg.local_ip,4); checksum(rx,20,10);
}
static void quote(const uint8_t *original,uint8_t type,uint8_t code) {
    memset(rx,0,sizeof(rx));
    rx[20]=type; rx[21]=code;
    memcpy(rx+28,original+14,28); checksum(rx+20,36,2);
    outer(cfg.gateway,36);
}
static void recheck_quote(void) { checksum(rx+28,20,10); checksum(rx+20,36,2); }
static void reply(void) {
    memcpy(rx+20,frame+34,40); rx[20]=0; checksum(rx+20,40,2);
    outer(htonl(0xc0000209),40);
}
static void step(uint64_t now) {
    ticks=now; net_ping_worker_tick(now); net_ipv4_tick(now); net_ping_worker_tick(now);
}
static net_trace_v1_t request(uint64_t deadline) {
    return (net_trace_v1_t){.version=1,.destination=htonl(0xc0000209),.ttl=1,
        .timeout_seconds=1,.sequence=1,.deadline_ticks=deadline};
}
static void codec(void) {
    quote(frame,11,0);
    icmp_quote_t q={.type=77}, before=q;
    for (size_t n=0;n<36;n++) { assert(icmp_quote_decode(rx+20,n,&q)); assert(!memcmp(&q,&before,sizeof(q))); }
    assert(!icmp_quote_decode(rx+20,36,&q) && q.type==11 && q.code==0 && q.ip_identifier==1);
    /* Original total length 60 > quoted bytes 28 must be accepted. */
    assert(rx[31]==60);
    for (unsigned i=0;i<12;i++) {
        quote(frame,11,0);
        if (i==0) rx[21]=1;
        if (i==1) rx[20]=4;
        if (i==2) rx[28]=0x44;
        if (i==3) rx[28]=0x46;
        if (i==4) rx[28]=0x65;
        if (i==5) rx[30]=rx[31]=0;
        if (i==6) rx[34]=0x20;
        if (i==7) rx[35]=1;
        if (i==8) rx[37]=17;
        if (i==9) rx[48]=0;
        if (i==10) rx[49]=1;
        recheck_quote(); if (i==11) rx[23]^=1;
        q=before; assert(icmp_quote_decode(rx+20,36,&q)); assert(!memcmp(&q,&before,sizeof(q)));
    }
    uint32_t seed=9;
    for (unsigned i=0;i<20000;i++) {
        uint8_t hostile[128];
        for (size_t j=0;j<sizeof(hostile);j++) { seed=seed*1664525+1013904223; hostile[j]=(uint8_t)(seed>>24); }
        q=before;
        int ret=icmp_quote_decode(hostile,i%129,&q);
        if (ret) assert(!memcmp(&q,&before,sizeof(q)));
    }
}
int main(void) {
    cfg=(net_config_t){.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    dev=(net_dev_t){.mac_addr={0x52,0x54,0,0x12,0x34,0x56},.mtu=1500,.send_packet=send};
    for (unsigned rate=0;rate<2;rate++) {
        hz=rate ? 1000:100; ticks=0;
        net_ipv4_init(&dev,&cfg); net_ping_init(true);
        net_trace_v1_t req=request(120*hz), result;
        uint64_t token, old, another;
        req.reserved0=1; assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL); req.reserved0=0;
        req.reserved1=1; assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL); req.reserved1=0;
        req.ttl=0; assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL); req.ttl=31;
        assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL); req.ttl=1;
        req.deadline_ticks=120*hz+1; assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL);
        req.deadline_ticks=0; assert(net_trace_submit(&req,&token)==SYSCALL_ETIMEDOUT); req.deadline_ticks=120*hz;
        req.responder=1; assert(net_trace_submit(&req,&token)==SYSCALL_EINVAL); req.responder=0;
        assert(!net_trace_submit(&req,&token)); step(0);
        assert(frame[22]==1 && !sum(frame+14,20) && !sum(frame+34,40));
        net_ping_v1_t ping={.version=1,.destination=req.destination,.timeout_seconds=1};
        assert(net_ping_submit(&ping,&another)==SYSCALL_EAGAIN);
        assert(net_trace_submit(&req,&another)==SYSCALL_EAGAIN);
        assert(net_ping_collect(token,&ping)==SYSCALL_EINTR);
        if (!rate) codec();
        /* Wrong quote tuple/id/IP-ID must not publish, even with valid sums. */
        for (unsigned i=0;i<6;i++) {
            quote(frame,11,0);
            if (i==0) rx[40]^=1;
            if (i==1) rx[44]^=1;
            if (i==2) rx[52]^=1;
            if (i==3) rx[54]^=1;
            if (i==4) rx[32]=rx[33]=0; /* a delayed ping quote */
            if (i==5) rx[23]^=1;
            if (i!=5) recheck_quote();
            ticks=hz/10; net_ipv4_input(rx,56); net_ping_worker_tick(ticks); assert(!net_ping_ready(&token));
        }
        quote(frame,11,0); net_ipv4_input(rx,56);
        ticks++; net_ipv4_input(rx,56); /* duplicate must not overwrite first RTT */
        net_ping_worker_tick(ticks);
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_HOP_EXPIRED && result.rtt_ticks==hz/10);
        memcpy(previous,frame,sizeof(previous)); old=token;
        req.sequence=2; req.ttl=2; assert(!net_trace_submit(&req,&token)); step(hz/5);
        assert(frame[22]==2);
        quote(previous,11,0); net_ipv4_input(rx,56); net_ping_worker_tick(ticks); assert(!net_ping_ready(&token));
        net_ping_cancel(old); assert(!net_ping_ready(&token));
        reply(); ticks++; net_ipv4_input(rx,60); net_ping_worker_tick(ticks);
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_REPLY);
        /* Whole-command deadline cuts a five-second probe short. */
        req=request(ticks+hz/2); req.timeout_seconds=5;
        assert(!net_trace_submit(&req,&token)); step(ticks);
        unsigned sent_before=sends; step(req.deadline_ticks);
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_PROBE_TIMEOUT && sends==sent_before);
        reply(); net_ipv4_input(rx,60); assert(net_trace_collect(token,&result)==SYSCALL_EINTR);
        /* ARP is bounded by the same shorter deadline, no TX after expiry. */
        cached=false; req=request(ticks+hz/2); req.timeout_seconds=5;
        assert(!net_trace_submit(&req,&token)); step(ticks); sent_before=sends; step(req.deadline_ticks);
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_PROBE_TIMEOUT && sends==sent_before);
        req=request(ticks+5*hz); req.timeout_seconds=5;
        assert(!net_trace_submit(&req,&token)); step(ticks); step(ticks+3*hz);
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_ARP_TIMEOUT);
        cached=true;
        for (unsigned code=0;code<=5;code++) {
            req=request(ticks+hz); assert(!net_trace_submit(&req,&token)); step(ticks);
            quote(frame,3,(uint8_t)code); ticks++; net_ipv4_input(rx,56); net_ping_worker_tick(ticks);
            assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_UNREACHABLE && result.icmp_code==code);
        }
        req=request(ticks+hz); assert(!net_trace_submit(&req,&token)); step(ticks);
        net_ipv4_link_down(); net_ping_worker_tick(ticks); memset(&result,0x55,sizeof(result));
        net_trace_v1_t before=result;
        assert(net_trace_collect(token,&result)==SYSCALL_EIO && !memcmp(&before,&result,sizeof(result)));
        req=request(ticks+hz); assert(!net_trace_submit(&req,&token)); step(ticks);
        net_ping_cancel(token); step(ticks+1); assert(net_trace_collect(token,&result)==SYSCALL_EINTR);
        /* Abandoned/stopped owner result reclaimed; stale cancellation harmless. */
        req=request(ticks+hz); assert(!net_trace_submit(&req,&old)); step(ticks); step(ticks+hz);
        assert(net_ping_ready(&old)); step(ticks+2*hz);
        assert(net_trace_collect(old,&result)==SYSCALL_EINTR && net_ipv4_idle());
        req=request(ticks+hz); send_fail=true; assert(!net_trace_submit(&req,&token)); step(ticks); send_fail=false;
        assert(!net_trace_collect(token,&result) && result.outcome==NETTRACE_TX_FAILED);
        assert(!net_ping_submit(&ping,&token)); step(ticks); assert(frame[22]==64 && frame[18]==0 && frame[19]==0);
        net_ping_cancel(token); step(ticks+1);
    }
    /* Near-wrap arithmetic: a valid one-tick command budget must not turn
     * the ordinary three-second ARP horizon into an already-expired value. */
    ticks=UINT64_MAX-2*hz-2;
    net_trace_v1_t near=request(ticks+1), near_result; uint64_t near_token;
    near.timeout_seconds=5;
    assert(!net_trace_submit(&near,&near_token)); step(ticks);
    assert(!net_ping_ready(&near_token)); step(near.deadline_ticks);
    assert(!net_trace_collect(near_token,&near_result) && near_result.outcome==NETTRACE_PROBE_TIMEOUT);
    /* Independent fake-clock context for the exhaustion seed. */
    ticks=0; net_ipv4_init(&dev,&cfg); net_ping_init(true);
    assert(net_trace_test_identity(UINT32_MAX-1));
    net_trace_v1_t last=request(ticks+hz); uint64_t token;
    assert(!net_trace_submit(&last,&token)); step(ticks);
    assert(frame[38]==255 && frame[39]==255 && frame[40]==255 && frame[41]==255);
    net_ping_cancel(token); step(ticks+1);
    assert(net_trace_submit(&last,&token)==SYSCALL_EAGAIN); /* never wraps */
    assert(wakes && sends && arp);
    puts("Trace host PASS: 100/1000 Hz, independent quote/fuzz, shared mailbox, stale identities, deadlines, ARP, link/lease/cancel and ping fence");
}
