/* Reuse the existing memory/fd/scheduler adapters and UDP regression baseline. */
#define main udp_baseline_main
#include "net_socket_host.c"
#undef main
#include "net_tcp.h"
#include "net_tcp_syscall.h"
static tcp_conn_t peer_connection;
static uint8_t peer_bytes[65536], peer_packet[1500], peer_scratch[1460], stream_output[65536];
static size_t peer_count, peer_queued;
static bool peer_active, peer_return, refuse, blackhole;
static unsigned tcp_packets;
static unsigned fin_packets, reset_packets;
static bool interrupt_after_send, interrupt_wait_before_reply;
/* Linker wrapper injects publication after actual queue acceptance and before
 * syscall return; production code and the scheduler adapter are unchanged. */
int64_t __real_net_tcp_send(unsigned slot, const void *data, size_t len);
int64_t __wrap_net_tcp_send(unsigned slot, const void *data, size_t len) {
    int64_t result=__real_net_tcp_send(slot,data,len);
    if (interrupt_after_send && result>0) {
        interrupt_after_send=false; signal_pending=true;
    }
    return result;
}
static int tcp_transmit(net_dev_t *d, const void *bytes, size_t n) {
    net_test_assert_unheld(); assert(d==&dev);
    const uint8_t *wire=bytes;
    ipv4_header_t ip; const uint8_t *segment, *data; size_t length, dn; tcp_header_t h;
    assert(!ipv4_decode(wire+14,n-14,&ip,&segment,&length));
    assert(ip.protocol==6 && !tcp_decode(segment,length,ip.src_ip,ip.dst_ip,&h,&data,&dn));
    ++tcp_packets;
    if (h.flags&TCP_FIN) ++fin_packets;
    if (h.flags&TCP_RST) ++reset_packets;
    if (tx_fail) return -1;
    if (blackhole) return 0;
    if (h.flags&TCP_SYN) {
        tcp_tuple_t tuple={ip.dst_ip,ip.src_ip,h.destination,h.source};
        if (!peer_active || peer_connection.tuple.remote_port!=h.source) {
            assert(!tcp_conn_init(&peer_connection,tuple,1,0xfffffff0,1500,false,now*10));
            peer_active=true; peer_count=peer_queued=0;
        }
    }
    if (peer_active) tcp_conn_input(&peer_connection,&h,data,dn,now*10);
    return 0;
}
static void peer_service(void) {
    if (peer_active && !blackhole) {
        tcp_conn_tick(&peer_connection,now*10);
        int n=tcp_conn_peek(&peer_connection,peer_scratch,sizeof(peer_scratch));
        if (n>0) {
            assert(peer_count+(size_t)n<=sizeof(peer_bytes));
            memcpy(peer_bytes+peer_count,peer_scratch,(size_t)n); peer_count+=(size_t)n;
            assert(!tcp_conn_consume(&peer_connection,(size_t)n));
        }
        if (peer_connection.eof && peer_return) {
            if (peer_queued<peer_count) {
                int accepted=tcp_conn_queue(&peer_connection,peer_bytes+peer_queued,peer_count-peer_queued);
                if (accepted>0) peer_queued+=(size_t)accepted;
            } else assert(!tcp_conn_shutdown(&peer_connection));
        }
        tcp_action_t a;
        if (!tcp_conn_prepare(&peer_connection,&a,peer_scratch,sizeof(peer_scratch))) {
            if (refuse && (a.header.flags&TCP_SYN)) {
                a.header=(tcp_header_t){.source=a.header.source,.destination=a.header.destination,
                    .acknowledgment=peer_connection.irs+1,.flags=TCP_RST|TCP_ACK};
                a.data_len=0;
            }
            assert(!tcp_encode(peer_packet+20,1480,peer_connection.tuple.local_ip,
                peer_connection.tuple.remote_ip,&a.header,peer_scratch,a.data_len));
            size_t header=a.header.has_mss ? 24 : 20;
            assert(!ipv4_encode(peer_packet,1500,peer_connection.tuple.local_ip,
                peer_connection.tuple.remote_ip,6,(uint16_t)(header+a.data_len),64,NULL));
            /* Reply only after client commit returned: mock NIC is not RX. */
            net_ipv4_input(peer_packet,20+header+a.data_len);
            if (!refuse) assert(tcp_conn_commit(&peer_connection,&a,true));
            else peer_active=false;
        }
    }
}
static void tcp_service(void) {
    net_tcp_tick(now,true); peer_service(); ++now;
}
/* This fixture substitutes its own finite scheduler driver via a macro below. */
static void tcp_wait(const void *channel, bool (*ready)(void *), void *arg) {
    assert(channel);
    if (interrupt_wait_before_reply) {
        interrupt_wait_before_reply=false;
        net_tcp_tick(now,true); /* Submit SYN, retain peer SYN/ACK until later. */
        assert(peer_active && peer_connection.state==TCP_SYN_RCVD);
        signal_pending=true; return;
    }
    for (unsigned i=0; i<4000; ++i) {
        if (ready(arg) || signal_pending) return;
        tcp_service();
    }
    assert(!"TCP fixture wait exceeded 40 fake seconds");
}
static int stream(void) { return (int)call(SYS_SOCKET,2,1,6,0,0,0); }
static long connectfd(int fd) { return call(SYS_CONNECT,fd,(uintptr_t)&destination,16,0,0,0); }
int main(void) {
    assert(!udp_baseline_main());
    memset(&process,0,sizeof(process)); process.cpu_affinity=0;
    now=13000; signal_pending=false; online=true; cached=true;
    dev.send_packet=tcp_transmit;
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    destination=(net_sockaddr_in_t){.family=2,.port=htons(7777),.address=cfg.gateway};
    extern void (*net_host_wait_override)(const void *, bool (*)(void *), void *);
    net_host_wait_override=tcp_wait;
    int fd=stream(); assert(fd>=0);
    cpu_locals[0].id=1;
    assert(call(SYS_CONNECT,fd,0,16,0,0,0)==SYSCALL_EOPNOTSUPP);
    cpu_locals[0].id=0;
    uint64_t saved_now=now; now=0;
    assert(connectfd(fd)==SYSCALL_EAGAIN && !tcp_packets);
    now=saved_now;
    assert(call(SYS_CONNECT,fd,0,16,0,0,0)==SYSCALL_EFAULT);
    assert(call(SYS_CONNECT,fd,0,15,0,0,0)==SYSCALL_EINVAL);
    assert(call(SYS_LISTEN,fd,1,0,0,0,0)==SYSCALL_EOPNOTSUPP);
    assert(!connectfd(fd)); assert(connectfd(fd)==SYSCALL_EISCONN);
    /* Exact space-limited count; no service/ACK may free space between calls. */
    memset(stream_output,0x5a,sizeof(stream_output));
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,8185,0,0,0)==8185);
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,32,0,0,0)==7);
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    for (unsigned i=0; i<2000 && peer_count<8192; ++i) tcp_service();
    assert(peer_count==8192);
    tcp_service(); /* Deliver final peer ACK, freeing all client TX space. */
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,8185,0,0,0)==8185);
    signal_pending=true;
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,32,0,0,0)==SYSCALL_EINTR);
    signal_pending=false;
    interrupt_after_send=true;
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,32,0,0,0)==7);
    assert(signal_pending && !interrupt_after_send);
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,1,0,0,0)==SYSCALL_EINTR);
    signal_pending=false;
    assert(call(SYS_SEND,fd,(uintptr_t)stream_output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    for (unsigned i=0; i<2000 && peer_count<16384; ++i) tcp_service();
    assert(peer_count==16384);
    for (unsigned i=0; i<16384; ++i) assert(peer_bytes[i]==0x5a);
    closefd(fd); tcp_service(); peer_active=false;
    fd=stream(); assert(fd>=0); assert(!connectfd(fd));
    assert(call(SYS_RECV,fd,(uintptr_t)output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    assert(call(SYS_RECV,fd,(uintptr_t)readonly,1,0,0,0)==SYSCALL_EFAULT);
    assert(call(SYS_SEND,fd,0,0,0,0,0)==0);
    peer_return=true;
    for (unsigned i=0; i<65536; ++i) stream_output[i]=(uint8_t)(i*31);
    size_t at=0;
    while (at<65536) {
        size_t n=65536-at; if (n>16384) n=16384;
        long accepted=call(SYS_SEND,fd,(uintptr_t)(stream_output+at),n,0,0,0);
        assert(accepted>0 && (size_t)accepted<=n); at+=(size_t)accepted;
    }
    assert(!call(SYS_SHUTDOWN,fd,1,0,0,0,0));
    assert(!call(SYS_SHUTDOWN,fd,1,0,0,0,0));
    assert(call(SYS_SEND,fd,(uintptr_t)output,1,0,0,0)==SYSCALL_EPIPE);
    at=0;
    while (at<65536) {
        size_t n=65536-at; if (n>16384) n=16384;
        long got=call(SYS_RECV,fd,(uintptr_t)(stream_output+at),n,0,0,0);
        assert(got>0); at+=(size_t)got;
    }
    assert(call(SYS_RECV,fd,(uintptr_t)output,1,0,0,0)==0);
    for (unsigned i=0; i<65536; ++i) assert(stream_output[i]==(uint8_t)(i*31));
    assert(peer_count==65536);
    net_tcp_wait_t stale; assert(net_tcp_snapshot(net_socket_index(process.fd_table[fd]),&stale));
    closefd(fd); tcp_service(); assert(net_tcp_ready(&stale));
    /* Shared fd keeps endpoint live; final close alone requests orphan work. */
    peer_active=false; peer_return=false;
    fd=stream(); assert(fd>=0); assert(!connectfd(fd));
    process.fd_table[31]=process.fd_table[fd]; ++process.fd_table[fd]->ref_count;
    closefd(fd); assert(call(SYS_RECV,31,(uintptr_t)output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    signal_pending=true; assert(call(SYS_RECV,31,(uintptr_t)output,1,0,0,0)==SYSCALL_EINTR);
    signal_pending=false;
    /* Indefinite receive crosses former 5/7s leases without reservation/data loss. */
    tcp_service(); net_tcp_wait_t idle;
    assert(net_tcp_snapshot(net_socket_index(process.fd_table[31]),&idle));
    for (unsigned i=0; i<1600; ++i) tcp_service();
    assert(!net_tcp_ready(&idle)); /* No timer-only wake churn for idle receive. */
    assert(call(SYS_RECV,31,(uintptr_t)output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    closefd(31); tcp_service();
    refuse=true; peer_active=false; fd=stream(); assert(fd>=0);
    assert(connectfd(fd)==SYSCALL_ECONNREFUSED); closefd(fd); tcp_service(); refuse=false;
    blackhole=true; peer_active=false; fd=stream(); assert(fd>=0);
    assert(connectfd(fd)==SYSCALL_ETIMEDOUT); closefd(fd); tcp_service(); blackhole=false;
    peer_active=false; fd=stream(); assert(fd>=0); signal_pending=true;
    assert(connectfd(fd)==SYSCALL_EINTR); signal_pending=false;
    tcp_service(); closefd(fd); tcp_service();
    fd=stream(); assert(fd>=0); closefd(fd); tcp_service();
    cached=false; peer_active=false; fd=stream(); assert(fd>=0);
    assert(connectfd(fd)==SYSCALL_ETIMEDOUT);
    unsigned arp_before=arps;
    for (unsigned i=0; i<200; ++i) tcp_service();
    assert(arps==arp_before); /* Closed no-action endpoint must stop ARP work. */
    closefd(fd); tcp_service(); cached=true;
    /* SIGINT interrupts the real CONNECT continuation after SYN submission.
     * Inject SYN/ACK after cancellation publication but before any worker tick,
     * at the exact same fake timestamp: the subsequent detach must use FIN. */
    peer_active=false; fd=stream(); assert(fd>=0);
    interrupt_wait_before_reply=true;
    assert(connectfd(fd)==SYSCALL_EINTR && signal_pending);
    signal_pending=false;
    unsigned fins_before=fin_packets, resets_before=reset_packets;
    peer_service(); /* SYN/ACK reaches client while cancellation is pending. */
    assert(net_tcp_connected(net_socket_index(process.fd_table[fd]))==SYSCALL_ENOTCONN);
    for (unsigned i=0; i<10 && fin_packets==fins_before; ++i) tcp_service();
    assert(fin_packets==fins_before+1 && reset_packets==resets_before);
    assert(peer_connection.eof);
    closefd(fd); tcp_service();
    puts("[PASS] CONNECT cancel/SYN-ACK same-timestamp race uses FIN; exact SEND space count, pending signal zero acceptance, post-accept signal preserves count/exact bytes");
    assert(!allocations && tcp_packets>50);
    puts("TCP socket host PASS: actual manager/syscalls/codec; 64KiB both directions, half-close, EOF, short sends, shared refs, stale identity, interruption/refusal/timeout; mocked scheduler and peer engine");
    return 0;
}
