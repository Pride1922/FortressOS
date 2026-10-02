#include "net.h"
#include "arp.h"
#include "string.h"
#include "apic.h"
#include "thread.h"
#include "e1000.h"
#include "serial.h"
#include "net_ipv4.h"
#include "net_ping.h"
#include "net_socket.h"
#include "net_socket_syscall.h"
#include "syscall_abi.h"
#include "smp.h"
#include "net_tcp.h"

const char g_net_poll_channel = 0;
static net_dev_t *s_if;
static net_config_t s_config;
static arp_cache_t s_arp_cache;
static uint8_t s_tx[ETH_MIN_FRAME_LEN];
static eth_header_t s_eth;
static arp_packet_t s_arp;
static uint64_t s_deadline, s_test_deadline;
static bool s_worker_started, s_test_pending;
static uint8_t s_gateway_mac[ETH_ALEN];
static net_ping_v1_t s_probe;
static uint64_t s_probe_token;
static bool s_probe_pending;
static uint64_t s_idle_start, s_idle_ticks;
static bool s_idle_reported;
static unsigned s_ap_probe_result;
static const uint8_t broadcast[ETH_ALEN] = {255,255,255,255,255,255};

static bool space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
static bool decimal(const char **p, const char *end, unsigned max, unsigned digits, unsigned *out) {
    unsigned value=0, count=0;
    while (*p<end && **p>='0' && **p<='9') {
        if (++count>digits) return false;
        value=value*10+(unsigned)(*(*p)++-'0');
        if (value>max) return false;
    }
    *out=value;
    return count!=0;
}
static bool ipv4(const char **p, const char *end, uint32_t *out) {
    uint32_t value=0;
    for (unsigned i=0; i<4; ++i) {
        unsigned octet;
        if (!decimal(p,end,255,3,&octet)) return false;
        value=(value<<8)|octet;
        if (i<3 && (*p==end || *(*p)++!='.')) return false;
    }
    *out=htonl(value);
    return true;
}
void net_parse_config(const char *cmdline, size_t len, net_config_t *out) {
    if (!out) return;
    *out=(net_config_t){.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    if (!cmdline) return;
    /* boot_info.cmdline capacity, also cap callers supplying larger ranges. */
    if (len>512) len=512;
    const char *p=cmdline, *end=cmdline+len;
    bool seen=false;
    while (p<end && *p) {
        if (space(*p)) { ++p; continue; }
        const char *start=p;
        while (p<end && *p && !space(*p)) ++p;
        size_t n=(size_t)(p-start);
        if (n==12 && !memcmp(start,"net_test=arp",12)) out->test_arp=true;
        if (n==14 && !memcmp(start,"net_test=rings",14)) out->test_rings=true;
        if (n==13 && !memcmp(start,"net_test=icmp",13)) out->test_icmp=true;
        if (n==12 && !memcmp(start,"net_test=udp",12)) out->test_udp=true;
        if (n==12 && !memcmp(start,"net_test=tcp",12)) out->test_tcp=true;
        if (n<4 || memcmp(start,"net=",4)) continue;
        const char *v=start+4;
        uint32_t local=0, gateway=0;
        unsigned prefix=0;
        bool valid=ipv4(&v,p,&local) && v<p && *v++=='/' &&
            decimal(&v,p,30,2,&prefix) && prefix>=1 && v<p && *v++==',' &&
            ipv4(&v,p,&gateway) && v==p;
        if (!valid || seen) out->malformed=true;
        else { out->local_ip=local; out->gateway=gateway; out->prefix=(uint8_t)prefix; }
        seen=true;
    }
    if (out->malformed) {
        out->local_ip=htonl(0x0a00020f); out->gateway=htonl(0x0a000202); out->prefix=24;
    }
}
void net_init(net_dev_t *dev, const net_config_t *config) {
    s_if=dev;
    s_config=*config;
    arp_cache_init(&s_arp_cache);
    net_ipv4_init(dev,config);
    net_ping_init(false);
    net_socket_init(dev,config);
    s_worker_started=false;
    s_test_pending=false;
    s_probe_pending=false;
    s_idle_reported=false; s_idle_start=0;
    __atomic_store_n(&s_ap_probe_result,0,__ATOMIC_RELEASE);
}
static int send_arp(net_dev_t *dev, const uint8_t *dest, uint32_t ip, bool reply) {
    memset(s_tx,0,sizeof(s_tx));
    if (eth_encode(s_tx,sizeof(s_tx),dest,dev->mac_addr,ETHERTYPE_ARP,NULL)) return -1;
    int result=reply ? arp_encode_reply(s_tx+ETH_HDR_LEN,sizeof(s_tx)-ETH_HDR_LEN,
        dev->mac_addr,s_config.local_ip,dest,ip,NULL) :
        arp_encode_request(s_tx+ETH_HDR_LEN,sizeof(s_tx)-ETH_HDR_LEN,
        dev->mac_addr,s_config.local_ip,ip,NULL);
    return result ? -1 : dev->send_packet(dev,s_tx,sizeof(s_tx));
}
void arp_input(net_dev_t *dev, const uint8_t *payload, size_t len) {
    if (dev!=s_if || !dev || arp_decode(payload,len,&s_arp)) return;
    /* Learn replies only; request senders are not cached in Phase 3. */
    if (s_arp.opcode==ARP_OP_REPLY) {
        arp_cache_update(&s_arp_cache,s_arp.sender_ip,s_arp.sender_mac,apic_timer_get_bsp_ticks());
    } else if (s_arp.target_ip==s_config.local_ip) {
        (void)send_arp(dev,s_arp.sender_mac,s_arp.sender_ip,true);
    }
}
void net_input(net_dev_t *dev, pbuf_t *packet) {
    if (!packet || !dev || !dev->recycle_rx) return;
    const uint8_t *payload;
    size_t len;
    if (dev==s_if && packet->length<=PBUF_CAPACITY &&
        !eth_decode(packet->payload,packet->length,&s_eth,&payload,&len) &&
        eth_mac_matches(s_eth.dest,dev->mac_addr)) {
        if (s_eth.ethertype==ETHERTYPE_ARP) arp_input(dev,payload,len);
        else if (s_eth.ethertype==ETHERTYPE_IPV4) net_ipv4_input(payload,len);
    }
    dev->recycle_rx(dev,packet);
}
int arp_resolve(net_dev_t *dev, uint32_t ip, uint8_t out_mac[ETH_ALEN]) {
    if (!dev || dev!=s_if || !out_mac || !dev->send_packet) return -1;
    if (!arp_cache_lookup(&s_arp_cache,ip,out_mac)) return 0;
    return send_arp(dev,broadcast,ip,false)==0 ? 1 : -1;
}
int net_arp_lookup(uint32_t ip, uint8_t out_mac[ETH_ALEN]) {
    return arp_cache_lookup(&s_arp_cache,ip,out_mac);
}
static bool deadline_reached(void *arg) {
    /* Under scheduler lock: ticks only, no device/cache lock or callbacks. */
    return apic_timer_get_bsp_ticks()>=*(uint64_t *)arg;
}
void net_timer_tick(void) {
    if (s_worker_started) sched_wake_all(&g_net_poll_channel);
}
void net_worker_main(void *arg) {
    (void)arg;
    bool was_online=e1000_network_online(s_if);
    if (s_config.test_icmp) {
        s_probe=(net_ping_v1_t){.version=1,.destination=s_config.gateway,.timeout_seconds=1,.sequence=1};
        s_probe_pending=net_ping_submit(&s_probe,&s_probe_token)==0;
        serial_puts(s_probe_pending ? "[NET 4] Echo probe submitted\n" : "[NET 4] Echo probe unavailable\n");
    }
    if (s_config.test_arp) {
        s_test_pending=true;
        s_test_deadline=apic_timer_get_bsp_ticks()+300;
        int result=arp_resolve(s_if,s_config.gateway,s_gateway_mac);
        serial_puts(result==1 ? "[NET 3] Gateway ARP request submitted\n" : "[NET 3] Gateway ARP request failed\n");
    }
    for (;;) {
        unsigned probe=__atomic_exchange_n(&s_ap_probe_result,0,__ATOMIC_ACQ_REL);
        if (probe) serial_puts(probe==1 ? "[NET 5] AP socket dispatch rejection PASS\n" :
            "[NET 5] AP socket dispatch rejection FAIL\n");
        bool online=e1000_service_link(s_if,apic_timer_get_bsp_ticks(),apic_timer_get_frequency());
        if (!online && was_online) arp_cache_init(&s_arp_cache);
        was_online=online;
        unsigned count=0;
        for (; online && count<64; ++count) {
            pbuf_t *p=s_if->poll_rx(s_if);
            if (!p) break;
            net_input(s_if,p);
        }
        if (s_test_pending) {
            if (!arp_cache_lookup(&s_arp_cache,s_config.gateway,s_gateway_mac)) {
                serial_puts("[NET 3] Gateway ARP resolved\n");
                s_test_pending=false;
            } else if (apic_timer_get_bsp_ticks()>=s_test_deadline) {
                serial_puts("[NET 3] Gateway ARP timeout\n");
                s_test_pending=false;
            }
        }
        net_ping_worker_tick(apic_timer_get_bsp_ticks());
        /* Includes requests published while cold-waiting, not only a falling
         * edge. Never restart protocol tables or erase endpoint generations. */
        if (!e1000_network_online(s_if)) net_ipv4_link_down();
        net_socket_worker_tick(apic_timer_get_bsp_ticks(),e1000_network_online(s_if));
        net_tcp_tick(apic_timer_get_bsp_ticks(),e1000_network_online(s_if));
        net_ipv4_tick(apic_timer_get_bsp_ticks());
        net_socket_worker_tick(apic_timer_get_bsp_ticks(),e1000_network_online(s_if));
        net_ping_worker_tick(apic_timer_get_bsp_ticks());
        if (s_probe_pending && net_ping_ready(&s_probe_token)) {
            int64_t ret=net_ping_collect(s_probe_token,&s_probe);
            serial_puts(!ret && s_probe.outcome==NETPING_REPLY ?
                "[NET 4] Echo probe reply matched\n" : "[NET 4] Echo probe failed\n");
            s_probe_pending=false;
        }
        /* Explicit test-only observation; own immortal worker TCB, no new
         * introspection ABI or scheduler operation. Idle means no RX packets
         * or active IPv4 TX/echo transaction throughout the measured window. */
        if ((s_config.test_udp || s_config.test_tcp) && !s_idle_reported) {
            uint64_t now=apic_timer_get_bsp_ticks();
            if (count || !net_ipv4_idle() || !s_idle_start) {
                s_idle_start=now; s_idle_ticks=thread_current()->total_ticks;
            } else if (now-s_idle_start>=5*apic_timer_get_frequency()) {
                serial_puts("[NET 5] Idle worker CPU ticks/elapsed ticks/hz (hex): ");
                serial_print_hex(thread_current()->total_ticks-s_idle_ticks); serial_puts("/");
                serial_print_hex(now-s_idle_start); serial_puts("/");
                serial_print_hex(apic_timer_get_frequency()); serial_puts("\n");
                s_idle_reported=true;
            }
        }
        if (count==64) thread_yield();
        else {
            s_deadline=apic_timer_get_bsp_ticks()+1;
            sched_wait_until(&g_net_poll_channel,deadline_reached,&s_deadline);
        }
    }
}
static void socket_ap_probe(void *arg) {
    (void)arg;
    interrupt_frame_t frame={0}; bool pass=true;
    for (unsigned nr=SYS_SOCKET; nr<=SYS_RECVFROM; ++nr) {
        frame.rax=nr;
        if (net_socket_syscall(&frame)!=SYSCALL_EOPNOTSUPP) pass=false;
    }
    for (unsigned nr=SYS_CONNECT; nr<=SYS_CONNECT_UNTIL; ++nr) {
        frame.rax=nr;
        if (net_socket_syscall(&frame)!=SYSCALL_EOPNOTSUPP) pass=false;
    }
    /* Publish test evidence to BSP worker; raw UART from an AP can interleave
     * bytewise with boot diagnostics and corrupt the result marker. */
    __atomic_store_n(&s_ap_probe_result,pass ? 1u : 2u,__ATOMIC_RELEASE);
}
void net_start(const char *cmdline, size_t len) {
    net_parse_config(cmdline,len,&s_config);
    if (s_config.malformed) serial_puts("[NET 3] Malformed net=; using 10.0.2.15/24,10.0.2.2\n");
    net_dev_t *dev=e1000_get_net_device();
    if (!dev || s_config.test_rings) return; /* Raw selftest remains sole RX owner. */
    net_init(dev,&s_config);
    if (!thread_create_on_cpu(0,"net_worker",net_worker_main,NULL)) {
        serial_puts("[NET 3] Worker creation failed\n");
        return;
    }
    s_worker_started=true;
    net_ping_init(true);
    net_socket_enable();
    /* Publish cold-offline before userspace can create a socket. */
    if (!e1000_network_online(dev))
        net_socket_worker_tick(apic_timer_get_bsp_ticks(),false);
    /* Explicit disposable-test opt-in. Direct dispatch from an AP kernel
     * thread tests context rejection, not a Ring 3 AP entry transition. */
    if ((s_config.test_udp || s_config.test_tcp) && smp_get_cpu_count()>1)
        (void)thread_create_on_cpu(1,"net_ap_probe",socket_ap_probe,NULL);
    serial_puts("[NET 3] BSP ingress worker started; tick-bounded polling\n");
    serial_puts("[NET-2] TCP reboot quiet time: CONNECT returns EAGAIN until BSP uptime 120 seconds\n");
}
