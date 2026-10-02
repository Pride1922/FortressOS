#include "net_ipv4.h"
#include "ipv4.h"
#include "icmp.h"
#include "string.h"
#include "apic.h"
#include "udp.h"
#include "net_socket.h"
#include "syscall_abi.h"
#include "net_tcp.h"

typedef struct {
    uint8_t frame[ETH_MAX_FRAME_LEN];
    size_t len;
    uint32_t next_hop;
    uint64_t next_arp, expire, start;
    unsigned attempts;
    bool active;
} pending_t;
static pending_t s_pending[2+NET_SOCKET_MAX]; /* ping, echo reply, per-socket UDP */
static struct { uint64_t token; int64_t result; bool done; size_t len; } s_udp[NET_SOCKET_MAX];
static net_dev_t *s_dev;
static net_config_t s_cfg;
static ipv4_header_t s_ip;
static icmp_echo_t s_echo;
static net_ping_v1_t s_ping;
static uint8_t s_ping_data[32], s_mac[ETH_ALEN];
static uint64_t s_token, s_sent_tick, s_echo_deadline;
static struct { uint32_t ip; uint64_t tick; bool used; } s_arp_recent[2+NET_SOCKET_MAX];
static bool s_ping_waiting, s_ping_done;
static uint64_t s_reply_dropped;
static uint64_t s_udp_malformed;

static uint32_t mask(void) { return 0xffffffffu << (32-s_cfg.prefix); }
bool net_ipv4_unicast(uint32_t ip) {
    uint32_t h=ntohl(ip), local=ntohl(s_cfg.local_ip), m=mask();
    if (!(h>>24) || (h>>24)==127 || (h>>24)>=224) return false;
    if ((h&m)==(local&m) && (!(h&~m) || (h&~m)==~m)) return false;
    return true;
}
bool net_ipv4_route(uint32_t dest, uint32_t *hop) {
    if (!net_ipv4_unicast(dest) || dest==s_cfg.local_ip) return false;
    uint32_t m=mask();
    if ((ntohl(dest)&m)==(ntohl(s_cfg.local_ip)&m)) *hop=dest;
    else {
        *hop=s_cfg.gateway;
        if (!net_ipv4_unicast(*hop) || *hop==s_cfg.local_ip ||
            (ntohl(*hop)&m)!=(ntohl(s_cfg.local_ip)&m)) return false;
    }
    return true;
}
uint32_t net_ipv4_local(void) { return s_cfg.local_ip; }
bool net_ipv4_idle(void) {
    if (s_ping_waiting) return false;
    if (!net_tcp_idle()) return false;
    for (unsigned i=0; i<2+NET_SOCKET_MAX; ++i) if (s_pending[i].active) return false;
    return true;
}
void net_ipv4_init(net_dev_t *dev, const net_config_t *config) {
    s_dev=dev; s_cfg=*config;
    memset(s_pending,0,sizeof(s_pending));
    memset(s_udp,0,sizeof(s_udp)); memset(s_arp_recent,0,sizeof(s_arp_recent));
    s_ping_waiting=s_ping_done=false; s_token=0; s_reply_dropped=s_udp_malformed=0;
}
static bool build(pending_t *p, uint32_t dest, uint8_t type, uint16_t id,
                  uint16_t seq, const uint8_t *data, size_t len, uint64_t now) {
    if (len>1472 || len+28>s_dev->mtu || !net_ipv4_route(dest,&p->next_hop)) return false;
    memset(p->frame,0,sizeof(p->frame));
    if (icmp_echo_encode(p->frame+34,sizeof(p->frame)-34,type,id,seq,data,len) ||
        ipv4_encode(p->frame+14,sizeof(p->frame)-14,s_cfg.local_ip,dest,1,(uint16_t)(8+len),64,NULL)) return false;
    p->len=42+len; if (p->len<60) p->len=60;
    p->attempts=0; p->next_arp=now; p->start=now;
    p->expire=now+3*apic_timer_get_frequency(); p->active=true;
    return true;
}
static void finish(uint32_t outcome, uint64_t rtt) {
    s_ping.outcome=outcome; s_ping.rtt_ticks=rtt;
    s_ping.echoed_bytes=outcome==NETPING_REPLY ? 32 : 0;
    s_ping.tick_hz=apic_timer_get_frequency();
    s_pending[0].active=false; s_ping_waiting=false; s_ping_done=true;
}
bool net_ipv4_ping_start(const net_ping_v1_t *request, uint64_t token, uint64_t now) {
    if (s_pending[0].active || s_ping_waiting || s_ping_done) return false;
    s_ping=*request; s_token=token;
    for (unsigned i=0; i<32; ++i) s_ping_data[i]=(uint8_t)(0xa0+i);
    for (unsigned i=0; i<8; ++i) s_ping_data[i]=(uint8_t)(token>>(i*8));
    uint64_t start=now+(request->start_delay_ms ? apic_timer_get_frequency() : 0);
    if (!build(&s_pending[0],request->destination,8,(uint16_t)token,
               (uint16_t)request->sequence,s_ping_data,32,start)) finish(NETPING_TX_FAILED,0);
    return true;
}
void net_ipv4_link_down(void) {
    if (s_pending[0].active || s_ping_waiting) finish(NETPING_TX_FAILED,0);
    for (unsigned i=0; i<2+NET_SOCKET_MAX; ++i) {
        if (i>=2 && s_pending[i].active) {
            s_udp[i-2].result=SYSCALL_EIO; s_udp[i-2].done=true;
        }
        s_pending[i].active=false;
    }
    memset(s_arp_recent,0,sizeof(s_arp_recent));
}
bool net_ipv4_ping_take(net_ping_v1_t *result) {
    if (!s_ping_done) return false;
    *result=s_ping; s_ping_done=false; return true;
}
void net_ipv4_ping_cancel(uint64_t token) {
    if (token!=s_token) return;
    s_pending[0].active=false; s_ping_done=s_ping_waiting=false;
}
bool net_ipv4_udp_start(unsigned slot, uint64_t token, uint32_t dest,
                        uint16_t sport, uint16_t dport, const uint8_t *data,
                        size_t len, uint64_t now) {
    if (slot>=NET_SOCKET_MAX || !s_dev || len>NET_UDP_DATA_MAX || len+28>s_dev->mtu) return false;
    pending_t *p=&s_pending[2+slot];
    if (p->active || !net_ipv4_route(dest,&p->next_hop)) return false;
    memset(p->frame,0,sizeof(p->frame));
    if (udp_encode(p->frame+34,sizeof(p->frame)-34,s_cfg.local_ip,dest,sport,dport,data,len) ||
        ipv4_encode(p->frame+14,sizeof(p->frame)-14,s_cfg.local_ip,dest,17,(uint16_t)(8+len),64,NULL)) return false;
    p->len=42+len; if (p->len<60) p->len=60;
    p->attempts=0; p->start=p->next_arp=now; p->expire=now+3*apic_timer_get_frequency(); p->active=true;
    s_udp[slot].token=token; s_udp[slot].len=len; s_udp[slot].done=false;
    return true;
}
void net_ipv4_udp_sync(unsigned slot, uint64_t token) {
    if (slot<NET_SOCKET_MAX && s_udp[slot].token!=token) {
        s_pending[slot+2].active=false; s_udp[slot].token=0; s_udp[slot].done=false;
    }
}
bool net_ipv4_udp_take(unsigned slot, uint64_t token, int64_t *result) {
    if (slot>=NET_SOCKET_MAX || s_udp[slot].token!=token || !s_udp[slot].done) return false;
    *result=s_udp[slot].result; s_udp[slot].done=false; return true;
}
void net_ipv4_input(const uint8_t *packet, size_t len) {
    if (!s_dev || ipv4_decode(packet,len,&s_ip,NULL,NULL) || ipv4_get_ihl_bytes(&s_ip)!=20 ||
        !s_ip.ttl || s_ip.dst_ip!=s_cfg.local_ip || !net_ipv4_unicast(s_ip.src_ip)) return;
    size_t n=ntohs(s_ip.total_len)-20;
    const uint8_t *data; size_t data_len;
    if (s_ip.protocol==6) { net_tcp_input(s_ip.src_ip,packet+20,n); return; }
    if (s_ip.protocol==17) {
        udp_header_t header;
        if (!udp_decode(packet+20,n,s_ip.src_ip,s_ip.dst_ip,&header,&data,&data_len))
            net_socket_input(s_ip.src_ip,header.source,header.destination,data,data_len);
        else ++s_udp_malformed;
        return;
    }
    if (s_ip.protocol!=1) return;
    if (icmp_echo_decode(packet+20,n,&s_echo,&data,&data_len)) return;
    uint64_t now=apic_timer_get_bsp_ticks();
    if (s_echo.type==ICMP_ECHO_REQUEST) {
        if (s_pending[1].active) { ++s_reply_dropped; return; }
        (void)build(&s_pending[1],s_ip.src_ip,0,s_echo.identifier,s_echo.sequence,data,data_len,now);
    } else if (s_ping_waiting && now<s_echo_deadline && s_ip.src_ip==s_ping.destination &&
        s_echo.identifier==(uint16_t)s_token && s_echo.sequence==s_ping.sequence &&
        data_len==32 && !memcmp(data,s_ping_data,32)) finish(NETPING_REPLY,now-s_sent_tick);
}
void net_ipv4_tick(uint64_t now) {
    if (!s_dev) return;
    uint64_t hz=apic_timer_get_frequency();
    if (s_ping_waiting && now>=s_echo_deadline) finish(NETPING_ECHO_TIMEOUT,0);
    for (unsigned i=0; i<2+NET_SOCKET_MAX; ++i) {
        pending_t *p=&s_pending[i];
        if (!p->active || now<p->start) continue;
        if (now>=p->expire) {
            p->active=false;
            if (!i) finish(NETPING_ARP_TIMEOUT,0);
            else if (i>=2) { s_udp[i-2].result=SYSCALL_EIO; s_udp[i-2].done=true; }
            continue;
        }
        /* Lookup is cache-only; resolve sends only at scheduled one-second retries. */
        int resolved=net_arp_lookup(p->next_hop,s_mac);
        if (resolved && now>=p->next_arp && p->attempts<3) {
            unsigned recent=0;
            for (; recent<2+NET_SOCKET_MAX; ++recent)
                if (s_arp_recent[recent].used && s_arp_recent[recent].ip==p->next_hop &&
                    now-s_arp_recent[recent].tick<hz) break;
            if (recent<2+NET_SOCKET_MAX) { p->next_arp=s_arp_recent[recent].tick+hz; continue; }
            resolved=arp_resolve(s_dev,p->next_hop,s_mac);
            ++p->attempts; p->next_arp=now+hz;
            for (recent=0; recent<2+NET_SOCKET_MAX; ++recent)
                if (!s_arp_recent[recent].used || now-s_arp_recent[recent].tick>=hz) break;
            if (recent<2+NET_SOCKET_MAX) {
                s_arp_recent[recent].used=true; s_arp_recent[recent].ip=p->next_hop; s_arp_recent[recent].tick=now;
            }
            if (resolved<0) {
                p->active=false; if (!i) finish(NETPING_TX_FAILED,0);
                else if (i>=2) { s_udp[i-2].result=SYSCALL_EIO; s_udp[i-2].done=true; }
                continue;
            }
        }
        if (resolved) continue;
        if (eth_encode(p->frame,sizeof(p->frame),s_mac,s_dev->mac_addr,ETHERTYPE_IPV4,NULL)) continue;
        p->active=false;
        int sent=s_dev->send_packet(s_dev,p->frame,p->len);
        if (i>=2) { s_udp[i-2].result=sent ? SYSCALL_EIO : (int64_t)s_udp[i-2].len; s_udp[i-2].done=true; }
        else if (sent) { if (!i) finish(NETPING_TX_FAILED,0); }
        else if (!i) {
            s_sent_tick=now; s_echo_deadline=now+s_ping.timeout_seconds*hz; s_ping_waiting=true;
        }
    }
}
