/* Actual owner/syscall/transport code; deterministic scheduler/clock adapters.
 * These interleavings supplement, rather than claim, real AP execution. */
#define TCP_LISTENER_OWNER_FIXTURE
#include "net_tcp_owner_host.c"
#undef main

static handoff_registration_t accept_begin(file_t *listener,uint64_t actor) {
    process.tid=actor; process.handoff=(handoff_registration_t){0};
    assert(!net_udp_begin(NET_OWNER_TCP_ACCEPT,listener,NULL,0,false));
    return process.handoff;
}
static void actor(handoff_registration_t registration,uint64_t tid) {
    process.tid=tid; process.handoff=registration;
}
static void drain_listener(void) {
    process.tid=1; process.handoff=(handoff_registration_t){0};
    for (unsigned i=0;i<256;++i) { tcp_service(); tick(); }
    assert(!handoff_stats().live && !net_udp_owner_work());
}
static file_t *provisional(int *fd) {
    file_t *file=NULL; assert(!net_socket_prepare(&file));
    *fd=fd_alloc(&process,file); assert(*fd>=0);
    assert(!net_socket_identity(file)); return file;
}
static uint32_t *invalidated_capacity;
static void failed_accept_wait(const void *channel,bool (*ready)(void *),void *arg) {
    if (invalidated_capacity && ready==net_udp_ready) {
        online=false; net_socket_worker_tick(now,false); net_tcp_tick(now,false);
        for (unsigned i=0;i<16;++i) tick();
        *invalidated_capacity=15; invalidated_capacity=NULL;
        return;
    }
    owner_wait(channel,ready,arg);
}
static void listener_transactions(net_config_t *cfg) {
    timer_hz=100; now=1000; online=true; peer_active=false; listener_fixture=true;
    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    int queued=stream(); assert(queued>=0);
    net_sockaddr_in_t local={.family=NET_AF_INET,.port=htons(9000)};
    assert(!call(SYS_BIND,queued,(uintptr_t)&local,16,0,0,0)); drain_listener();
    assert(!net_udp_begin(NET_OWNER_TCP_LISTEN,process.fd_table[queued],NULL,1,false));
    online=false; for (unsigned i=0;i<16;++i) tick();
    handoff_reply_t offline; assert(!net_udp_collect(&offline) && offline.status==SYSCALL_EIO);
    net_udp_release(); assert(!net_tcp_listener(net_socket_index(process.fd_table[queued])));
    online=true; drain_listener(); assert(!call(SYS_LISTEN,queued,1,0,0,0,0));
    closefd(queued); drain_listener(); assert(!allocations);
    puts("PASS queued LISTEN rechecks link availability on owner, returns EIO without mutation and succeeds after recovery");
    int listener=listenfd(); file_t *file=process.fd_table[listener];
    drain_listener(); unsigned base=allocations;
    memset(synacks,0,sizeof(synacks)); passive_open(0,true); passive_open(1,true);
    uint64_t before=handoff_stats().reclaimed;
    handoff_registration_t first=accept_begin(file,4001), second=accept_begin(file,4002);
    for (unsigned i=0;i<16;++i) tick();
    net_sockaddr_in_t a,b;
    actor(first,4001); assert(net_tcp_owner_accept_pin(&a));
    actor(second,4002); assert(net_tcp_owner_accept_pin(&b));
    assert(a.port!=b.port);
    /* Both pins survive a lease much longer than the original one-second
     * offer. Protocol data and FIN keep progressing during the pinned choice. */
    now+=700; incoming(0,TCP_ACK|TCP_PSH|TCP_FIN,synacks[0].sequence+1,"abc",3);
    for (unsigned i=0;i<16;++i) { tcp_service(); tick(); }
    actor(second,4002); net_tcp_owner_accept_decide(NULL,SYSCALL_EMFILE);
    pending_step(second.token,NET_OWNER_TCP_ACCEPT);
    handoff_reply_t reply; assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EMFILE);
    net_udp_release();
    actor(first,4001); int fd; file_t *accepted=provisional(&fd);
    net_tcp_owner_accept_decide(accepted,0); handoff_cancel(first.token);
    pending_step(first.token,NET_OWNER_TCP_ACCEPT);
    assert(!net_udp_collect(&reply) && !reply.status); net_udp_release();
    assert(net_socket_stream(accepted) && net_socket_identity(accepted));
    drain_listener(); assert(handoff_stats().reclaimed-before==2 && file->ref_count==1);
    /* A competing acceptor receives the restored child exactly once. */
    int other=(int)acceptfd(listener,&b,&(uint32_t){16}); assert(other>=0);
    assert(a.port!=b.port);
    net_tcp_child_t child; net_sockaddr_in_t peer;
    assert(net_tcp_accept_peek(net_socket_index(file),&child,&peer)==SYSCALL_EAGAIN);
    closefd(listener); net_tcp_tick(now,true);
    assert(stream_receive_call(SYS_READ,fd,output,3)==3 && !memcmp(output,"abc",3));
    assert(!stream_receive_call(SYS_RECV,fd,output,1));
    closefd(fd); closefd(other); drain_listener(); assert(!allocations);
    puts("PASS two retained acceptors reserve distinct existing children; seven-second pins, abort restoration, commit beats cancellation, pre-adoption data/FIN, listener-close independence and exact reference/record reclamation");

    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    listener=listenfd(); file=process.fd_table[listener]; drain_listener();
    memset(synacks,0,sizeof(synacks)); passive_open(0,true);
    first=accept_begin(file,4011); for (unsigned i=0;i<16;++i) tick();
    second=accept_begin(file,4012); for (unsigned i=0;i<16;++i) tick();
    actor(first,4011); now+=101;
    pending_step(first.token,NET_OWNER_TCP_ACCEPT); /* release, don't reoffer */
    assert(!net_tcp_owner_accept_pin(&a));
    pending_step(second.token,NET_OWNER_TCP_ACCEPT);
    actor(second,4012); assert(net_tcp_owner_accept_pin(&b));
    assert(b.port==htons(50000));
    net_tcp_owner_accept_decide(NULL,SYSCALL_EFAULT);
    pending_step(second.token,NET_OWNER_TCP_ACCEPT);
    assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EFAULT); net_udp_release();
    actor(first,4011); net_udp_abandon(); net_udp_abandon();
    drain_listener(); assert(allocations==base && file->ref_count==1);
    int restored=(int)acceptfd(listener,NULL,NULL); assert(restored>=0);
    closefd(restored); closefd(listener); drain_listener(); assert(!allocations);
    puts("PASS unpinned lease yields child to competing acceptor without public timeout; stale pin rejected, abort and repeated death/reaper abandonment restore child and references");

    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    listener=listenfd(); file=process.fd_table[listener]; drain_listener();
    memset(synacks,0,sizeof(synacks)); passive_open(0,true);
    first=accept_begin(file,4021); for (unsigned i=0;i<16;++i) tick();
    assert(net_tcp_owner_accept_pin(&a)); accepted=provisional(&fd);
    /* Peer reset between provisional insertion and commit invalidates the
     * exact child generation. Failed adoption must never trap or consume a
     * subsequent child that reuses the block. */
    incoming(0,TCP_RST|TCP_ACK,synacks[0].sequence+1,NULL,0); net_tcp_tick(now,true);
    net_tcp_owner_accept_decide(accepted,0);
    pending_step(first.token,NET_OWNER_TCP_ACCEPT);
    assert(!net_udp_collect(&reply) && reply.status==SYSCALL_EBADF); net_udp_release();
    closefd(fd); drain_listener(); assert(allocations==base);
    passive_open(1,true); restored=(int)acceptfd(listener,&b,&(uint32_t){16});
    assert(restored>=0 && b.port==htons(50001));
    closefd(restored); closefd(listener); drain_listener(); assert(!allocations);
    listener_fixture=false;
    puts("PASS reset during provisional adoption returns rollback error, frees staged handle, rejects stale child identity and admits a fresh child");
    listener_fixture=true;
    net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
    listener=listenfd(); drain_listener();
    uint32_t capacity=16; invalidated_capacity=&capacity;
    net_host_wait_override=failed_accept_wait;
    assert(acceptfd(listener,&a,&capacity)==SYSCALL_EINVAL);
    assert(!invalidated_capacity);
    net_host_wait_override=owner_wait;
    online=true; closefd(listener); drain_listener(); assert(!allocations);
    puts("PASS caller output revalidation beats terminal owner/link failure before adoption; no fd insertion or child consumption on rejected outputs");
    for (unsigned boundary=0;boundary<4;++boundary) {
        listener_fixture=true;
        net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
        listener=listenfd(); file=process.fd_table[listener]; drain_listener();
        memset(synacks,0,sizeof(synacks)); passive_open(0,true);
        first=accept_begin(file,4100+boundary);
        if (boundary) for (unsigned i=0;i<16;++i) tick();
        if (boundary>=2) {
            assert(net_tcp_owner_accept_pin(&a)); accepted=provisional(&fd);
            net_tcp_owner_accept_decide(accepted,boundary==3 ? SYSCALL_EFAULT : 0);
            /* Death closes the fd before the owner resolves the decision. */
            process.fd_table[fd]=NULL; vfs_close(accepted);
        }
        process.fd_table[listener]=NULL; vfs_close(file);
        net_udp_abandon(); net_udp_abandon(); /* exit and detached reaper */
        drain_listener(); assert(!allocations);
    }
    for (unsigned i=0;i<MAX_PROCESS_FDS;++i) assert(!process.fd_table[i]);
    listener_fixture=false;
    puts("PASS last descriptor close and idempotent death/reaper cleanup before claim, after reserve, after provisional commit and after abort publication; checked rollback references survive caller teardown");
}
static void listener_link_loss_transactions(net_config_t *cfg) {
    /* Lose/recover carrier before claim, after offer, while pinned and after
     * commit publication. Retained child/fd lifetimes must survive every cut;
     * no accepted decision may revive the failed listener after replug. */
    for (unsigned cut=0;cut<4;++cut) {
        online=true; peer_active=false; listener_fixture=true;
        net_socket_init(&dev,cfg); net_ipv4_init(&dev,cfg); net_socket_enable();
        int listener=listenfd(); file_t *file=process.fd_table[listener];
        drain_listener(); unsigned base=allocations;
        memset(synacks,0,sizeof(synacks)); passive_open(0,true);
        handoff_registration_t registration=accept_begin(file,4200+cut);
        if (cut) for (unsigned i=0;i<16;++i) tick();
        net_sockaddr_in_t peer; file_t *accepted=NULL; int fd=-1;
        if (cut>=2) {
            assert(net_tcp_owner_accept_pin(&peer)); accepted=provisional(&fd);
            if (cut==3) net_tcp_owner_accept_decide(accepted,0);
        }
        online=false; net_socket_worker_tick(now,false); net_tcp_tick(now,false);
        assert(net_tcp_poll(net_socket_index(file),0)==POLLERR);
        online=true; net_socket_worker_tick(now,true); net_tcp_tick(now,true);
        handoff_reply_t reply;
        if (cut==1) {
            /* An outstanding copied offer may remain interested until caller
             * pin/decision or lease expiry. Resuming after replug must still
             * roll its provisional child back, never adopt it successfully. */
            assert(net_tcp_owner_accept_pin(&peer)); accepted=provisional(&fd);
            net_tcp_owner_accept_decide(accepted,0);
        }
        if (cut==2) {
            pending_step(registration.token,NET_OWNER_TCP_ACCEPT);
            assert(net_udp_collect(&reply)==HANDOFF_WAIT); /* Pin cannot be revoked. */
            net_tcp_owner_accept_decide(accepted,0);
        }
        for (unsigned i=0;i<16;++i) tick();
        int rc=net_udp_collect(&reply);
        if (rc || reply.status!=SYSCALL_EIO)
            fprintf(stderr,"link-loss cut=%u collect=%d status=%lld\n",cut,rc,(long long)reply.status);
        assert(!rc && reply.status==SYSCALL_EIO);
        net_udp_release();
        if (fd>=0) closefd(fd); /* Same provisional rollback as accept_socket. */
        drain_listener(); assert(allocations==base && file->ref_count==1);
        assert(acceptfd(listener,NULL,NULL)==SYSCALL_EIO);
        closefd(listener); drain_listener(); assert(!allocations);
        /* The old queue is detached only by normal close; the port and pool
         * must then admit a fresh listener and a distinct delivered child. */
        listener=listenfd(); memset(synacks,0,sizeof(synacks)); passive_open(1,true);
        fd=(int)acceptfd(listener,&peer,&(uint32_t){16});
        assert(fd>=0 && peer.port==htons(50001));
        closefd(fd); closefd(listener); drain_listener(); assert(!allocations);
    }
    listener_fixture=false;
    puts("PASS persistent listener EIO across link loss at queued/offer/pin/published-commit boundaries, pinned decision acknowledgement, provisional rollback, exact cleanup and fresh port/child reuse");
}
int main(void) {
    assert(!client_owner_main());
    net_config_t cfg={.local_ip=htonl(0x0a00020f),.gateway=htonl(0x0a000202),.prefix=24};
    listener_transactions(&cfg);
    listener_link_loss_transactions(&cfg);
    return 0;
}
