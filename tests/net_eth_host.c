#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include "../src/net/rx_diag.h"
#include "../src/net/net.h"
#include "arp.h"
#include "net_ipv4.h"
#include "ipv4.h"
#include "icmp.h"
#include "thread.h"
#include "net_udp_owner.h"
/* This control/worker-only fixture has no socket subsystem. */
void net_udp_step(handoff_token_t t,handoff_request_t r) { (void)t;(void)r;assert(false); }
void net_udp_maintenance(void) {}
void net_udp_refresh_work(void) {}
bool net_udp_owner_work(void) { return false; }
#include "../src/net/poll_budget.h"

static unsigned long long diag_value(const char *text, const char *key) {
    const char *p = strstr(text, key); assert(p);
    return strtoull(p + strlen(key), NULL, 10);
}
static void test_diag_bounds(void) {
    uint64_t values[] = {0, UINT64_MAX};
    const char *const names[] = {"zero", "max"};
    char text[128];
    size_t full = rx_diag_format(text, sizeof(text)-1, "[TEST]", names, values, 2);
    text[full] = 0;
    assert(!strcmp(text, "[TEST] zero=0 max=18446744073709551615\n"));
    for (size_t cap = 0; cap < sizeof(text); ++cap) {
        memset(text, 0x5a, sizeof(text));
        size_t n = rx_diag_format(text, cap, "[TEST]", names, values, 2);
        assert(n == (cap < full ? 0 : full) && text[cap] == 0x5a);
    }
}
static net_dev_t dev;
static bool carrier=true;
static net_dev_t wireless, usb;
static bool wireless_present, usb_present;
static unsigned wireless_disconnects;
net_dev_t *wlan_get_net_device(void) { return wireless_present ? &wireless : NULL; }
net_dev_t *rtl8153_get_net_device(void) { return usb_present ? &usb : NULL; }
int wlan_mgr_disconnect(void) {
    ++wireless_disconnects;
    wireless.flags &= ~NET_RUNNING;
    return 0;
}
bool net_test_service_active_interface(uint64_t now, uint64_t freq);
static bool mock_link(net_dev_t *d, uint64_t now, uint64_t hz) {
    (void)now; (void)hz;
    return (d->flags & (NET_UP | NET_RUNNING)) == (NET_UP | NET_RUNNING);
}
static void test_interface_handover(net_config_t cfg) {
    cfg.is_dhcp=true; cfg.is_static=false;
    carrier=false;
    wireless=(net_dev_t){.name="wlan0",.mac_addr={2,0,0,0,0,1},.mtu=1500,
        .flags=NET_UP|NET_RUNNING,.service_link=mock_link};
    usb=(net_dev_t){.name="eth1",.mac_addr={2,0,0,0,0,2},.mtu=1500,
        .flags=NET_UP,.service_link=mock_link};
    wireless_present=true; usb_present=true; wireless_disconnects=0;
    net_init(&dev,&cfg);
    assert(net_test_service_active_interface(100,100));
    net_set_config(&cfg); /* Simulate the successful Wi-Fi DHCP commit. */
    wireless.flags &= ~NET_RUNNING; /* Reauthentication/roaming/link interruption. */
    for (unsigned i=0;i<100;++i) {
        assert(!net_test_service_active_interface(101+i,100));
        netctl_ifget_t st;
        assert(!net_get_ifconfig(&st));
        assert(!memcmp(st.mac,wireless.mac_addr,6));
        assert(st.local_ipv4==cfg.local_ip && !wireless_disconnects);
    }
    wireless.flags |= NET_RUNNING;
    assert(net_test_service_active_interface(201,100));
    assert(!wireless_disconnects);
    carrier=true; /* Real wired carrier still has priority. */
    assert(net_test_service_active_interface(202,100));
    netctl_ifget_t st; assert(!net_get_ifconfig(&st));
    assert(!memcmp(st.mac,dev.mac_addr,6) && st.local_ipv4==0);
    assert(wireless_disconnects==1);
    carrier=false;
    assert(!net_test_service_active_interface(203,100));
    assert(wireless_disconnects==1);
    wireless.flags |= NET_RUNNING;
    assert(net_test_service_active_interface(204,100));
    net_set_config(&cfg);
    usb.flags |= NET_RUNNING; /* A genuinely online dock NIC also wins. */
    assert(net_test_service_active_interface(205,100));
    assert(!net_get_ifconfig(&st));
    assert(!memcmp(st.mac,usb.mac_addr,6) && st.local_ipv4==0);
    assert(wireless_disconnects==2);
    wireless_present=usb_present=false; carrier=true;
    net_init(&dev,&cfg);
    puts("Interface handover PASS: offline Ethernet/dock cannot evict Wi-Fi; 100 down turns; real wired/USB takeover retained");
}
static unsigned link_checks, rx_polls;
static bool test_poll_hint;
static bool test_rx_burst;
static unsigned peer_yield = 1;
static uint64_t poll_hz,poll_cycles;
static unsigned poll_fault, poll_reads;
uint64_t apic_poll_clock_hz(void) { return poll_hz; }
uint64_t apic_poll_clock_read(void) {
    ++poll_reads;
    if (poll_fault==2 || (poll_fault==1 && poll_reads>5)) return 0;
    poll_cycles+=10; return poll_cycles;
}
bool net_tcp_idle(void) { return true; }
static bool mock_tcp_receiving;
bool net_tcp_receiving(void) { return mock_tcp_receiving; }
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len) {
    (void)source; (void)data; (void)len;
}
void net_tcp_tick(uint64_t ticks, bool online) { (void)ticks; (void)online; }
void net_tcp_set_local_ip(uint32_t ip) { (void)ip; }
/* Phase 3 isolation: sockets are tested separately with actual implementation. */
void net_socket_init(net_dev_t *d, const net_config_t *cfg) { (void)d; (void)cfg; }
void net_socket_enable(void) {}
void net_socket_set_dev(net_dev_t *d) { (void)d; }
void net_socket_set_local(uint32_t ip) { (void)ip; }
void net_socket_worker_tick(uint64_t now, bool online) { (void)now; (void)online; }
void net_socket_input(uint32_t ip, uint16_t sp, uint16_t dp, const uint8_t *data, size_t len) {
    (void)ip; (void)sp; (void)dp; (void)data; (void)len;
}
bool e1000_network_online(net_dev_t *d) { return d==&dev && carrier; }
uint32_t e1000_link_state_abi(const net_dev_t *d) { return (d==&dev && carrier) ? NET_IF_LINK_ONLINE : NET_IF_LINK_DOWN; }
bool e1000_service_link(net_dev_t *d, uint64_t now, uint64_t hz) {
    (void)now; assert(hz==100); ++link_checks; return e1000_network_online(d);
}
size_t smp_get_cpu_count(void) { return 1; }
int64_t net_socket_syscall(interrupt_frame_t *frame) { (void)frame; return -14; }
static pbuf_t packet;
static uint8_t sent[60];
static unsigned sends, recycles, wakes, waits, yields;
static bool fail_send, absent, fail_create;
void e1000_get_stats(const net_dev_t *d, uint64_t *rx, uint64_t *tx) { (void)d; if (rx) *rx = 0; if (tx) *tx = sends; }
static uint64_t ticks;
static jmp_buf done;
uint64_t apic_timer_get_bsp_ticks(void) { return ticks; }
uint64_t apic_timer_get_frequency(void) { return 100; }
void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t n) { (void)n; }
void serial_raw_puts(const char *s) { (void)s; }
void serial_raw_print_hex(uint64_t n) { (void)n; }
tcb_t *thread_current(void) { static tcb_t worker; return &worker; }
net_dev_t *e1000_get_net_device(void) { return absent ? NULL : &dev; }
tcb_t *thread_create_on_cpu(size_t cpu, const char *name, void (*entry)(void *), void *arg) {
    assert(cpu==0 && !strcmp(name,"net_worker") && entry==net_worker_main && !arg);
    return fail_create ? NULL : (tcb_t *)&dev;
}
bool process_signal_interrupt(void) { return false; }
void thread_yield(void) { ++yields; }
void sched_wake_all(const void *channel) { if (channel==&handoff_owner_channel) ++wakes; }
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    assert(channel==&handoff_owner_channel && !ready(arg));
    assert(*(uint64_t *)arg==ticks+1);
    if (test_poll_hint) {
        unsigned before=wakes;
        net_request_poll();net_request_poll();
        assert(wakes==before+2 && ready(arg));
        if (++waits==3) longjmp(done,1);
        return; /* No timer progress: worker must consume the hint next pass. */
    }
    unsigned before=wakes;
    ++ticks;
    net_timer_tick();
    assert(wakes==before+1 && ready(arg));
    if (++waits==3) longjmp(done,1);
}
static int send_packet(net_dev_t *d, const void *buf, size_t len) {
    assert(d==&dev && len==60);
    memcpy(sent,buf,len); ++sends;
    return fail_send ? -1 : 0;
}
static pbuf_t *poll_rx(net_dev_t *d) {
    assert(d==&dev); ++rx_polls;
    /* Peer response arrives only after the first application scheduling turn. */
    if (test_rx_burst && (rx_polls==1 || (yields==peer_yield && rx_polls==peer_yield+2))) return &packet;
    return NULL;
}
static void recycle(net_dev_t *d, pbuf_t *p) { assert(d==&dev && p==&packet); ++recycles; }
static void input(void) { unsigned before=recycles; net_input(&dev,&packet); assert(recycles==before+1); }
static void make_arp(const uint8_t *dest, bool reply, uint32_t target) {
    static const uint8_t peer[6]={2,3,4,5,6,7};
    pbuf_init(&packet); packet.length=60;
    memset(packet.data,0,60);
    assert(!eth_encode(packet.data,60,dest,peer,ETHERTYPE_ARP,NULL));
    if (reply) assert(!arp_encode_reply(packet.data+14,46,peer,htonl(0x0a000202),dev.mac_addr,target,NULL));
    else assert(!arp_encode_request(packet.data+14,46,peer,htonl(0x0a000202),target,NULL));
}
int main(void) {
    net_config_t cfg;
    dev=(net_dev_t){.mac_addr={0x52,0x54,0,0x12,0x34,0x56},.flags=NET_UP|NET_RUNNING,.mtu=1500,
        .send_packet=send_packet,.poll_rx=poll_rx,.recycle_rx=recycle,
        .network_online=e1000_network_online,.service_link=e1000_service_link};
    net_parse_config(NULL,0,&cfg);
    assert(cfg.local_ip==htonl(0x0a00020f) && cfg.gateway==htonl(0x0a000202) && cfg.prefix==24);
    const char *valid="xnet=bad\tnet=192.168.1.150/24,192.168.1.1 net_test=arp";
    net_parse_config(valid,strlen(valid),&cfg);
    assert(!cfg.malformed && cfg.local_ip==htonl(0xc0a80196) && cfg.gateway==htonl(0xc0a80101) && cfg.test_arp);
    const char *bad[]={"net=", "net=256.1.2.3/24,1.2.3.4", "net=1.2.3/24,1.2.3.4",
        "net=1.2.3.4/0,1.2.3.4", "net=1.2.3.4/31,1.2.3.4", "net=1.2.3.4/24,1.2.3.4x",
        "net=1.2.3.4/24,1.2.3.4 net=2.3.4.5/24,1.2.3.4", "net=1234.1.2.3/24,1.2.3.4"};
    for (unsigned i=0; i<sizeof(bad)/sizeof(*bad); ++i) {
        net_parse_config(bad[i],strlen(bad[i]),&cfg);
        assert(cfg.malformed && cfg.local_ip==htonl(0x0a00020f));
    }
    char bounded[4]={'n','e','t','='};
    net_parse_config(bounded,sizeof(bounded),&cfg); assert(cfg.malformed);
    net_parse_config(NULL,0,&cfg); net_init(&dev,&cfg);
    net_parse_config("net_test=icmp",13,&cfg); assert(cfg.test_icmp);
    net_parse_config(NULL,0,&cfg); net_init(&dev,&cfg);
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    make_arp(broadcast,false,cfg.local_ip); input(); assert(sends==1);
    eth_header_t eth; arp_packet_t arp; const uint8_t *payload; size_t len;
    assert(!eth_decode(sent,60,&eth,&payload,&len) && eth.ethertype==ETHERTYPE_ARP);
    assert(!arp_decode(payload,len,&arp) && arp.opcode==ARP_OP_REPLY);
    assert(arp.sender_ip==cfg.local_ip && arp.target_ip==cfg.gateway);
    assert(!memcmp(eth.dest,arp.target_mac,6) && !memcmp(eth.src,dev.mac_addr,6));
    /* RFC 826: the first incoming targeted request taught us the sender.
     * It must not require an outgoing ping to populate the neighbor cache. */
    uint8_t mac[6];
    assert(!arp_resolve(&dev,cfg.gateway,mac) && sends==1 && mac[0]==2);
    uint32_t cold_peer=htonl(0x0a000203);
    assert(arp_resolve(&dev,cold_peer,mac)==1 && sends==2);
    assert(!arp_decode(sent+14,46,&arp) && arp.opcode==ARP_OP_REQUEST && arp.target_ip==cold_peer);
    assert(eth_is_broadcast(sent));
    make_arp(dev.mac_addr,true,cfg.local_ip); input();
    assert(!arp_resolve(&dev,cfg.gateway,mac) && sends==2 && mac[0]==2);
    static const uint8_t other[6]={8,9,10,11,12,13};
    make_arp(other,false,cfg.local_ip); input(); assert(sends==2);
    make_arp(broadcast,false,cfg.local_ip); packet.data[12]=0x88; packet.data[13]=0xb5; input(); assert(sends==2);
    make_arp(broadcast,false,cfg.local_ip); packet.data[12]=8; packet.data[13]=0; input(); assert(sends==2);
    make_arp(broadcast,false,htonl(0x0a000210)); input(); assert(sends==2);
    make_arp(broadcast,false,cfg.local_ip); packet.data[14]=99; input(); assert(sends==2);
    /* Exact allocations ensure ASan observes an accidental read beyond length. */
    for (unsigned n=0; n<42; ++n) {
        uint8_t short_frame[n+1];
        memset(short_frame,0,n+1);
        if (n>=14) eth_encode(short_frame,n,broadcast,dev.mac_addr,ETHERTYPE_ARP,NULL);
        packet.payload=short_frame; packet.length=(uint16_t)n; input();
    }
    make_arp(broadcast,false,cfg.local_ip); fail_send=true; input(); assert(sends==3);
    assert(arp_resolve(&dev,htonl(0x0a000211),mac)==-1); fail_send=false;
    packet.length=1515; input(); packet.payload=NULL; packet.length=60; input();
    absent=true; net_start(NULL,0); net_timer_tick();
    netctl_ifget_t absent_status;
    assert(wakes && net_get_ifconfig(&absent_status)==SYSCALL_EIO);
    /* A device-less control owner is intentional; failed worker creation and
     * the raw ring fixture still must not publish a polling worker. */
    wakes=0; absent=false;
    fail_create=true; net_start(NULL,0); net_timer_tick(); assert(!wakes); fail_create=false;
    net_start("net_test=rings",14); net_timer_tick(); assert(!wakes);
    net_start(NULL,0);
    waits=wakes=0; /* Initial owner publication is a separate wake. */
    if (!setjmp(done)) net_worker_main(NULL);
    assert(waits==3 && wakes==3 && yields==0);
    unsigned before_polls=rx_polls;
    waits=wakes=0; carrier=false;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(waits==3 && wakes==3 && yields==0 && rx_polls==before_polls);
    waits=wakes=0; carrier=true;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(waits==3 && wakes==3 && yields==0 && rx_polls==before_polls+3 && link_checks==9);
    waits=wakes=0;test_poll_hint=true;volatile uint64_t before_ticks=ticks;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(waits==3 && wakes==6 && ticks==before_ticks && yields==2);
    test_poll_hint=false; test_rx_burst=true;
    poll_hz=1000000;
    waits=wakes=rx_polls=yields=0;
    volatile unsigned before_recycles=recycles;
    make_arp(broadcast,false,cfg.local_ip);
    before_ticks=ticks;
    if (!setjmp(done)) net_worker_main(NULL);
    /* Both packets arrive without advancing ticks, then the budget expires and the
     * ordinary timer sleep resumes. Every packet is recycled once. */
    assert(recycles==before_recycles+2 && yields>30 && yields<4096 && waits==3);
    assert(ticks==before_ticks+3 && rx_polls==yields+5);
    /* Deliver the peer response beyond 200us but before 1ms. Both
     * packets must be handled before the first timer wait, then return idle. */
    peer_yield=30; poll_cycles=0; waits=wakes=rx_polls=yields=0;
    before_recycles=recycles; before_ticks=ticks;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(recycles==before_recycles+2 && yields>peer_yield && yields<4096 && waits==3);
    assert(ticks==before_ticks+3 && rx_polls==yields+5);
    peer_yield=1;
    mock_tcp_receiving=true; poll_cycles=0; waits=wakes=rx_polls=yields=0;
    before_recycles=recycles; before_ticks=ticks;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(recycles==before_recycles+2 && yields>100 && yields<4096 && waits==3);
    assert(ticks==before_ticks+3 && rx_polls==yields+5);
    mock_tcp_receiving=false;
    net_poll_budget_t budget;
    net_poll_adaptive_t adaptive={0};
    assert(net_poll_adaptive_us(&adaptive,10,100,true,true)==1000);
    assert(net_poll_adaptive_us(&adaptive,11,100,true,true)==2000);
    assert(net_poll_adaptive_us(&adaptive,12,100,true,false)==2000);
    assert(net_poll_adaptive_us(&adaptive,13,100,true,false)==1000);
    assert(net_poll_adaptive_us(&adaptive,14,100,true,true)==1000);
    assert(net_poll_adaptive_us(&adaptive,14,100,true,true)==2000);
    assert(net_poll_adaptive_us(&adaptive,14,100,false,true)==1000);
    net_poll_budget_start_us(&budget,100,1000000,2000);
    assert(net_poll_budget_continue(&budget,2099));
    assert(!net_poll_budget_continue(&budget,2100) && !budget.failed);
    net_poll_budget_start(&budget,100,1000000);
    /* A peer arriving after the old 200us window still gets polled, without
     * extending the original deadline on empty turns. */
    assert(net_poll_budget_continue(&budget,450));
    assert(net_poll_budget_continue(&budget,1099));
    assert(!net_poll_budget_continue(&budget,1100));
    assert(!budget.failed);
    net_poll_budget_start(&budget,100,1000000);
    assert(!net_poll_budget_continue(&budget,99));
    assert(budget.failed);
    net_poll_budget_start(&budget,100,1000000);
    for (unsigned i=0;i<4095;++i) assert(net_poll_budget_continue(&budget,100));
    assert(!net_poll_budget_continue(&budget,100));
    assert(budget.failed);
    net_poll_budget_start(&budget,100,0);
    assert(!net_poll_budget_continue(&budget,100));
    poll_hz=0; waits=wakes=rx_polls=yields=0;
    before_recycles=recycles;
    if (!setjmp(done)) net_worker_main(NULL);
    assert(recycles==before_recycles+2 && yields==2 && waits==3 && rx_polls==7);
    char profile[1025]; size_t profile_len=net_poll_profile_format(profile,1024);
    profile[profile_len]=0;
    assert(strstr(profile,"[NET POLL] clock-hz=0\n"));
    assert(strstr(profile,"[NET POLL] expired="));
    assert(strstr(profile,"[NET POLL] backward-clock=0\n"));
    assert(strstr(profile,"[NET POLL] iteration-backstop=0\n"));
    assert(strstr(profile,"[NET POLL] hint-returns=2\n"));
    assert(strstr(profile,"[NET POLL] yield-over-1ms=0\n"));
    for (volatile unsigned fault=1;fault<=2;fault++) {
        poll_fault=fault; poll_reads=0; poll_cycles=0; poll_hz=1000000;
        waits=wakes=rx_polls=yields=0;
        if (!setjmp(done)) net_worker_main(NULL);
        profile_len=net_poll_profile_format(profile,1024); profile[profile_len]=0;
        assert(strstr(profile,"[NET POLL] clock-hz=0\n"));
        assert(strstr(profile,"[NET POLL] backward-clock=1\n"));
        if (fault==2) assert(strstr(profile,"[NET POLL] iteration-backstop=1\n"));
        assert(waits==3 && yields<=4097);
    }
    poll_fault=0;
    for (size_t cap=0;cap<80;cap++) {
        memset(profile,0x5a,sizeof(profile));
        size_t n=net_poll_profile_format(profile,cap);
        assert(n<=cap && profile[cap]==0x5a);
    }
    /* End-to-end actual Ethernet/ARP/IPv4/ICMP path, mocked NIC only.
     * Repeat from a cleared ARP cache with no application-originated traffic. */
    for (unsigned cycle=0;cycle<3;++cycle) {
        net_init(&dev,&cfg);
        unsigned cold_before=sends;
        make_arp(broadcast,false,cfg.local_ip);
        input();
        assert(sends==cold_before+1);
        assert(!arp_decode(sent+14,46,&arp) && arp.opcode==ARP_OP_REPLY);
        static const uint8_t peer_mac[6]={2,3,4,5,6,7};
        memset(packet.data,0,60); packet.payload=packet.data; packet.length=60;
        assert(!eth_encode(packet.data,60,dev.mac_addr,peer_mac,ETHERTYPE_IPV4,NULL));
        assert(!icmp_echo_encode(packet.data+34,26,ICMP_ECHO_REQUEST,0x1234,
                                 (uint16_t)cycle,"cold",4));
        assert(!ipv4_encode(packet.data+14,46,cfg.gateway,cfg.local_ip,1,12,64,NULL));
        ticks+=6000; /* Idle before peer arrival; no application activity. */
        input();
        net_ipv4_tick(ticks);
        assert(sends==cold_before+2);
        ipv4_header_t response; const uint8_t *body,*echo_data; size_t body_len,echo_len;
        icmp_echo_t returned;
        assert(!ipv4_decode(sent+14,46,&response,&body,&body_len));
        assert(response.src_ip==cfg.local_ip && response.dst_ip==cfg.gateway);
        assert(!memcmp(sent,peer_mac,6));
        assert(!icmp_echo_decode(body,body_len,&returned,&echo_data,&echo_len));
        assert(returned.type==ICMP_ECHO_REPLY && returned.identifier==0x1234 &&
               returned.sequence==cycle && echo_len==4 && !memcmp(echo_data,"cold",4));
    }
    /* Observe the actual dispatch and TX failure branches, not a mock
     * diagnostic callback. Snapshots must not send or recycle packets. */
    char diag[512];
    size_t dn = net_arp_profile_format(diag, sizeof(diag)-1); diag[dn]=0;
    unsigned long long local0=diag_value(diag,"local-request=");
    unsigned long long ok0=diag_value(diag,"reply-tx-ok=");
    unsigned long long fail0=diag_value(diag,"reply-tx-fail=");
    make_arp(broadcast,false,cfg.local_ip); input();
    fail_send=true;
    make_arp(broadcast,false,cfg.local_ip); input();
    fail_send=false;
    unsigned sent0=sends, recycled0=recycles;
    dn=net_arp_profile_format(diag,sizeof(diag)-1); diag[dn]=0;
    assert(diag_value(diag,"local-request=")==local0+2);
    assert(diag_value(diag,"reply-tx-ok=")==ok0+1);
    assert(diag_value(diag,"reply-tx-fail=")==fail0+1);
    assert(sends==sent0 && recycles==recycled0);
    test_diag_bounds();
    puts("RX diagnostics PASS: ARP request/reply success/failure, bounded formatting, uint64 maximum, side-effect-free snapshots");
    puts("Cold inbound LAN PASS: broadcast ARP, immediate neighbor learning, echo replies without outgoing ping (3 fresh-cache cycles)");
    test_interface_handover(cfg);
    printf("NET 3 host PASS: dispatch/recycle, cache/resolve, bounded config, timer deadline (mock scheduler); %u inputs recycled\n",recycles);
    return 0;
}
