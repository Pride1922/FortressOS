/* Actual TCP client/stream owner and syscall code, deterministic scheduler and
 * transport peer adapters inherited from the legacy socket fixture. */
#define TCP_OWNER_FIXTURE
#include "net_tcp_socket_host.c"
#undef main
static unsigned snapshot_ack_checks, revoke_checks;
static unsigned late_copy_checks;
static void copy_after_snapshot(const char *name) {
    if (strcmp(name,"udp_handoff_values")) return;
    net_host_unlock_hook=NULL;
    const uint8_t *data; int64_t count;
    assert(net_udp_pin(&data,&count,NULL) && count==4 && !memcmp(data,"ijkl",4));
    net_udp_unpin(true); ++late_copy_checks;
}
static void ack_after_snapshot(const char *name) {
    if (strcmp(name,"udp_handoff_values")) return;
    net_host_unlock_hook=NULL;
    assert(net_tcp_owner_connect_ack()); ++snapshot_ack_checks;
}
static void pin_after_revoke(const char *name) {
    if (strcmp(name,"udp_handoff_values")) return;
    net_host_unlock_hook=NULL;
    const uint8_t *data; int64_t count;
    assert(!net_udp_pin(&data,&count,NULL)); ++revoke_checks;
}
static void pending_step(handoff_token_t wanted,unsigned kind) {
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        handoff_token_t token; handoff_request_t request;
        assert(handoff_pending(&token,&request));
        if (token.slot==wanted.slot && token.generation==wanted.generation) {
            assert(request.kind==kind); net_udp_step(token,request); return;
        }
        assert(handoff_finish(token,(handoff_reply_t){0},true));
    }
    assert(!"pending token not found");
}
static bool cancel_offer;
static uint64_t expire_offer;
static uint64_t expire_admission;
static unsigned admission_packets;
static void owner_wait(const void *channel,bool (*ready)(void *),void *arg) {
    assert(channel);
    if (channel==&handoff_admission_channel && expire_admission) {
        handoff_token_t token; handoff_request_t request;
        for (unsigned i=0;i<24;++i) {
            assert(handoff_claim(&token,&request) && request.kind==99);
            assert(handoff_finish(token,(handoff_reply_t){.status=SYSCALL_EINVAL},false));
        }
        /* The sleeping publisher receives its FIFO grant and the owner starts
         * CONNECT without another caller-side publish attempt. */
        for (unsigned i=0;i<32 && tcp_packets==admission_packets;++i) { tick(); tcp_service(); }
        if (tcp_packets==admission_packets) {
            handoff_reply_t diagnostic={0};
            int collected=net_udp_collect(&diagnostic);
            fprintf(stderr,"admission diagnostic collect=%d status=%lld live=%u pending=%u now=%llu expiry=%llu\n",
                collected,(long long)diagnostic.status,handoff_stats().live,handoff_stats().pending,
                (unsigned long long)now,(unsigned long long)expire_admission);
        }
        assert(tcp_packets>admission_packets);
        now=expire_admission; expire_admission=0; return;
    }
    if (ready==net_tcp_ready) { tcp_wait(channel,ready,arg); return; }
    for (unsigned i=0;i<10000;++i) {
        if (ready(arg) || signal_pending) {
            if (ready==net_udp_ready && cancel_offer) { cancel_offer=false; signal_pending=true; }
            if (ready==net_udp_ready && expire_offer) { now=expire_offer; expire_offer=0; }
            return;
        }
        tcp_service(); tick();
    }
    assert(!"TCP owner wait exceeded bound");
}
static int64_t io(unsigned nr,int fd,void *p,size_t n,uint64_t deadline) {
    interrupt_frame_t f={.rax=nr,.rdi=fd,.rsi=(uintptr_t)p,.rdx=n,.r8=deadline};
    return net_tcp_syscall(&f);
}
static void connect_admission_expiry(void) {
    peer_active=false; peer_return=false; blackhole=true;
    int fd=stream(); assert(fd>=0);
    for (unsigned i=0;i<64;++i) { tick(); tcp_service(); }
    handoff_registration_t occupied[24]={0};
    for (unsigned i=0;i<24;++i) {
        assert(!handoff_begin(&occupied[i],(handoff_request_t){.kind=99,.actor=3000+i}));
        assert(!handoff_publish(occupied[i].token));
    }
    assert(handoff_stats().data==24);
    expire_admission=now+100; admission_packets=tcp_packets;
    assert(call(SYS_CONNECT_UNTIL,fd,(uintptr_t)&destination,16,expire_admission,0,0)==SYSCALL_ETIMEDOUT);
    assert(!expire_admission && !process.handoff.active);
    assert(net_tcp_connected(net_socket_index(process.fd_table[fd]))==SYSCALL_ENOTCONN);
    for (unsigned i=0;i<24;++i) handoff_release(&occupied[i]);
    closefd(fd);
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations); blackhole=false;
    puts("PASS CONNECT FIFO grant/claim while caller waits for admission; expired resumed caller resolves owner cancellation before return and all 25 records reclaim");
}
static void shared_payload_saturation(void) {
    peer_active=false; peer_return=false;
    int fd=stream(); assert(fd>=0); assert(!connectfd(fd));
    int udp=create(); assert(udp>=0);
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    file_t *file=process.fd_table[fd], *datagram=process.fd_table[udp];
    assert(net_udp_begin(NET_OWNER_TCP_SEND,file,NULL,4097,false)==SYSCALL_EINVAL);
    assert(net_udp_begin(NET_OWNER_TCP_RECV,file,NULL,65537,false)==SYSCALL_EINVAL);
    uint64_t before=handoff_stats().reclaimed;
    handoff_registration_t pins[8]; const uint8_t *buffers[8]; int64_t count;
    for (unsigned i=0;i<8;++i) {
        process.tid=2000+i; process.handoff=(handoff_registration_t){0};
        assert(!net_udp_begin(NET_OWNER_TCP_SEND,file,NULL,1,false)); pins[i]=process.handoff;
    }
    for (unsigned i=0;i<16;++i) tick();
    for (unsigned i=0;i<8;++i) {
        process.tid=2000+i; process.handoff=pins[i];
        assert(net_udp_pin(&buffers[i],&count,NULL) && count==1);
        for (unsigned j=0;j<i;++j) assert(buffers[i]!=buffers[j]);
    }
    process.tid=2010; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_TCP_SEND,file,NULL,1,true));
    for (unsigned i=0;i<16;++i) tick();
    handoff_reply_t reply; assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EAGAIN); net_udp_release();
    process.tid=2013; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_TCP_RECV,file,NULL,1,false));
    handoff_registration_t eof=process.handoff;
    for (unsigned i=0;i<16;++i) tick();
    assert(!net_udp_ready(&eof.token));
    assert(!tcp_conn_shutdown(&peer_connection)); tcp_service();
    net_udp_refresh_work(); handoff_acknowledge();
    assert(!handoff_owner_ready(NULL) && net_udp_owner_work());
    pending_step(eof.token,NET_OWNER_TCP_RECV);
    assert(!net_udp_collect(&reply) && reply.status==0); net_udp_release();
    process.tid=2011; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_UDP_SEND,datagram,&destination,1,false));
    handoff_registration_t oldest=process.handoff;
    for (unsigned i=0;i<16;++i) tick();
    assert(!net_udp_ready(&oldest.token));
    process.tid=2000; process.handoff=pins[0]; net_udp_unpin(false); net_udp_release();
    /* The older UDP ticket must receive the first released TCP credit. */
    for (unsigned i=0;i<16 && !net_udp_ready(&oldest.token);++i) tick();
    process.tid=2011; process.handoff=oldest;
    const uint8_t *data; assert(net_udp_pin(&data,&count,NULL) && count==1);
    process.tid=2012; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_TCP_SEND,file,NULL,1,false));
    handoff_registration_t newest=process.handoff;
    for (unsigned i=0;i<16;++i) tick();
    assert(!net_udp_pin(&data,&count,NULL));
    net_udp_abandon();
    for (unsigned i=1;i<8;++i) {
        process.tid=2000+i; process.handoff=pins[i]; net_udp_unpin(false); net_udp_release();
    }
    process.tid=2011; process.handoff=oldest; net_udp_unpin(false); net_udp_release();
    (void)newest;
    process.tid=1; process.handoff=(handoff_registration_t){0};
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && handoff_stats().reclaimed-before==12);
    assert(file->ref_count==1 && datagram->ref_count==1 && !net_udp_owner_work());
    assert(peer_count==0); /* None of these copied offers was accepted as TX. */
    closefd(fd); closefd(udp);
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!allocations && !handoff_stats().live);
    puts("PASS eight pinned TCP slabs, ninth nonblocking EAGAIN, shared UDP/TCP FIFO grant, slab-free EOF level readiness, twelve exact reclamations and base references restored");
}
int main(void) {
    if (getenv("NET_TCP_EXPIRY_GUARD")) {
        extern void net_tcp_test_expiry_guard(void);
        net_tcp_test_expiry_guard(); return 99;
    }
    process.tid=1; process.cpu_affinity=0; handoff_start();
    dev=(net_dev_t){.mtu=1500,.send_packet=tcp_transmit};
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    destination=(net_sockaddr_in_t){.family=2,.port=htons(7777),.address=cfg.gateway};
    net_host_wait_override=owner_wait;
    int fd=stream(); assert(fd>=0); assert(!connectfd(fd));
    assert(connectfd(fd)==SYSCALL_EISCONN);
    static uint8_t input[65536], result[65536];
    for (unsigned i=0;i<sizeof(input);++i) input[i]=(uint8_t)(i*37+(i>>8));
    assert(io(SYS_RECV_UNTIL,fd,result,1,now)==SYSCALL_ETIMEDOUT);
    online=false; net_socket_worker_tick(now,false);
    assert(io(SYS_RECV_UNTIL,fd,result,1,now)==SYSCALL_EIO);
    assert(io(SYS_SEND_UNTIL,fd,input,1,now)==SYSCALL_EIO);
    online=true; net_socket_worker_tick(now,true);
    interrupt_after_send=true;
    assert(io(SYS_SEND,fd,input,32,0)==32 && signal_pending && !interrupt_after_send);
    signal_pending=false;
    for (size_t offset=32;offset<sizeof(input);) {
        size_t n=sizeof(input)-offset; if (n>16384) n=16384;
        int64_t count=io(SYS_WRITE,fd,input+offset,n,0);
        assert(count>0 && count<=4096); offset+=(size_t)count;
    }
    assert(!call(SYS_SHUTDOWN,fd,NET_SHUT_WR,0,0,0,0)); peer_return=true;
    size_t offset=0;
    for (;;) {
        int64_t count=io(SYS_READ,fd,result+offset,sizeof(result)-offset,0);
        assert(count>=0); if (!count) break; offset+=(size_t)count;
        if (offset==sizeof(result)) { assert(io(SYS_RECV,fd,result,1,0)==0); break; }
    }
    assert(offset==sizeof(input) && peer_count==sizeof(input));
    assert(!memcmp(input,result,sizeof(input)) && !memcmp(input,peer_bytes,sizeof(input)));
    assert(io(SYS_WRITE,fd,input,1,0)==SYSCALL_EPIPE);
    assert(last_signal_actor==1 && last_signal_target==1 && last_signal_number==SIGPIPE);
    closefd(fd); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS owner client: binary 64 KiB each way, <=4096-byte SEND, READ/WRITE, half-close, ordered EOF and EPIPE; exact reclamation");
    peer_active=false; peer_return=false;
    fd=stream(); assert(fd>=0); cancel_offer=true;
    assert(connectfd(fd)==SYSCALL_EINTR && signal_pending && !cancel_offer);
    signal_pending=false;
    assert(net_tcp_connected(net_socket_index(process.fd_table[fd]))==SYSCALL_ENOTCONN);
    closefd(fd); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS CONNECT handshake offer cancels on owner before caller acknowledgement; no ambiguous restart");
    peer_active=false; peer_return=false;
    fd=stream(); assert(fd>=0);
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!net_udp_begin(NET_OWNER_TCP_CONNECT,process.fd_table[fd],&destination,0,false));
    handoff_token_t offered=process.handoff.token;
    for (unsigned i=0;i<256 && !net_udp_ready(&offered);++i) { tcp_service(); tick(); }
    assert(net_udp_ready(&offered) && !handoff_ready(&offered));
    net_host_unlock_hook=ack_after_snapshot;
    pending_step(offered,NET_OWNER_TCP_CONNECT);
    assert(snapshot_ack_checks==1 && !net_host_unlock_hook);
    handoff_acknowledge();
    assert(!handoff_owner_ready(NULL) && net_udp_owner_work());
    for (unsigned i=0;i<16;++i) tick();
    handoff_reply_t acknowledged;
    assert(!net_udp_collect(&acknowledged) && !acknowledged.status); net_udp_release();
    closefd(fd); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS AP acknowledgement between owner phase snapshot and finish cannot be overwritten");
    /* Forced competing readers and a pinned lease spanning seven seconds.
     * Protocol input appends; owner never consumes/reuses the pinned prefix. */
    peer_active=false; peer_return=false;
    fd=stream(); assert(fd>=0); assert(!connectfd(fd));
    file_t *file=process.fd_table[fd];
    for (unsigned i=0;i<4;++i) { tcp_service(); tick(); }
    assert(tcp_conn_queue(&peer_connection,"abcdefgh",8)==8);
    for (unsigned i=0;i<20;++i) { tcp_service(); tick(); }
    expire_offer=now+10;
    assert(io(SYS_RECV_UNTIL,fd,result,4,expire_offer)==SYSCALL_ETIMEDOUT);
    assert(!expire_offer);
    assert(!net_udp_begin(NET_OWNER_TCP_RECV,file,NULL,4,false));
    handoff_registration_t first=process.handoff;
    for (unsigned i=0;i<8;++i) tick();
    const uint8_t *pinned; int64_t staged;
    now+=101; net_host_unlock_hook=pin_after_revoke;
    pending_step(first.token,NET_OWNER_TCP_RECV);
    assert(revoke_checks==1 && !net_host_unlock_hook);
    assert(net_udp_pin(&pinned,&staged,NULL) && staged==4 && !memcmp(pinned,"abcd",4));
    process.tid=2; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_TCP_RECV,file,NULL,4,false));
    handoff_registration_t second=process.handoff;
    for (unsigned i=0;i<8;++i) tick();
    assert(!net_udp_pin(&pinned,&staged,NULL));
    process.tid=1; process.handoff=first;
    now+=701;
    for (unsigned i=0;i<8;++i) tick();
    assert(!memcmp(pinned,"abcd",4));
    net_udp_unpin(true); handoff_cancel(first.token);
    pending_step(first.token,NET_OWNER_TCP_RECV);
    net_udp_refresh_work(); handoff_acknowledge();
    assert(!handoff_owner_ready(NULL) && net_udp_owner_work());
    handoff_reply_t committed;
    assert(!net_udp_collect(&committed) && committed.status==4);
    net_udp_release();
    process.tid=2; process.handoff=second;
    for (unsigned i=0;i<16;++i) tick();
    assert(net_udp_pin(&pinned,&staged,NULL) && staged==4 && !memcmp(pinned,"efgh",4));
    net_udp_unpin(false);
    for (unsigned i=0;i<16;++i) tick();
    assert(!net_udp_collect(&committed) && committed.status==SYSCALL_EINTR);
    net_udp_release();
    process.tid=1; process.handoff=(handoff_registration_t){0};
    assert(io(SYS_RECV,fd,result,4,0)==4 && !memcmp(result,"efgh",4));
    assert(tcp_conn_queue(&peer_connection,"ijkl",4)==4);
    for (unsigned i=0;i<20;++i) { tcp_service(); tick(); }
    assert(!net_udp_begin(NET_OWNER_TCP_RECV,file,NULL,4,false));
    for (unsigned i=0;i<8;++i) tick();
    net_socket_worker_tick(now,false);
    net_host_unlock_hook=copy_after_snapshot;
    pending_step(process.handoff.token,NET_OWNER_TCP_RECV);
    assert(late_copy_checks==1 && !net_host_unlock_hook);
    pending_step(process.handoff.token,NET_OWNER_TCP_RECV);
    assert(!net_udp_collect(&committed) && committed.status==4); net_udp_release();
    net_socket_worker_tick(now,true);
    closefd(fd); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS receive pin survives lease, competing reader blocks, commit beats cancel, abort restores ordered prefix exactly once");
    puts("PASS copied receive after owner phase snapshot survives terminal link cleanup and releases its decision credit");
    peer_active=false; peer_return=false;
    fd=stream(); assert(fd>=0); blackhole=true;
    uint64_t deadline=now+20;
    assert(call(SYS_CONNECT_UNTIL,fd,(uintptr_t)&destination,16,deadline,0,0)==SYSCALL_ETIMEDOUT);
    assert(now>=deadline); closefd(fd); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS unanswered CONNECT original absolute deadline and retained cleanup");
    blackhole=false; peer_active=false;
    connect_admission_expiry();
    shared_payload_saturation();
    listener_tests(&cfg);
    listener_data_fin_tests(&cfg);
    repeated_receive_event_test(&cfg);
    listener_link_loss_test(&cfg);
    timer_hz=1000; now*=10; peer_active=false; blackhole=true;
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable();
    int boundary=stream(); assert(boundary>=0);
    assert(io(SYS_RECV,boundary,NULL,65537,0)==SYSCALL_EINVAL);
    assert(io(SYS_SEND,boundary,NULL,16385,0)==SYSCALL_EINVAL);
    assert(io(SYS_RECV_UNTIL,boundary,NULL,0,0)==0);
    assert(call(SYS_CONNECT_UNTIL,boundary,(uintptr_t)&destination,16,now+60001,0,0)==SYSCALL_EINVAL);
    unsigned no_start=tcp_packets;
    assert(call(SYS_CONNECT_UNTIL,boundary,(uintptr_t)&destination,16,now,0,0)==SYSCALL_ETIMEDOUT && tcp_packets==no_start);
    uint64_t boundary_deadline=now+100;
    assert(call(SYS_CONNECT_UNTIL,boundary,(uintptr_t)&destination,16,boundary_deadline,0,0)==SYSCALL_ETIMEDOUT && now>=boundary_deadline);
    closefd(boundary); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    boundary=stream(); assert(boundary>=0);
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!net_tcp_owner_begin(NET_OWNER_TCP_CONNECT,process.fd_table[boundary],&destination,16,false,true,now+1000,now));
    handoff_token_t reverse=process.handoff.token;
    for (unsigned i=0;i<16;++i) { tcp_service(); tick(); }
    now-=2; pending_step(reverse,NET_OWNER_TCP_CONNECT);
    handoff_reply_t clock_reply; assert(!net_udp_collect(&clock_reply) && clock_reply.status==SYSCALL_EIO); net_udp_release();
    closefd(boundary); for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !allocations);
    puts("PASS 1000 Hz horizon/expiry, zero length and public length bounds; backward clock after owner progress returns EIO and cleans the single CONNECT attempt");
    return 0;
}
