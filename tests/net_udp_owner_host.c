/* Actual UDP handoff/socket/syscall/IPv4 codecs. Pthreads replace scheduling;
 * no claim about guest IRQ frames or physical NIC ownership follows. */
#define main socket_baseline_main
#include "net_socket_host.c"
#undef main
#include <sched.h>
#include <time.h>
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static bool owner_stop, owner_park;
static bool owner_parked;
static bool park_after_arp;
static unsigned producers_entered, producers_done, successes, busy, receive_busy;
static file_t *shared[16];
static bool seen[48][40];
static unsigned cancelled_mode;
static void notify(const void *channel) {
    (void)channel; net_test_assert_unheld();
    pthread_mutex_lock(&event_lock); pthread_cond_broadcast(&event); pthread_mutex_unlock(&event_lock);
}
static void wait_adapter(const void *channel,bool (*ready)(void *),void *arg) {
    (void)channel; net_test_assert_unheld();
    if (cancelled_mode) {
        cancelled_mode=0; signal_pending=true; return;
    }
    pthread_mutex_lock(&event_lock);
    while (!ready(arg) && !signal_pending) {
        struct timespec end; clock_gettime(CLOCK_REALTIME,&end); ++end.tv_sec;
        int rc=pthread_cond_timedwait(&event,&event_lock,&end);
        assert(!rc || rc==110);
    }
    pthread_mutex_unlock(&event_lock);
}
static void *owner(void *unused) {
    (void)unused; process.tid=999;
    while (!__atomic_load_n(&owner_stop,__ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&park_after_arp,__ATOMIC_ACQUIRE) && __atomic_load_n(&arps,__ATOMIC_ACQUIRE))
            __atomic_store_n(&owner_park,true,__ATOMIC_RELEASE);
        if (__atomic_load_n(&owner_park,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&owner_parked,true,__ATOMIC_RELEASE); sched_yield(); continue;
        }
        __atomic_store_n(&owner_parked,false,__ATOMIC_RELEASE);
        tick();
        /* Configuration is applied only by this owner; queued wildcard binds
         * and unbound sends observe its current values. Echo uses that tuple. */
        if (apic_timer_get_bsp_ticks()%97==0) {
            uint32_t local=htonl(0x0a00020f+((apic_timer_get_bsp_ticks()/97)&1));
            pthread_mutex_lock(&config_lock);
            net_ipv4_set_config(local,24,htonl(0x0a000202)); net_socket_set_local(local);
            pthread_mutex_unlock(&config_lock);
        }
        sched_yield();
    }
    return NULL;
}
static void drain(void) {
    for (unsigned i=0;i<1000000;++i) {
        if (!handoff_stats().live) return;
        sched_yield();
    }
    assert(!"resource drainage exceeded bound");
}
static void until_tick(uint64_t target) {
    for (unsigned i=0;i<10000000;++i) {
        if (apic_timer_get_bsp_ticks()>=target) return;
        sched_yield();
    }
    assert(!"owner deadline progress exceeded bound");
}
static void park_owner(void) {
    __atomic_store_n(&owner_park,true,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&owner_parked,__ATOMIC_ACQUIRE)) sched_yield();
}
static void resume_owner(void) {
    __atomic_store_n(&owner_park,false,__ATOMIC_RELEASE); handoff_notify();
    while (__atomic_load_n(&owner_parked,__ATOMIC_ACQUIRE)) sched_yield();
}
static unsigned payload_saturation(void) {
    drain(); park_owner();
    handoff_registration_t registrations[9]={0};
    handoff_registration_t original=process.handoff;
    uint64_t actor=process.tid;
    net_sockaddr_in_t destination={.family=2,.port=htons(7777),.address=htonl(0x0a000202)};
    /* Freeze time and execute actual bounded owner turns explicitly. Each of
     * eight distinct actors stages a send without committing or pinning it. */
    for (unsigned i=0;i<8;++i) {
        process.tid=2000+i; process.handoff=(handoff_registration_t){0};
        assert(!net_udp_begin(NET_OWNER_UDP_SEND,shared[i],&destination,1,false));
        registrations[i]=process.handoff;
    }
    process.tid=999;
    for (unsigned i=0;i<8;++i) tick();
    for (unsigned i=0;i<8;++i) {
        process.tid=2000+i; process.handoff=registrations[i];
        const uint8_t *data; int64_t result;
        assert(net_udp_pin(&data,&result,NULL) && result==1);
        /* Retain the pin while testing the ninth admission. */
    }
    online=false; process.tid=999;
    net_socket_worker_tick(apic_timer_get_bsp_ticks(),false);
    for (unsigned i=0;i<2;++i) {
        process.tid=2040+i; process.handoff=(handoff_registration_t){0};
        assert(!net_udp_begin(i ? NET_OWNER_UDP_SEND : NET_OWNER_UDP_RECV,shared[13],
                             i ? &destination : NULL,i ? 1 : 0,i==0));
        handoff_registration_t unavailable=process.handoff;
        process.tid=999; tick();
        handoff_reply_t unavailable_reply;
        assert(!handoff_collect(unavailable.token,&unavailable_reply) && unavailable_reply.status==SYSCALL_EIO);
        process.tid=2040+i; process.handoff=unavailable; net_udp_release();
        process.tid=999; for (unsigned n=0;n<64;++n) tick();
    }
    online=true; process.tid=999; net_socket_worker_tick(apic_timer_get_bsp_ticks(),true);
    process.tid=2008; process.handoff=(handoff_registration_t){0};
    uint8_t queued=0x6d;
    /* Initial wildcard binds allocate 49152..49167 in order. */
    net_socket_input(htonl(0x0a000202),7777,49160,&queued,1);
    uint64_t identity=net_socket_identity(shared[8]);
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,shared[8],NULL,0,true));
    registrations[8]=process.handoff;
    process.tid=999; tick();
    handoff_reply_t reply;
    assert(!handoff_collect(registrations[8].token,&reply) && reply.status==SYSCALL_EAGAIN);
    assert(net_socket_identity_live(shared[8],identity));
    process.tid=2008; process.handoff=registrations[8]; net_udp_release();
    process.tid=2010; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_UDP_SEND,shared[10],&destination,1,false));
    handoff_registration_t oldest=process.handoff;
    process.tid=999; tick(); /* Oldest eligible payload waiter gets its ticket. */
    assert(!net_udp_owner_work()); /* Eight pins leave no grantable credit. */
    process.tid=2000; process.handoff=registrations[0]; net_udp_unpin(false);
    assert(net_udp_owner_work());
    process.tid=999;
    handoff_acknowledge(); assert(net_udp_owner_work());
    for (;;) {
        handoff_token_t turn; handoff_request_t request;
        assert(handoff_pending(&turn,&request)); net_udp_step(turn,request);
        if (turn.slot==registrations[0].token.slot && turn.generation==registrations[0].token.generation) break;
    }
    handoff_acknowledge(); assert(net_udp_owner_work()); /* Freed slab + waiter. */
    /* A slab is free, but a newer nonblocking claim must not bypass the
     * earlier waiter before its rotating pending turn. */
    process.tid=2011; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,shared[8],NULL,0,true));
    handoff_registration_t newcomer=process.handoff;
    process.tid=999; tick();
    assert(!handoff_collect(newcomer.token,&reply) && reply.status==SYSCALL_EAGAIN);
    process.tid=2011; process.handoff=newcomer; net_udp_release();
    process.tid=2010; process.handoff=oldest;
    const uint8_t *oldest_data; int64_t oldest_result;
    assert(net_udp_pin(&oldest_data,&oldest_result,NULL) && oldest_result==1);
    net_udp_unpin(false); net_udp_release();
    process.tid=2000; process.handoff=registrations[0]; net_udp_release();
    for (unsigned i=1;i<8;++i) {
        process.tid=2000+i; process.handoff=registrations[i];
        net_udp_unpin(false); net_udp_release();
    }
    process.tid=999;
    for (unsigned i=0;i<64;++i) tick();
    assert(!handoff_stats().live);
    assert(!net_udp_owner_work());
    for (unsigned i=0;i<16;++i) assert(shared[i]->ref_count==1);
    /* Force driver acceptance before cancellation but before typed completion. */
    process.tid=2020; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_UDP_SEND,shared[11],&destination,1,false));
    handoff_registration_t accepted=process.handoff;
    process.tid=999; tick();
    process.tid=2020; process.handoff=accepted;
    const uint8_t *accepted_data; int64_t accepted_result;
    assert(net_udp_pin(&accepted_data,&accepted_result,NULL));
    *(uint8_t *)accepted_data=0x7d; net_udp_unpin(true);
    assert(net_udp_owner_work());
    handoff_acknowledge(); assert(net_udp_owner_work());
    process.tid=999; tick(); /* Owner publishes the socket's copied send. */
    unsigned accepted_before=sends;
    echo=false; /* This gate proves driver acceptance, not remote delivery. */
    net_socket_worker_tick(apic_timer_get_bsp_ticks(),true);
    net_ipv4_tick(apic_timer_get_bsp_ticks());
    net_socket_worker_tick(apic_timer_get_bsp_ticks(),true);
    echo=true;
    assert(sends==accepted_before+1 && handoff_collect(accepted.token,&reply)==HANDOFF_WAIT);
    assert(frame[42]==0x7d);
    handoff_cancel(accepted.token); tick();
    assert(!handoff_collect(accepted.token,&reply) && reply.status==1);
    process.tid=2020; process.handoff=accepted; net_udp_release();
    process.tid=999; for (unsigned i=0;i<64;++i) tick();
    uint64_t clock_before=apic_timer_get_bsp_ticks();
    process.tid=2030; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,shared[12],NULL,0,false));
    handoff_registration_t clock_call=process.handoff;
    process.tid=999; tick();
    __atomic_store_n(&now,clock_before+100,__ATOMIC_RELEASE); tick();
    __atomic_store_n(&now,clock_before+20,__ATOMIC_RELEASE); tick();
    assert(!handoff_collect(clock_call.token,&reply) && reply.status==SYSCALL_EIO);
    process.tid=2030; process.handoff=clock_call; net_udp_release();
    process.tid=999; __atomic_store_n(&now,clock_before+102,__ATOMIC_RELEASE);
    for (unsigned i=0;i<64;++i) tick();
    process.tid=actor; process.handoff=original;
    resume_owner();
    uint8_t restored=0;
    assert(call(SYS_RECVFROM,8,(uintptr_t)&restored,1,NET_MSG_DONTWAIT,0,0)==1 && restored==queued);
    puts("PASS eight pinned payload slabs, ninth nonblocking EAGAIN, FIFO grant before new claims, exact reference and credit recovery");
    puts("PASS accepted SEND wins cancellation before typed completion, exactly one driver submission");
    puts("PASS backward owner clock after progress, still above admission, returns EIO and recovers");
    puts("PASS link unavailable outranks slab saturation for nonblocking RECEIVE and SEND");
    puts("PASS copy decisions and released payload credits remain level-runnable after generic hint clearing; idle after drain");
    return 16;
}
static unsigned deterministic(void) {
    net_sockaddr_in_t dest={.family=2,.port=htons(7777),.address=htonl(0x0a000202)};
    uint8_t bytes[]={0xde,0xad,0,0xff};
    unsigned operations=0;
    assert(call(SYS_SENDTO,0,(uintptr_t)bytes,4,0,(uintptr_t)&dest,16)==4); ++operations;
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,process.fd_table[0],NULL,0,false)); ++operations;
    handoff_token_t t=process.handoff.token;
    sched_wait_until(handoff_channel(t),net_udp_ready,&t);
    const uint8_t *data; int64_t result; net_sockaddr_in_t source;
    while (!net_udp_pin(&data,&result,&source)) sched_wait_until(handoff_channel(t),net_udp_ready,&t);
    assert(result==4 && !memcmp(data,bytes,4));
    /* Cancellation + lease expiry cannot recycle pinned data. The owner runs
     * well past the lease, without retaining the caller's stack or CR3. */
    handoff_cancel(t); until_tick(apic_timer_get_bsp_ticks()+850);
    handoff_reply_t reply;
    assert(handoff_collect(t,&reply)==HANDOFF_WAIT && !memcmp(data,bytes,4));
    net_udp_unpin(false);
    sched_wait_until(handoff_channel(t),handoff_ready,&t);
    assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EINTR);
    net_udp_release(); drain();
    /* Abort preserves the entire datagram and its queue position. */
    uint8_t returned[4];
    assert(call(SYS_RECVFROM,0,(uintptr_t)returned,4,0,0,0)==4 && !memcmp(returned,bytes,4)); ++operations;
    assert(call(SYS_SENDTO,0,(uintptr_t)bytes,4,0,(uintptr_t)&dest,16)==4); ++operations;
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,process.fd_table[0],NULL,0,false)); ++operations;
    t=process.handoff.token;
    sched_wait_until(handoff_channel(t),net_udp_ready,&t);
    /* A stopped continuation without a copy pin cannot monopolize a slab.
     * Finite UDP receive retains its original five-second deadline. */
    until_tick(apic_timer_get_bsp_ticks()+600);
    sched_wait_until(handoff_channel(t),handoff_ready,&t);
    assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EAGAIN);
    net_udp_release(); drain();
    assert(call(SYS_RECVFROM,0,(uintptr_t)returned,4,0,0,0)==4 && !memcmp(returned,bytes,4)); ++operations;
    /* Commit followed by cancellation must consume exactly once and publish
     * the positive result. Park at the copy decision to force that ordering. */
    assert(call(SYS_SENDTO,0,(uintptr_t)bytes,4,0,(uintptr_t)&dest,16)==4); ++operations;
    assert(!net_udp_begin(NET_OWNER_UDP_RECV,process.fd_table[0],NULL,0,false)); ++operations;
    t=process.handoff.token;
    do { sched_wait_until(handoff_channel(t),net_udp_ready,&t); }
    while (!net_udp_pin(&data,&result,&source));
    park_owner(); net_udp_unpin(true); handoff_cancel(t); resume_owner();
    sched_wait_until(handoff_channel(t),handoff_ready,&t);
    assert(!net_udp_collect(&reply) && reply.status==4);
    net_udp_release(); drain();
    assert(call(SYS_RECVFROM,0,(uintptr_t)returned,4,NET_MSG_DONTWAIT,0,0)==SYSCALL_EAGAIN); ++operations;
    drain();
    assert(!net_udp_owner_work());

    /* Resolve a claimed cold-ARP send cancellation, then restore the route:
     * no abandoned datagram may transmit when ARP becomes available. */
    park_owner(); cached=false; unsigned sent_before=sends;
    __atomic_store_n(&arps,0,__ATOMIC_RELEASE);
    assert(!net_udp_begin(NET_OWNER_UDP_SEND,process.fd_table[0],&dest,4,false)); ++operations;
    resume_owner(); t=process.handoff.token;
    do { sched_wait_until(handoff_channel(t),net_udp_ready,&t); }
    while (!net_udp_pin(&data,&result,&source));
    park_owner();
    __atomic_store_n(&park_after_arp,true,__ATOMIC_RELEASE);
    memcpy((void *)data,bytes,4); net_udp_unpin(true);
    __atomic_store_n(&owner_park,false,__ATOMIC_RELEASE); handoff_notify();
    /* Pinning is done; rendezvous at the actual IPv4 ARP attempt. */
    while (!__atomic_load_n(&arps,__ATOMIC_ACQUIRE)) sched_yield();
    while (!__atomic_load_n(&owner_parked,__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&park_after_arp,false,__ATOMIC_RELEASE);
    handoff_cancel(t); resume_owner();
    sched_wait_until(handoff_channel(t),handoff_ready,&t);
    assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EINTR);
    net_udp_release(); drain(); park_owner();
    assert(sends==sent_before); cached=true; resume_owner();
    until_tick(apic_timer_get_bsp_ticks()+20); park_owner();
    assert(sends==sent_before); resume_owner();
    puts("PASS cancellation while copy pinned, lease revocation, finite original deadline and ordered datagram restoration");
    puts("PASS committed receive wins cancellation; claimed cold-ARP send cancels with zero stale transmission");
    return operations;
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    process.tid=100+id; process.cpu_affinity=(int)(id%8);
    assert(vfs_file_get(shared[id%16])); process.fd_table[3]=shared[id%16];
    uint8_t bytes[4], reply_bytes[4];
    net_sockaddr_in_t dest={.family=2,.port=htons(7777),.address=htonl(0x0a000202)};
    __atomic_add_fetch(&producers_entered,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<40;++i) {
        bytes[0]=(uint8_t)id; bytes[1]=(uint8_t)i; bytes[2]=0xa5; bytes[3]=0x5a;
        int64_t rc;
        do {
            rc=call(SYS_SENDTO,3,(uintptr_t)bytes,4,0,(uintptr_t)&dest,16);
            if (rc==SYSCALL_EAGAIN) __atomic_add_fetch(&busy,1,__ATOMIC_RELAXED);
        } while (rc==SYSCALL_EAGAIN);
        assert(rc==4);
        do {
            rc=call(SYS_RECVFROM,3,(uintptr_t)reply_bytes,4,0,0,0);
            if (rc==SYSCALL_EAGAIN) __atomic_add_fetch(&receive_busy,1,__ATOMIC_RELAXED);
        } while (rc==SYSCALL_EAGAIN);
        assert(rc==4 && reply_bytes[0]<48 && reply_bytes[1]<40 && reply_bytes[2]==0xa5 && reply_bytes[3]==0x5a);
        pthread_mutex_lock(&event_lock);
        assert(!seen[reply_bytes[0]][reply_bytes[1]]); seen[reply_bytes[0]][reply_bytes[1]]=true;
        pthread_mutex_unlock(&event_lock);
        __atomic_add_fetch(&successes,1,__ATOMIC_RELEASE);
    }
    vfs_close(process.fd_table[3]); process.fd_table[3]=NULL;
    __atomic_add_fetch(&producers_done,1,__ATOMIC_RELEASE); return NULL;
}
int main(void) {
    process.tid=1;
    dev=(net_dev_t){.mtu=1500,.send_packet=transmit};
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    net_ipv4_init(&dev,&cfg); net_socket_init(&dev,&cfg); net_socket_enable(); handoff_start();
    net_host_wait_override=wait_adapter; net_host_wake_override=notify;
    net_host_owner_tid=999;
    pthread_t worker; assert(!pthread_create(&worker,NULL,owner,NULL));
    for (unsigned i=0;i<16;++i) {
        int fd=create(); assert(fd>=0); shared[i]=process.fd_table[fd];
        net_sockaddr_in_t address={.family=2};
        assert(!call(SYS_BIND,fd,(uintptr_t)&address,16,0,0,0));
    }
    drain();
    unsigned extra=deterministic(); drain();
    extra+=payload_saturation(); drain();
    park_owner();
    pthread_t threads[48];
    for (unsigned i=0;i<48;++i) assert(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)i));
    for (unsigned i=0;i<1000000;++i) {
        handoff_stats_t ready=handoff_stats();
        if (ready.live==48 && ready.queued==24) break;
        sched_yield();
    }
    handoff_stats_t saturation=handoff_stats();
    assert(saturation.live==48 && saturation.queued==24 && saturation.data==24);
    __atomic_store_n(&owner_park,false,__ATOMIC_RELEASE); handoff_notify();
    for (unsigned i=0;i<48;++i) assert(!pthread_join(threads[i],NULL));
    assert(successes==1920 && producers_entered==48 && producers_done==48);
    for (unsigned i=0;i<48;++i) for (unsigned j=0;j<40;++j) assert(seen[i][j]);
    drain();
    /* Unclaimed cancellation and retained constructor failure are reclaimed
     * only after typed cleanup. A new operation from that actor must wait. */
    cancelled_mode=1;
    assert(call(SYS_RECVFROM,0,(uintptr_t)output,1,0,0,0)==SYSCALL_EINTR);
    signal_pending=false; drain();
    assert(handoff_stats().reclaimed==32+extra+3840+busy+receive_busy+1);
    for (unsigned i=0;i<16;++i) assert(__atomic_load_n(&shared[i]->ref_count,__ATOMIC_ACQUIRE)==1);
    for (unsigned i=0;i<32;++i) if (process.fd_table[i]) {
        vfs_close(process.fd_table[i]); process.fd_table[i]=NULL;
    }
    drain();
    __atomic_store_n(&owner_stop,true,__ATOMIC_RELEASE); assert(!pthread_join(worker,NULL));
    net_host_owner_tid=0;
    /* Final file callbacks may have published detach just after the last sweep. */
    for (unsigned i=0;i<16;++i) tick();
    assert(!allocations && !handoff_stats().live);
    printf("PASS 48 concurrent actors: 24 data credits saturated, 1920 exact echoed datagrams, %u protocol-contention EAGAIN retries, %llu reclaimed records; owner configuration and retained shared refs\n",busy,(unsigned long long)handoff_stats().reclaimed);
    return 0;
}
