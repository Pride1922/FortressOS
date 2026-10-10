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
static uint32_t last_syn_seq;
static unsigned fin_packets, reset_packets;
static bool interrupt_after_send, interrupt_wait_before_reply, expire_wait_with_reply;
static bool listener_fixture, invalidate_listener_wait;
static bool disconnect_listener_wait;
static unsigned listener_wait_slot;
static tcp_header_t synacks[16];
static unsigned adoption_listener, adoption_checks;
static net_tcp_child_t expected_child;
extern void (*net_host_unlock_hook)(const char *);
static unsigned close_reuse_checks;
static void reuse_after_common_close(const char *name) {
    if (strcmp(name,"socket_table")) return;
    net_host_unlock_hook=NULL;
    file_t *staged=NULL;
    /* All other common slots occupied: constructor overwrites the just-freed
     * common slot's kind, then fails because old TCP close is not published
     * yet. The original close must use its captured adopted kind. */
    assert(net_socket_stage_stream(&staged)==SYSCALL_ENOSPC && !staged);
    ++close_reuse_checks;
}
static void inserted_before_removal(tcb_t *caller, unsigned fd) {
    net_tcp_child_t child; net_sockaddr_in_t peer; net_tcp_wait_t wait;
    assert(caller==&process && net_socket_stream(caller->fd_table[fd]));
    assert(!net_tcp_accept_peek(adoption_listener,&child,&peer));
    assert(child.block==expected_child.block && child.generation==expected_child.generation);
    assert(!net_tcp_snapshot(net_socket_index(caller->fd_table[fd]),&wait));
    ++adoption_checks;
}
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
    if (listener_fixture) {
        if ((h.flags&(TCP_SYN|TCP_ACK))==(TCP_SYN|TCP_ACK) && h.destination>=50000 && h.destination<50016)
            synacks[h.destination-50000]=h;
        return 0;
    }
    if (tx_fail) return -1;
    if (blackhole) return 0;
    if (h.flags&TCP_SYN) {
        last_syn_seq = h.sequence;
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
    if (disconnect_listener_wait) {
        disconnect_listener_wait=false;
        net_socket_worker_tick(now,false); net_tcp_tick(now,false);
        assert(ready(arg)); return;
    }
    if (invalidate_listener_wait) {
        invalidate_listener_wait=false;
        net_tcp_close(listener_wait_slot); net_tcp_tick(now,true); return;
    }
    if (interrupt_wait_before_reply) {
        interrupt_wait_before_reply=false;
        net_tcp_tick(now,true); /* Submit SYN, retain peer SYN/ACK until later. */
        assert(peer_active && peer_connection.state==TCP_SYN_RCVD);
        signal_pending=true; return;
    }
    if (expire_wait_with_reply) {
        expire_wait_with_reply=false;
        net_tcp_wait_t *wait=arg;
        net_tcp_tick(now,true); /* SYN submitted before the peer's SYN/ACK. */
        assert(peer_active && peer_connection.state==TCP_SYN_RCVD);
        now=wait->deadline_ticks; peer_service();
        assert(!net_tcp_connected(wait->slot)); /* handshake raced to completion */
        return; /* syscall must cancel, despite the now-established transport */
    }
    for (unsigned i=0; i<4000; ++i) {
        if (ready(arg) || signal_pending) return;
        tcp_service();
    }
    assert(!"TCP fixture wait exceeded 40 fake seconds");
}
static int stream(void) { return (int)call(SYS_SOCKET,2,1,6,0,0,0); }
static long connectfd(int fd) { return call(SYS_CONNECT,fd,(uintptr_t)&destination,16,0,0,0); }
static void incoming_at(unsigned index, uint32_t sequence, unsigned flags, uint32_t ack, const void *bytes, size_t length) {
    tcp_header_t h={.source=(uint16_t)(50000+index),.destination=9000,
        .sequence=sequence,.acknowledgment=ack,.flags=(uint8_t)flags,.window=8192};
    assert(!tcp_encode(peer_packet+20,1480,destination.address,htonl(0x0a00020f),&h,bytes,length));
    assert(!ipv4_encode(peer_packet,1500,destination.address,htonl(0x0a00020f),6,(uint16_t)(20+length),64,NULL));
    net_ipv4_input(peer_packet,40+length);
}
static void incoming(unsigned index, unsigned flags, uint32_t ack, const void *bytes, size_t length) {
    incoming_at(index,100+index+((flags&TCP_SYN) ? 0 : 1),flags,ack,bytes,length);
}
static void passive_open(unsigned index, bool complete) {
    incoming(index,TCP_SYN,0,NULL,0); net_tcp_tick(now,true);
    assert(synacks[index].flags==(TCP_SYN|TCP_ACK));
    if (complete) {
        incoming(index,TCP_ACK,synacks[index].sequence+1,NULL,0); net_tcp_tick(now,true);
    }
}
static long acceptfd(int fd, net_sockaddr_in_t *peer, uint32_t *size) {
    return call(SYS_ACCEPT,fd,(uintptr_t)peer,(uintptr_t)size,NET_SOCK_CLOEXEC,0,0);
}
static int listenfd(void) {
    int fd=stream(); assert(fd>=0);
    net_sockaddr_in_t local={.family=2,.port=htons(9000)};
    assert(!call(SYS_BIND,fd,(uintptr_t)&local,16,0,0,0));
    assert(call(SYS_LISTEN,fd,0,0,0,0,0)==SYSCALL_EINVAL);
    assert(call(SYS_LISTEN,fd,0x100000001ull,0,0,0,0)==SYSCALL_EINVAL);
    assert(!call(SYS_LISTEN,fd,4,0,0,0,0));
    assert(call(SYS_LISTEN,fd,4,0,0,0,0)==SYSCALL_EINVAL);
    return fd;
}
static int64_t stream_receive_call(unsigned nr, int fd, void *data, size_t capacity) {
    interrupt_frame_t frame={.rax=nr,.rdi=(uint64_t)fd,.rsi=(uintptr_t)data,.rdx=capacity};
    /* SYS_READ reaches TCP from kernel SYS_READ dispatch, not the socket-only
     * numbered-syscall dispatcher used by call() above. */
    return nr==SYS_READ ? net_tcp_syscall(&frame) : net_socket_syscall(&frame);
}
static void listener_data_fin_tests(net_config_t *cfg) {
    static const uint8_t payload[]="fortress-tcp-inbound\n";
    uint8_t received[64];
    /* Combined segment and adjacent segments, before/after ACCEPT, through
     * both real READ and RECV dispatch. No worker tick/read separates data/FIN. */
    for (unsigned mode=0; mode<16; ++mode) {
        assert(!allocations); peer_active=false; listener_fixture=true;
        net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
        int listener=listenfd(); memset(synacks,0,sizeof(synacks)); passive_open(0,true);
        int child=-1;
        if (mode&2) { child=(int)acceptfd(listener,NULL,NULL); assert(child>=0); }
        if (mode&1) {
            incoming(0,TCP_ACK|TCP_PSH,synacks[0].sequence+1,payload,sizeof(payload)-1);
            incoming_at(0,101+sizeof(payload)-1,TCP_ACK|TCP_FIN,synacks[0].sequence+1,NULL,0);
        } else incoming(0,TCP_ACK|TCP_PSH|TCP_FIN,synacks[0].sequence+1,payload,sizeof(payload)-1);
        net_tcp_tick(now,true);
        if (child<0) { child=(int)acceptfd(listener,NULL,NULL); assert(child>=0); }
        closefd(listener); net_tcp_tick(now,true); /* Child must retain buffered RX. */
        long nr=(mode&4) ? SYS_RECV : SYS_READ;
        memset(received,0,sizeof(received));
        assert(call(SYS_RECV_UNTIL,child,(uintptr_t)received,sizeof(received),0,now,0)==SYSCALL_ETIMEDOUT);
        for(unsigned i=0;i<sizeof(received);++i) assert(!received[i]);
        size_t first=(mode&8) ? sizeof(payload)-1 : 5;
        assert(stream_receive_call(nr,child,received,first)==(long)first);
        assert(!memcmp(received,payload,first));
        if (first<sizeof(payload)-1) {
            assert(stream_receive_call(nr,child,received,sizeof(received))==(long)(sizeof(payload)-1-first));
            assert(!memcmp(received,payload+first,sizeof(payload)-1-first));
        }
        assert(stream_receive_call(nr,child,received,sizeof(received))==0);
        assert(stream_receive_call(nr,child,received,sizeof(received))==0);
        closefd(child); net_tcp_tick(now,true); listener_fixture=false;
        assert(!allocations);
    }
    puts("[PASS] listener data+FIN combined/adjacent, pre/post ACCEPT, READ/RECV: bytes before EOF after listener close");
}
static void repeated_receive_event_test(net_config_t *cfg) {
    assert(!allocations); peer_active=false; listener_fixture=true;
    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    int listener=listenfd(); memset(synacks,0,sizeof(synacks)); passive_open(0,true);
    int child=(int)acceptfd(listener,NULL,NULL); assert(child>=0);
    incoming(0,TCP_ACK|TCP_PSH,synacks[0].sequence+1,"ab",2); net_tcp_tick(now,true);
    unsigned before_poll=poll_requests;
    unsigned before_yield=handoff_yields;
    uint8_t bytes[2]; assert(stream_receive_call(SYS_RECV,child,bytes,2)==2 && !memcmp(bytes,"ab",2));
    assert(poll_requests>before_poll); /* Application drain requests ACK service. */
    assert(handoff_yields==before_yield+1);
    net_tcp_wait_t wait; assert(net_tcp_snapshot(net_socket_index(process.fd_table[child]),&wait));
    /* No zero-count worker sample between consume and equal-sized next batch. */
    incoming_at(0,103,TCP_ACK|TCP_PSH,synacks[0].sequence+1,"cd",2);
    assert(!net_tcp_ready(&wait)); net_tcp_tick(now,true);
    assert(net_tcp_ready(&wait));
    assert(stream_receive_call(SYS_RECV,child,bytes,2)==2 && !memcmp(bytes,"cd",2));
    closefd(listener); closefd(child); net_tcp_tick(now,true); listener_fixture=false;
    assert(!allocations);
    puts("[PASS] equal-sized RX batches separated by userspace consume publish a fresh wake event without a zero-count worker sample");
}
static void listener_link_loss_test(net_config_t *cfg) {
    assert(!allocations); peer_active=false; listener_fixture=true;
    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    int listener=listenfd(); disconnect_listener_wait=true;
    assert(acceptfd(listener,NULL,NULL)==SYSCALL_EIO && !disconnect_listener_wait);
    assert(stream()==SYSCALL_EIO); /* No new socket while disconnected. */
    net_socket_worker_tick(now,true); net_tcp_tick(now,true);
    assert(acceptfd(listener,NULL,NULL)==SYSCALL_EIO); /* Old endpoint stays failed. */
    closefd(listener); net_tcp_tick(now,true);
    listener=listenfd(); memset(synacks,0,sizeof(synacks)); passive_open(0,true);
    int child=(int)acceptfd(listener,NULL,NULL); assert(child>=0);
    closefd(listener); closefd(child); net_tcp_tick(now,true); listener_fixture=false;
    assert(!allocations);
    puts("[PASS] blocked ACCEPT link loss returns EIO; old listener stays failed after replug; fresh listener binds and accepts");
}
static void listener_tests(net_config_t *cfg) {
    assert(!allocations); peer_active=false; listener_fixture=true;
    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    int listener=listenfd(); unsigned slot=net_socket_index(process.fd_table[listener]);
    incoming(0,TCP_SYN|TCP_ECE|TCP_CWR,0,NULL,0); net_tcp_tick(now,true);
    assert(synacks[0].flags==(TCP_SYN|TCP_ACK)); /* Decline ECN, do not drop SYN. */
    passive_open(0,false); passive_open(1,true);
    passive_open(2,false); passive_open(3,false);
    incoming(4,TCP_SYN,0,NULL,0); net_tcp_tick(now,true);
    assert(!synacks[4].flags); /* One combined backlog bounds half-open+ready. */
    incoming(0,TCP_SYN,0,NULL,0); net_tcp_tick(now,true); /* Duplicate consumes no entry. */
    net_tcp_child_t before, after; net_sockaddr_in_t peer;
    assert(!net_tcp_accept_peek(slot,&before,&peer) && peer.port==htons(50001));
    /* Completed child behind a half-open head stays eligible. */
    uint32_t size=16;
    assert(acceptfd(listener,(net_sockaddr_in_t *)readonly,&size)==SYSCALL_EFAULT);
    assert(call(SYS_ACCEPT,listener,(uintptr_t)&peer,0,0,0,0)==SYSCALL_EINVAL);
    assert(call(SYS_ACCEPT,listener,(uintptr_t)&peer,(uintptr_t)&peer,0,0,0)==SYSCALL_EINVAL);
    size=15; assert(acceptfd(listener,&peer,&size)==SYSCALL_EINVAL); size=16;
    signal_pending=true; assert(acceptfd(listener,&peer,&size)==SYSCALL_EINTR); signal_pending=false;
    unsigned baseline=allocations;
    for (int failure=0; failure<2; ++failure) {
        fail_alloc=failure; assert(acceptfd(listener,&peer,&size)==SYSCALL_ENOMEM);
        fail_alloc=-1; assert(allocations==baseline);
    }
    for (unsigned i=0; i<32; ++i) if (!process.fd_table[i]) {
        process.fd_table[i]=process.fd_table[listener]; ++process.fd_table[listener]->ref_count;
    }
    assert(acceptfd(listener,&peer,&size)==SYSCALL_EMFILE && allocations==baseline);
    for (unsigned i=0; i<32; ++i) if ((int)i!=listener) closefd((int)i);
    int udp_fds[15];
    for (unsigned i=0; i<15; ++i) { udp_fds[i]=create(); assert(udp_fds[i]>=0); }
    assert(acceptfd(listener,&peer,&size)==SYSCALL_ENOSPC);
    for (unsigned i=0; i<15; ++i) closefd(udp_fds[i]);
    assert(!net_tcp_accept_peek(slot,&after,&peer));
    assert(before.block==after.block && before.generation==after.generation);
    int fillers[3];
    for (unsigned i=0; i<3; ++i) { fillers[i]=stream(); assert(fillers[i]>=0); }
    assert(stream()==SYSCALL_ENOSPC); /* All eight blocks occupied. */
    adoption_listener=slot; expected_child=before;
    net_host_fd_inserted=inserted_before_removal;
    int child=(int)acceptfd(listener,&peer,&size); assert(child>=0 && size==16 && peer.port==htons(50001));
    net_host_fd_inserted=NULL; assert(adoption_checks==1);
    assert(process.fd_flags[child]==FD_FLAG_CLOEXEC);
    /* A second acceptor cannot acquire the transferred child. */
    assert(net_tcp_accept_peek(slot,&after,&peer)==SYSCALL_EAGAIN);
    for (unsigned i=0; i<3; ++i) closefd(fillers[i]);
    closefd(listener); net_tcp_tick(now,true);
    /* Accepted child retains tuple ownership but does not monopolize its
     * server's port. A replacement listener can bind while child is live. */
    int replacement=listenfd(); closefd(replacement); net_tcp_tick(now,true);
    incoming(1,TCP_ACK,synacks[1].sequence+1,"child survives",14);
    assert(call(SYS_RECV,child,(uintptr_t)output,14,NET_MSG_DONTWAIT,0,0)==14);
    assert(!memcmp(output,"child survives",14));
    int occupied[15];
    for (unsigned i=0; i<15; ++i) { occupied[i]=create(); assert(occupied[i]>=0); }
    net_host_unlock_hook=reuse_after_common_close;
    closefd(child); assert(close_reuse_checks==1 && !net_host_unlock_hook);
    for (unsigned i=0; i<15; ++i) closefd(occupied[i]);
    net_tcp_tick(now,true);
    now+=12100; net_tcp_tick(now,true); now+=101; net_tcp_tick(now,true);
    /* Half-open expiry frees backlog entries and real pool/TIME_WAIT slots. */
    listener=listenfd(); slot=net_socket_index(process.fd_table[listener]);
    memset(synacks,0,sizeof(synacks)); passive_open(0,false);
    int full_pool[6];
    for (unsigned i=0; i<6; ++i) { full_pool[i]=stream(); assert(full_pool[i]>=0); }
    incoming(2,TCP_SYN,0,NULL,0); net_tcp_tick(now,true);
    assert(!synacks[2].flags); /* Global block shortage below backlog capacity. */
    for (unsigned i=0; i<6; ++i) closefd(full_pool[i]);
    net_tcp_tick(now,true);
    now+=3001; net_tcp_tick(now,true); now+=101; net_tcp_tick(now,true);
    passive_open(1,true);
    child=(int)acceptfd(listener,NULL,NULL); assert(child>=0);
    closefd(child); net_tcp_tick(now,true);
    /* Empty wait has no staged allocations; forced identity invalidation
     * models the defensive EBADF path, not shared fd-table close. */
    listener_wait_slot=slot; invalidate_listener_wait=true; baseline=allocations;
    assert(acceptfd(listener,NULL,NULL)==SYSCALL_EBADF && allocations==baseline);
    closefd(listener); net_tcp_tick(now,true);
    assert(!allocations); listener_fixture=false;
    puts("[PASS] listener backlog/duplicate SYN/half-open expiry; output/signal/heap/fd/handle rollback; full-pool adoption; child independence; single delivery; defensive invalidation");
}
int main(void) {
    if (getenv("NET_TCP_EXPIRY_GUARD")) {
        extern void net_tcp_test_expiry_guard(void);
        net_tcp_test_expiry_guard(); return 99;
    }
    assert(!udp_baseline_main());
    memset(&process,0,sizeof(process)); process.cpu_affinity=0;
    now=13000; signal_pending=false; online=true; cached=true;
    dev.send_packet=tcp_transmit;
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    /* Existing shell startup resets BSP uptime with no deadline hint. */
    now=0; net_tcp_tick(now,true); assert(net_tcp_deadline_clock_ok());
    now=13000;
    destination=(net_sockaddr_in_t){.family=2,.port=htons(7777),.address=cfg.gateway};
    extern void (*net_host_wait_override)(const void *, bool (*)(void *), void *);
    net_host_wait_override=tcp_wait;
    /* Approved per-call deadlines: real manager/syscall, fake BSP clock. */
    int timedfd=stream(); assert(timedfd>=0);
    assert(call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,now+6001,0,0)==SYSCALL_EINVAL);
    unsigned before=tcp_packets;
    assert(call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,now,0,0)==SYSCALL_ETIMEDOUT);
    assert(tcp_packets==before);
    assert(!call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,now+100,0,0));
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)readonly,1,0,now,0)==SYSCALL_EFAULT);
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,NET_MSG_DONTWAIT,now+10,0)==SYSCALL_EINVAL);
    assert(call(SYS_RECV_UNTIL,timedfd,0,0,0,0,0)==0);
    signal_pending=true;
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,0,now,0)==SYSCALL_EINTR);
    signal_pending=false;
    uint64_t expiry=now+3;
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,0,expiry,0)==SYSCALL_ETIMEDOUT && now>=expiry);
    unsigned slot=net_socket_index(process.fd_table[timedfd]);
    net_tcp_wait_t early,later; assert(net_tcp_snapshot(slot,&early)); later=early;
    early.timed=later.timed=true; early.deadline_ticks=now+2; later.deadline_ticks=now+5;
    assert(net_tcp_deadline_register(&later) && net_tcp_deadline_register(&early));
    net_tcp_tick(now,true); /* Flush any unrelated pending event first. */
    assert(net_tcp_snapshot(slot,&early)); later=early;
    early.timed=later.timed=true; early.deadline_ticks=now+2; later.deadline_ticks=now+5;
    assert(net_tcp_deadline_register(&early) && net_tcp_deadline_register(&later));
    now+=2; unsigned wake_before=wakes; net_tcp_tick(now,true);
    assert(wakes>wake_before && net_tcp_ready(&early) && net_tcp_ready(&later));
    assert(net_tcp_snapshot(slot,&later)); later.timed=true; later.deadline_ticks=now+3;
    assert(net_tcp_deadline_register(&later)); now+=3; net_tcp_tick(now,true); assert(net_tcp_ready(&later));
    blackhole=true;
    assert(call(SYS_SEND_UNTIL,timedfd,(uintptr_t)stream_output,8192,0,now+10,0)==8192);
    expiry=now+3;
    assert(call(SYS_SEND_UNTIL,timedfd,(uintptr_t)stream_output,1,0,expiry,0)==SYSCALL_ETIMEDOUT);
    net_tcp_wait_t deadline_stale=early; deadline_stale.timed=true; deadline_stale.deadline_ticks=now+20;
    closefd(timedfd); tcp_service(); peer_active=false;
    assert(!net_tcp_deadline_register(&deadline_stale) && net_tcp_ready(&deadline_stale));
    timedfd=stream(); assert(timedfd>=0); expiry=now+3;
    assert(call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,expiry,0,0)==SYSCALL_ETIMEDOUT);
    tcp_service(); assert(net_tcp_connected(net_socket_index(process.fd_table[timedfd]))==SYSCALL_ENOTCONN);
    closefd(timedfd); tcp_service(); blackhole=false; peer_active=false;
    timedfd=stream(); expire_wait_with_reply=true; expiry=now+3;
    assert(call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,expiry,0,0)==SYSCALL_ETIMEDOUT);
    before=fin_packets; unsigned timed_resets=reset_packets;
    for (unsigned i=0; i<10 && fin_packets==before; ++i) tcp_service();
    assert(fin_packets==before+1 && reset_packets==timed_resets && peer_connection.eof);
    assert(net_tcp_connected(net_socket_index(process.fd_table[timedfd]))==SYSCALL_ENOTCONN);
    closefd(timedfd); tcp_service(); peer_active=false;
    assert(!allocations); timer_hz=1000; now=130000;
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    timedfd=stream(); assert(timedfd>=0);
    assert(!call(SYS_CONNECT_UNTIL,timedfd,(uintptr_t)&destination,16,now+100,0,0));
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,0,now+60001,0)==SYSCALL_EINVAL);
    expiry=now+3;
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,0,expiry,0)==SYSCALL_ETIMEDOUT && now>=expiry);
    /* A decreasing clock must wake hints and fail closed, not wait for wrap. */
    net_tcp_wait_t reset_wait; assert(net_tcp_snapshot(net_socket_index(process.fd_table[timedfd]),&reset_wait));
    reset_wait.timed=true; reset_wait.deadline_ticks=now+100; reset_wait.clock_floor=now;
    assert(net_tcp_deadline_register(&reset_wait)); now-=2; net_tcp_tick(now,true);
    assert(net_tcp_ready(&reset_wait) && !net_tcp_deadline_clock_ok());
    assert(call(SYS_RECV_UNTIL,timedfd,(uintptr_t)output,1,0,now+100,0)==SYSCALL_EIO);
    closefd(timedfd); tcp_service(); peer_active=false; timer_hz=100;
    puts("[PASS] timed connect/send/receive expiry, no-start expired CONNECT, scalar/pointer/signal precedence, zero length, shared earliest hint/rearm and generation-checked cancellation");
    assert(!allocations); now=13000; tcp_packets=fin_packets=reset_packets=0;
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    int fd=stream(); assert(fd>=0);
    cpu_locals[0].id=1;
    assert(call(SYS_CONNECT,fd,0,16,0,0,0)==SYSCALL_EOPNOTSUPP);
    cpu_locals[0].id=0;
    assert(call(SYS_CONNECT,fd,0,16,0,0,0)==SYSCALL_EFAULT);
    assert(call(SYS_CONNECT,fd,0,15,0,0,0)==SYSCALL_EINVAL);
    assert(call(SYS_LISTEN,fd,1,0,0,0,0)==SYSCALL_EINVAL);
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
    unsigned no_handoff=handoff_yields;
    assert(call(SYS_RECV,fd,(uintptr_t)output,1,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN);
    assert(call(SYS_RECV,fd,(uintptr_t)readonly,1,0,0,0)==SYSCALL_EFAULT);
    assert(call(SYS_RECV,fd,0,0,0,0,0)==0);
    assert(handoff_yields==no_handoff);
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
    no_handoff=handoff_yields;
    assert(call(SYS_RECV,fd,(uintptr_t)output,1,0,0,0)==0);
    assert(handoff_yields==no_handoff);
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
    listener_tests(&cfg);
    listener_data_fin_tests(&cfg);
    repeated_receive_event_test(&cfg);
    listener_link_loss_test(&cfg);
    /* Event-counter exhaustion invalidates identity and clears timed hints;
     * neither the old hint nor an old continuation may attach to slot reuse. */
    peer_active=false; now=13000;
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    fd=stream(); assert(!connectfd(fd));
    unsigned exhausted_slot=net_socket_index(process.fd_table[fd]);
    net_tcp_wait_t exhausted; assert(net_tcp_snapshot(exhausted_slot,&exhausted));
    exhausted.timed=true; exhausted.deadline_ticks=now+100; exhausted.clock_floor=now;
    assert(net_tcp_deadline_register(&exhausted));
    extern void net_tcp_test_event_exhaustion(unsigned);
    net_tcp_test_event_exhaustion(exhausted_slot);
    assert(net_tcp_ready(&exhausted) && !net_tcp_deadline_register(&exhausted));
    assert(call(SYS_RECV_UNTIL,fd,(uintptr_t)output,1,0,now+100,0)==SYSCALL_ENOTCONN);
    closefd(fd); tcp_service();
    fd=stream(); assert(net_socket_index(process.fd_table[fd])==exhausted_slot);
    assert(!net_tcp_deadline_register(&exhausted));
    closefd(fd); tcp_service(); assert(!allocations);
    /* Verify distinct ISNs across sequential connections */
    uint32_t seq1 = 0, seq2 = 0;
    fd = stream(); assert(!connectfd(fd)); seq1 = last_syn_seq; closefd(fd); tcp_service();
    now += 10;
    fd = stream(); assert(!connectfd(fd)); seq2 = last_syn_seq; closefd(fd); tcp_service();
    assert(seq1 != 0 && seq2 != 0 && seq1 != seq2);
    puts("[PASS] RFC 6528 cryptographic ISN distinct across sequential connections");
    puts("[PASS] TCP event exhaustion invalidates timed waits and rejects hints across slot reuse");
    puts("TCP socket host PASS: actual manager/syscalls/codec; 64KiB both directions, half-close, EOF, short sends, shared refs, stale identity, interruption/refusal/timeout; mocked scheduler and peer engine");
    return 0;
}
