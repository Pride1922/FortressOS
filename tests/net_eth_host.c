#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "../src/net/net.h"
#include "arp.h"
#include "thread.h"

static net_dev_t dev;
bool net_tcp_idle(void) { return true; }
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len) {
    (void)source; (void)data; (void)len;
}
void net_tcp_tick(uint64_t ticks, bool online) { (void)ticks; (void)online; }
/* Phase 3 isolation: sockets are tested separately with actual implementation. */
void net_socket_init(net_dev_t *d, const net_config_t *cfg) { (void)d; (void)cfg; }
void net_socket_enable(void) {}
void net_socket_worker_tick(uint64_t now, bool online) { (void)now; (void)online; }
void net_socket_input(uint32_t ip, uint16_t sp, uint16_t dp, const uint8_t *data, size_t len) {
    (void)ip; (void)sp; (void)dp; (void)data; (void)len;
}
bool e1000_network_online(net_dev_t *d) { return d==&dev; }
size_t smp_get_cpu_count(void) { return 1; }
int64_t net_socket_syscall(interrupt_frame_t *frame) { (void)frame; return -14; }
static pbuf_t packet;
static uint8_t sent[60];
static unsigned sends, recycles, wakes, waits, yields;
static bool fail_send, absent, fail_create;
static uint64_t ticks;
static jmp_buf done;
uint64_t apic_timer_get_bsp_ticks(void) { return ticks; }
uint64_t apic_timer_get_frequency(void) { return 100; }
void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t n) { (void)n; }
tcb_t *thread_current(void) { static tcb_t worker; return &worker; }
net_dev_t *e1000_get_net_device(void) { return absent ? NULL : &dev; }
tcb_t *thread_create_on_cpu(size_t cpu, const char *name, void (*entry)(void *), void *arg) {
    assert(cpu==0 && !strcmp(name,"net_worker") && entry==net_worker_main && !arg);
    return fail_create ? NULL : (tcb_t *)&dev;
}
void thread_yield(void) { ++yields; }
void sched_wake_all(const void *channel) { assert(channel==&g_net_poll_channel); ++wakes; }
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    assert(channel==&g_net_poll_channel && !ready(arg));
    assert(*(uint64_t *)arg==ticks+1);
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
static pbuf_t *poll_rx(net_dev_t *d) { assert(d==&dev); return NULL; }
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
    dev=(net_dev_t){.mac_addr={0x52,0x54,0,0x12,0x34,0x56},.flags=NET_UP|NET_RUNNING,
        .send_packet=send_packet,.poll_rx=poll_rx,.recycle_rx=recycle};
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
    uint8_t mac[6]; assert(arp_resolve(&dev,cfg.gateway,mac)==1 && sends==2); /* requests not cached */
    assert(!arp_decode(sent+14,46,&arp) && arp.opcode==ARP_OP_REQUEST && arp.target_ip==cfg.gateway);
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
    absent=true; net_start(NULL,0); net_timer_tick(); assert(!wakes); absent=false;
    fail_create=true; net_start(NULL,0); net_timer_tick(); assert(!wakes); fail_create=false;
    net_start("net_test=rings",14); net_timer_tick(); assert(!wakes);
    net_start(NULL,0);
    if (!setjmp(done)) net_worker_main(NULL);
    assert(waits==3 && wakes==3 && yields==0);
    printf("NET 3 host PASS: dispatch/recycle, cache/resolve, bounded config, timer deadline (mock scheduler); %u inputs recycled\n",recycles);
    return 0;
}
