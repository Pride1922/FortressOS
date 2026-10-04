#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tcp_tcb.h"
#include "net.h"

static tcp_conn_t a,b,saved;
static tcp_pool_t pool;
static uint8_t scratch[1460], wire[1480], readbuf[2048], offered[1024];
static uint64_t now;
static tcp_tuple_t ta={0},tb={0};
static void setup(bool simultaneous, uint32_t ai, uint32_t bi) {
    now=0;
    ta=(tcp_tuple_t){htonl(0xc0a800a8),htonl(0xc0a800de),40000,7777};
    tb=(tcp_tuple_t){ta.remote_ip,ta.local_ip,ta.remote_port,ta.local_port};
    assert(!tcp_conn_init(&a,ta,1,ai,1500,true,now));
    assert(!tcp_conn_init(&b,tb,2,bi,1500,simultaneous,now));
}
static void deliver(tcp_conn_t *to, tcp_tuple_t from, const uint8_t *p, size_t n) {
    tcp_header_t h; const uint8_t *data; size_t len;
    assert(!tcp_decode(p,n,from.local_ip,from.remote_ip,&h,&data,&len));
    tcp_conn_input(to,&h,data,len,now);
}
static bool send_one(tcp_conn_t *from, tcp_conn_t *to, bool drop) {
    if (from->now_ms!=now) tcp_conn_tick(from,now);
    tcp_action_t action;
    int result=tcp_conn_prepare(from,&action,scratch,sizeof(scratch));
    if (result==TCP_WOULD_BLOCK) return false;
    assert(!result);
    assert(!tcp_encode(wire,sizeof(wire),from->tuple.local_ip,from->tuple.remote_ip,
                       &action.header,scratch,action.data_len));
    size_t n=(action.header.has_mss ? 24 : 20)+action.data_len;
    assert(tcp_conn_commit(from,&action,true));
    if (!drop) deliver(to,from->tuple,wire,n);
    return true;
}
static void settle(void) {
    for (unsigned i=0; i<100; ++i) {
        bool x=send_one(&a,&b,false), y=send_one(&b,&a,false);
        if (!x && !y) return;
    }
    assert(!"unbounded control traffic");
}
static void handshake(void) {
    assert(send_one(&a,&b,false)); ++now;
    assert(send_one(&b,&a,false)); ++now;
    settle(); assert(a.state==TCP_ESTABLISHED && b.state==TCP_ESTABLISHED);
    assert(a.snd_una==a.iss+1 && b.snd_una==b.iss+1);
    assert(a.mss==1460 && b.mss==1460 && a.have_rtt && b.have_rtt);
}
static void input(tcp_conn_t *c, uint32_t seq, uint32_t ack, uint16_t wnd,
                  uint8_t flags, const void *data, size_t len) {
    tcp_header_t h={.source=c->tuple.remote_port,.destination=c->tuple.local_port,
        .sequence=seq,.acknowledgment=ack,.window=wnd,.flags=flags};
    assert(!tcp_encode(wire,sizeof(wire),c->tuple.remote_ip,c->tuple.local_ip,&h,data,len));
    deliver(c,(tcp_tuple_t){c->tuple.remote_ip,c->tuple.local_ip,c->tuple.remote_port,c->tuple.local_port},wire,len+20);
}
static void transactions(void) {
    setup(false,100,200);
    tcp_action_t action;
    saved=a;
    assert(!tcp_conn_prepare(&a,&action,scratch,sizeof(scratch)));
    assert(a.snd_nxt==saved.snd_nxt && !a.retx_count && !a.rto_deadline);
    assert(tcp_conn_commit(&a,&action,false) && !a.syn_sent && !a.retries);
    assert(!tcp_conn_commit(&a,&action,true));
    assert(!tcp_conn_prepare(&a,&action,scratch,sizeof(scratch)));
    tcp_conn_tick(&a,1); assert(!tcp_conn_commit(&a,&action,true));
    now=1; handshake();
    assert(tcp_conn_queue(&a,"hello",5)==5);
    saved=a;
    assert(tcp_conn_prepare(&a,&action,scratch,4)==TCP_NO_SPACE);
    assert(!memcmp(&a,&saved,sizeof(a)));
    assert(!tcp_conn_prepare(&a,&action,scratch,sizeof(scratch)));
    tcp_action_t forged=action; forged.header.sequence++;
    assert(!tcp_conn_commit(&a,&forged,true) && a.action_pending && a.tx_count==5);
    assert(tcp_conn_commit(&a,&action,false));
    assert(a.snd_nxt==saved.snd_nxt && a.tx_count==5 && !a.retx_count);
    settle(); assert(b.rx_count==5 && a.tx_count==0);
    assert(tcp_conn_peek(&b,readbuf,2)==2 && !memcmp(readbuf,"he",2) && b.rx_count==5);
    assert(tcp_conn_consume(&b,6)==TCP_INVALID && b.rx_count==5);
    assert(!tcp_conn_consume(&b,2)); assert(tcp_conn_peek(&b,readbuf,10)==3 && !memcmp(readbuf,"llo",3));
    assert(!tcp_conn_consume(&b,3)); settle();
    assert(tcp_conn_peek(&b,readbuf,10)==TCP_WOULD_BLOCK);
    assert(tcp_conn_queue(&a,NULL,1)==TCP_INVALID);
    assert(tcp_conn_peek(&a,NULL,1)==TCP_INVALID);
    assert(!tcp_conn_queue(&a,NULL,0) && !tcp_conn_peek(&a,NULL,0));
    puts("[PASS] prepare/commit, failed local submission, stale action, retained TX and nonconsuming RX");
}
static void partial_ack(void) {
    setup(false,0xfffffff0U,0xfffffff8U); handshake();
    a.cwnd=8192;
    memset(offered,0x5a,sizeof(offered)); assert(tcp_conn_queue(&a,offered,1000)==1000);
    assert(send_one(&a,&b,true)); uint32_t seq=a.snd_una;
    input(&a,a.rcv_nxt,seq+333,8192,TCP_ACK,NULL,0);
    assert(a.tx_count==667 && a.tx_sequence==seq+333 && a.retx_count==1);
    assert(a.retx[0].sequence==seq+333 && a.retx[0].length==667);
    uint32_t nxt=a.snd_nxt; uint16_t count=a.tx_count;
    input(&a,a.rcv_nxt,nxt+1,8192,TCP_ACK,NULL,0);
    assert(a.snd_nxt==nxt && a.tx_count==count && a.ack_pending);
    a.ack_pending=false; now=a.rto_deadline; tcp_conn_tick(&a,now);
    tcp_action_t act; assert(!tcp_conn_prepare(&a,&act,scratch,sizeof(scratch)));
    assert(act.data_len==667 && act.header.sequence==seq+333);
    for (unsigned i=0; i<667; ++i) assert(scratch[i]==0x5a);
    assert(tcp_conn_commit(&a,&act,true));
    input(&a,a.rcv_nxt,nxt,8192,TCP_ACK,NULL,0);
    assert(!a.tx_count && !a.retx_count && a.snd_una==nxt);
    assert(tcp_seq_before(0xfffffff0U,20) && !tcp_seq_before(20,0xfffffff0U));
    assert(!tcp_seq_before(0,0x80000000U) && !tcp_seq_before(0x80000000U,0));
    puts("[PASS] partial ACK range trimming, exact retransmit bytes, sequence wrap and invalid ACK");
}
static void receive_cases(void) {
    setup(false,100,200); handshake(); uint32_t start=b.rcv_nxt;
    input(&b,start+3,b.snd_nxt,8192,TCP_ACK,"def",3);
    assert(!b.rx_count && b.rcv_nxt==start);
    input(&b,start+2,b.snd_nxt,8192,TCP_ACK,"cXXX",4);
    input(&b,start,b.snd_nxt,8192,TCP_ACK,"ab",2);
    assert(tcp_conn_peek(&b,readbuf,10)==6 && !memcmp(readbuf,"abcdef",6));
    input(&b,start,b.snd_nxt,8192,TCP_ACK,"abcdef",6);
    assert(b.rx_count==6); assert(!tcp_conn_consume(&b,6));
    input(&b,start,b.snd_nxt,8192,TCP_ACK,"abcdef",6); assert(!b.rx_count);
    start=b.rcv_nxt;
    input(&b,start+3,b.snd_nxt,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(!b.eof && b.remote_fin_pending);
    input(&b,start,b.snd_nxt,8192,TCP_ACK,"xyz",3);
    assert(b.eof && b.state==TCP_CLOSE_WAIT && b.rcv_nxt==start+4);
    assert(tcp_conn_peek(&b,readbuf,10)==3 && !memcmp(readbuf,"xyz",3));
    assert(!tcp_conn_consume(&b,3) && !tcp_conn_peek(&b,readbuf,1));
    uint32_t expected=b.rcv_nxt;
    input(&b,start+3,b.snd_nxt,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(b.rcv_nxt==expected && b.ack_pending);
    input(&b,expected,b.snd_nxt,8192,TCP_ACK,"post-fin",8); assert(!b.rx_count);
    puts("[PASS] OOO gaps, duplicate/conflicting overlaps, FIN ordering, EOF and duplicate FIN");
}
static void timers(void) {
    setup(false,100,200);
    assert(send_one(&a,&b,true)); assert(a.rto_deadline==1000);
    tcp_conn_tick(&a,999); assert(!a.retransmit_pending);
    now=1000; tcp_conn_tick(&a,now);
    uint64_t due=a.rto_deadline, revision=a.revision;
    for (unsigned i=0; i<3; ++i) tcp_conn_tick(&a,now);
    assert(a.rto_deadline==due && a.rto_ms==1000 && !a.retries);
    assert(a.retransmit_pending && a.retransmit_timeout && a.revision==revision+3);
    tcp_action_t act; assert(!tcp_conn_prepare(&a,&act,scratch,sizeof(scratch)) && act.timeout);
    assert(tcp_conn_commit(&a,&act,false) && a.retries==0 && a.rto_ms==1000);
    assert(send_one(&a,&b,false)); assert(a.retries==1 && a.rto_ms==2000);
    due=a.rto_deadline;
    for (unsigned i=0; i<3; ++i) tcp_conn_tick(&a,now);
    assert(a.rto_deadline==due && a.rto_ms==2000 && a.retries==1);
    assert(!a.retransmit_pending && !a.retransmit_timeout);
    puts("[PASS] repeated same-timestamp ticks preserve timer deadlines/retries/backoff before and after retransmission commit");
    ++now; assert(send_one(&b,&a,false)); settle();
    assert(a.state==TCP_ESTABLISHED && !a.have_rtt && a.rto_ms>=3000);
    uint64_t clock=a.now_ms; tcp_conn_tick(&a,clock-1); assert(a.now_ms==clock);
    setup(false,1,2); handshake();
    a.rto_ms=1000; a.have_rtt=false; a.cwnd=8192;
    assert(tcp_conn_queue(&a,"rtt",3)==3); assert(send_one(&a,&b,true));
    now+=100; input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_ACK,NULL,0);
    assert(a.have_rtt && a.srtt_ms==100 && a.rttvar_ms==50 && a.rto_ms==1000);
    assert(tcp_conn_queue(&a,"again",5)==5); assert(send_one(&a,&b,true));
    now+=300; input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_ACK,NULL,0);
    assert(a.srtt_ms==125 && a.rttvar_ms==87);
    setup(false,1,2); handshake();
    a.have_rtt=false; assert(tcp_conn_queue(&a,"lost",4)==4); assert(send_one(&a,&b,true));
    now=a.rto_deadline; tcp_conn_tick(&a,now); assert(send_one(&a,&b,true));
    assert(a.cwnd==a.mss && a.rto_ms==2000 && a.retries==1);
    now+=100; input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_ACK,NULL,0);
    assert(!a.have_rtt && a.rto_ms==2000); /* Karn excludes ambiguous ACK. */
    assert(tcp_conn_queue(&a,"retry-budget",12)==12); assert(send_one(&a,&b,true));
    for (unsigned i=0; i<8; ++i) {
        now=a.rto_deadline; tcp_conn_tick(&a,now); assert(send_one(&a,&b,true));
        assert(a.retries==i+1 && a.rto_ms<=60000);
    }
    now=a.rto_deadline; tcp_conn_tick(&a,now); assert(a.state==TCP_CLOSED && a.error==TCP_TIMEOUT);
    assert(a.reset_pending); assert(send_one(&a,&b,true)); assert(!a.reset_pending);
    puts("[PASS] clock bounds, RTO/backoff/submission, handshake Karn, RTT estimator and retry exhaustion");
}
static void reset_and_close(void) {
    setup(false,100,200); handshake();
    input(&a,a.rcv_nxt+1,a.snd_nxt,8192,TCP_RST,NULL,0);
    assert(a.state==TCP_ESTABLISHED && a.ack_pending); settle();
    input(&a,a.rcv_nxt+TCP_RXBUF_MAX,a.snd_nxt,8192,TCP_RST,NULL,0); assert(a.state==TCP_ESTABLISHED);
    input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_RST,NULL,0);
    assert(a.state==TCP_CLOSED && a.error==TCP_RESET && !a.reset_pending);
    setup(false,100,200); handshake();
    assert(tcp_conn_queue(&a,"before-fin",10)==10); assert(!tcp_conn_shutdown(&a));
    assert(tcp_conn_queue(&a,"no",2)==TCP_WRITE_CLOSED); settle();
    assert(a.state==TCP_FIN_WAIT_2 && b.state==TCP_CLOSE_WAIT && b.rx_count==10);
    assert(tcp_conn_queue(&b,"reply",5)==5); assert(!tcp_conn_shutdown(&b)); settle();
    assert(a.state==TCP_TIME_WAIT && b.state==TCP_CLOSED && a.rx_count==5);
    now+=10; input(&a,a.rcv_nxt-1,a.snd_nxt,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(a.timewait_deadline==now+TCP_TIMEWAIT_MS); settle();
    now=a.timewait_deadline-1; tcp_conn_tick(&a,now); assert(a.state==TCP_TIME_WAIT);
    tcp_conn_tick(&a,now+1); assert(a.state==TCP_CLOSED);
    setup(true,100,200);
    assert(send_one(&a,&b,true)); assert(send_one(&b,&a,true));
    input(&a,200,0,8192,TCP_SYN,NULL,0); input(&b,100,0,8192,TCP_SYN,NULL,0); settle();
    assert(a.state==TCP_ESTABLISHED && b.state==TCP_ESTABLISHED);
    assert(!tcp_conn_shutdown(&a) && !tcp_conn_shutdown(&b));
    assert(send_one(&a,&b,true)); assert(send_one(&b,&a,true));
    input(&a,b.fin_sequence,a.snd_una,8192,TCP_FIN|TCP_ACK,NULL,0);
    input(&b,a.fin_sequence,b.snd_una,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(a.state==TCP_CLOSING && b.state==TCP_CLOSING); settle();
    assert(a.state==TCP_TIME_WAIT && b.state==TCP_TIME_WAIT);
    setup(false,100,200); handshake(); assert(!tcp_conn_shutdown(&a)); settle();
    assert(a.state==TCP_FIN_WAIT_2 && !a.finwait2_deadline);
    tcp_conn_detach(&a,now); assert(a.finwait2_deadline==now+TCP_FINWAIT2_MS);
    now=a.finwait2_deadline; tcp_conn_tick(&a,now); assert(a.error==TCP_TIMEOUT);
    puts("[PASS] RST sequence validation, half-close, FIN ordering, TIME_WAIT, simultaneous open/close and orphan expiry");
}
static void windows_and_reno(void) {
    setup(false,100,200); handshake();
    a.cwnd=8192;
    for (unsigned i=0; i<8; ++i) assert(tcp_conn_queue(&a,offered,sizeof(offered))==1024);
    assert(send_one(&a,&b,true));
    for (unsigned i=0; i<3; ++i) {
        assert(send_one(&a,&b,false)); assert(send_one(&b,&a,false));
    }
    assert(a.fast_recovery && a.retransmit_pending && !a.retransmit_timeout);
    uint32_t threshold=a.ssthresh;
    assert(send_one(&a,&b,false)); assert(send_one(&b,&a,false));
    assert(!a.fast_recovery && a.cwnd==threshold); settle();
    for (unsigned batch=0; batch<3; ++batch) {
        for (unsigned i=0; i<8; ++i) assert(tcp_conn_queue(&a,offered,sizeof(offered))==1024);
        settle();
    }
    assert(b.rx_count==TCP_RXBUF_MAX && !a.tx_count && !a.snd_wnd);
    for (unsigned i=0; i<TCP_RXBUF_MAX; ++i) assert(b.rx[i]==offered[i%1024]);
    assert(tcp_conn_queue(&a,"zero-window",11)==11);
    tcp_conn_tick(&a,now); assert(a.persist_deadline);
    now=a.persist_deadline; tcp_conn_tick(&a,now);
    uint32_t cwnd=a.cwnd; uint8_t retries=a.retries;
    assert(send_one(&a,&b,false)); assert(send_one(&b,&a,false));
    assert(a.retries==retries && a.cwnd==cwnd && b.rx_count==TCP_RXBUF_MAX && a.tx_count==11);
    assert(!tcp_conn_consume(&b,TCP_RXBUF_MAX)); assert(send_one(&b,&a,true)); /* lost window update */
    now=a.persist_deadline; tcp_conn_tick(&a,now);
    assert(send_one(&a,&b,false)); assert(send_one(&b,&a,false)); settle();
    assert(!a.tx_count && b.rx_count==11 && !a.persist_deadline);
    assert(tcp_conn_peek(&b,readbuf,20)==11 && !memcmp(readbuf,"zero-window",11));
    setup(false,100,200); handshake(); assert(!tcp_conn_shutdown(&a));
    assert(send_one(&a,&b,true)); /* FIN submitted, peer window closes before it arrives */
    input(&a,a.rcv_nxt,a.snd_una,0,TCP_ACK,NULL,0);
    tcp_conn_tick(&a,now); assert(a.persist_deadline);
    now=a.persist_deadline; tcp_conn_tick(&a,now);
    tcp_action_t probe; assert(!tcp_conn_prepare(&a,&probe,scratch,sizeof(scratch)));
    assert(probe.kind==TCP_ACTION_PROBE && !probe.data_len && probe.header.sequence==a.snd_una-1);
    assert(tcp_conn_commit(&a,&probe,true) && !a.retries);
    input(&a,a.rcv_nxt,a.snd_una,8192,TCP_ACK,NULL,0);
    now=a.rto_deadline; tcp_conn_tick(&a,now); assert(send_one(&a,&b,false)); settle();
    assert(a.fin_acked && a.state==TCP_FIN_WAIT_2);
    setup(false,100,200); b.local_mss=1;
    settle(); assert(a.state==TCP_ESTABLISHED && a.mss==1);
    a.cwnd=8192; assert(tcp_conn_queue(&a,offered,1024)==1024);
    for (unsigned i=0; i<32; ++i) assert(send_one(&a,&b,true));
    assert(a.retx_count==32 && a.snd_nxt-a.snd_una==32);
    tcp_action_t act; assert(tcp_conn_prepare(&a,&act,scratch,sizeof(scratch))==TCP_WOULD_BLOCK);
    input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_ACK,NULL,0);
    assert(!a.retx_count && a.tx_count==992);
    assert(send_one(&a,&b,true));
    puts("[PASS] Reno fast retransmit/recovery, full/zero window, persist/lost update and tiny-MSS descriptor bound");
}
static void pool_cases(void) {
    setup(false,1,2); tcp_pool_init(&pool);
    for (unsigned i=0; i<8; ++i) {
        tcp_tuple_t t=ta; t.local_port=(uint16_t)(40000+i);
        assert(tcp_pool_open(&pool,t,i,1500,true,now)==(int)i);
    }
    tcp_tuple_t other=ta; other.local_port=41000;
    assert(tcp_pool_open(&pool,other,1,1500,true,now)==TCP_NO_SPACE);
    for (unsigned i=0; i<8; ++i) tcp_conn_detach(&pool.blocks[i],now);
    tcp_pool_tick(&pool,now);
    for (unsigned i=0; i<8; ++i) assert(!pool.used[i]);
    int slot=tcp_pool_open(&pool,ta,1,1500,true,now); assert(slot==0);
    tcp_action_t stale, current;
    assert(!tcp_conn_prepare(&pool.blocks[slot],&stale,scratch,sizeof(scratch)));
    tcp_conn_detach(&pool.blocks[slot],now); tcp_pool_tick(&pool,now);
    assert(tcp_pool_open(&pool,ta,1,1500,true,now)==slot);
    assert(!tcp_conn_prepare(&pool.blocks[slot],&current,scratch,sizeof(scratch)));
    assert(!tcp_conn_commit(&pool.blocks[slot],&stale,true));
    assert(tcp_conn_commit(&pool.blocks[slot],&current,false));
    tcp_conn_detach(&pool.blocks[slot],now); tcp_pool_tick(&pool,now);
    assert(tcp_pool_open(&pool,ta,1,1500,true,now)==slot);
    assert(!tcp_conn_prepare(&pool.blocks[slot],&current,scratch,sizeof(scratch)));
    assert(tcp_conn_commit(&pool.blocks[slot],&current,true));
    tcp_conn_detach(&pool.blocks[slot],now); tcp_pool_tick(&pool,now);
    assert(pool.used[slot] && pool.blocks[slot].reset_pending);
    assert(!tcp_conn_prepare(&pool.blocks[slot],&current,scratch,sizeof(scratch)));
    assert(tcp_conn_commit(&pool.blocks[slot],&current,false));
    tcp_pool_tick(&pool,now+1000); assert(!pool.used[slot]);
    tcp_pool_init(&pool);
    for (unsigned i=0; i<16; ++i) {
        tcp_tuple_t t=ta; t.local_port=(uint16_t)(40000+i);
        slot=tcp_pool_open(&pool,t,100,1500,true,now); assert(slot==0);
        tcp_tuple_t peer={t.remote_ip,t.local_ip,t.remote_port,t.local_port};
        assert(!tcp_conn_init(&b,peer,1000+i,200,1500,false,now));
        tcp_conn_t *c=&pool.blocks[slot];
        assert(send_one(c,&b,false)); assert(send_one(&b,c,false)); assert(send_one(c,&b,false));
        assert(c->state==TCP_ESTABLISHED); tcp_conn_detach(c,now);
        for (unsigned pass=0; pass<20; ++pass) {
            send_one(c,&b,false);
            if (b.state==TCP_CLOSE_WAIT) assert(!tcp_conn_shutdown(&b));
            send_one(&b,c,false);
            if (c->state==TCP_TIME_WAIT && !c->ack_pending && b.state==TCP_CLOSED) break;
        }
        assert(c->state==TCP_TIME_WAIT && !c->ack_pending);
        tcp_pool_tick(&pool,now); assert(!pool.used[slot] && pool.timewait[i].active);
        assert(tcp_pool_open(&pool,t,123,1500,true,now)==TCP_NO_SPACE); ++now;
    }
    assert(tcp_pool_open(&pool,other,1,1500,true,now)==TCP_NO_SPACE);
    tcp_timewait_t record=pool.timewait[0];
    tcp_header_t fin={.source=record.tuple.remote_port,.destination=record.tuple.local_port,
        .sequence=record.rcv_nxt-1,.flags=TCP_FIN|TCP_ACK}, reply;
    now=1000; assert(tcp_pool_timewait_input(&pool,record.tuple,&fin,0,now,&reply));
    assert(reply.acknowledgment==record.rcv_nxt && pool.timewait[0].expires_ms==now+TCP_TIMEWAIT_MS);
    fin.flags=TCP_RST; assert(!tcp_pool_timewait_input(&pool,record.tuple,&fin,0,now,&reply));
    tcp_pool_tick(&pool,TCP_TIMEWAIT_MS+100);
    assert(pool.timewait[0].active && !pool.timewait[1].used);
    assert(tcp_pool_open(&pool,record.tuple,1,1500,true,TCP_TIMEWAIT_MS+100)==TCP_NO_SPACE);
    tcp_pool_tick(&pool,TCP_TIMEWAIT_MS+1000);
    assert(tcp_pool_open(&pool,record.tuple,1,1500,true,TCP_TIMEWAIT_MS+1000)==0);
    pool.generation=UINT64_MAX;
    assert(tcp_pool_open(&pool,other,1,1500,true,TCP_TIMEWAIT_MS+1000)==TCP_NO_SPACE);
    assert(tcp_conn_init(&a,ta,0,1,1500,true,now)==TCP_INVALID);
    assert(tcp_conn_init(&a,ta,1,1,43,true,now)==TCP_INVALID);
    puts("[PASS] bounded pool, reserved/exported TIME_WAIT, duplicate FIN, tuple exclusion and stale reuse");
}
static void control_edges(void) {
    setup(false,100,200);
    input(&a,200,101,8192,TCP_RST|TCP_ACK,NULL,0); /* no SYN submitted yet */
    assert(a.state==TCP_SYN_SENT && !a.error);
    assert(send_one(&a,&b,true));
    input(&a,200,100,8192,TCP_RST|TCP_ACK,NULL,0); assert(!a.error);
    input(&a,200,102,8192,TCP_SYN|TCP_ACK,NULL,0); assert(a.reset_pending);
    assert(send_one(&a,&b,true) && !a.reset_pending);
    input(&a,200,101,8192,TCP_SYN|TCP_ACK,NULL,0);
    assert(a.state==TCP_ESTABLISHED && a.peer_mss==536 && a.mss==536);
    setup(false,100,200); assert(send_one(&a,&b,false));
    input(&b,b.rcv_nxt,b.iss,8192,TCP_ACK,NULL,0);
    assert(b.state==TCP_SYN_RCVD && b.reset_pending); assert(send_one(&b,&a,true));
    setup(false,100,200); handshake();
    input(&a,a.rcv_nxt+TCP_RXBUF_MAX,a.snd_nxt,8192,TCP_ACK,"outside",7);
    assert(!a.rx_count && a.ack_pending); settle();
    saved=a;
    input(&a,a.rcv_nxt,a.snd_nxt,8192,TCP_ACK|TCP_URG,"urgent",6);
    assert(a.state==saved.state && a.rx_count==saved.rx_count);
    setup(false,100,200); handshake();
    /* Both FINs submitted; one FIN is retransmitted carrying the new FIN ACK. */
    assert(!tcp_conn_shutdown(&a) && !tcp_conn_shutdown(&b));
    assert(send_one(&a,&b,true)); assert(send_one(&b,&a,true));
    input(&a,b.fin_sequence,a.snd_una,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(a.state==TCP_CLOSING && !a.fin_acked);
    input(&a,b.fin_sequence,a.snd_nxt,8192,TCP_FIN|TCP_ACK,NULL,0);
    assert(a.state==TCP_TIME_WAIT && a.fin_acked);
    setup(false,100,200); handshake();
    input(&b,b.rcv_nxt,b.snd_nxt,8192,TCP_ACK,"retained",8);
    input(&b,b.rcv_nxt,b.snd_nxt,8192,TCP_RST,NULL,0);
    assert(b.error==TCP_RESET && tcp_conn_peek(&b,readbuf,20)==8);
    assert(!memcmp(readbuf,"retained",8)); assert(!tcp_conn_consume(&b,8));
    assert(tcp_conn_peek(&b,readbuf,20)==TCP_RESET && !b.ack_pending);
    setup(false,100,200); tcp_conn_tick(&a,TCP_HANDSHAKE_MS);
    assert(a.state==TCP_CLOSED && a.error==TCP_TIMEOUT);
    setup(false,100,200); handshake(); a.user_deadline=now+17;
    tcp_conn_tick(&a,now+17); assert(a.error==TCP_TIMEOUT);
    setup(false,100,200); handshake(); tcp_conn_detach(&a,now);
    tcp_conn_tick(&a,now+TCP_ORPHAN_MS); assert(a.error==TCP_TIMEOUT);
    puts("[PASS] SYN/ACK/MSS/control validation, duplicate FIN carrying ACK, buffered-before-reset and separate deadlines");
}
static void control_loss(void) {
    setup(false,100,200);
    assert(send_one(&a,&b,false)); assert(send_one(&b,&a,false));
    assert(send_one(&a,&b,true)); /* final handshake ACK lost */
    assert(a.state==TCP_ESTABLISHED && b.state==TCP_SYN_RCVD);
    now=b.rto_deadline; tcp_conn_tick(&b,now);
    assert(send_one(&b,&a,false)); settle();
    assert(a.state==TCP_ESTABLISHED && b.state==TCP_ESTABLISHED && b.snd_nxt==201);
    assert(!tcp_conn_shutdown(&a)); assert(send_one(&a,&b,true));
    now=a.rto_deadline; tcp_conn_tick(&a,now); assert(send_one(&a,&b,false));
    assert(send_one(&b,&a,false)); assert(a.state==TCP_FIN_WAIT_2);
    assert(!tcp_conn_shutdown(&b)); assert(send_one(&b,&a,false));
    assert(send_one(&a,&b,true)); /* ACK of final FIN lost */
    assert(a.state==TCP_TIME_WAIT && b.state==TCP_LAST_ACK);
    now=b.rto_deadline; tcp_conn_tick(&b,now);
    assert(send_one(&b,&a,false)); assert(send_one(&a,&b,false));
    assert(b.state==TCP_CLOSED && a.state==TCP_TIME_WAIT);
    puts("[PASS] lost final handshake ACK, FIN retransmission and lost final teardown ACK recovery");
}
static void invariants(const tcp_conn_t *c) {
    assert(c->rx_count<=TCP_RXBUF_MAX && c->tx_count<=TCP_TXBUF_MAX && c->retx_count<=32);
    assert(c->rcv_nxt==c->rx_sequence+c->rx_count+(c->eof ? 1U : 0U));
    for (unsigned i=0; i<c->rx_count; ++i) {
        unsigned pos=(c->rx_head+i)%TCP_RXBUF_MAX;
        assert(c->rx_valid[pos/8]&(1U<<(pos%8)));
    }
    if (c->state!=TCP_CLOSED) {
        uint32_t sequence=c->snd_una;
        for (unsigned i=0; i<c->retx_count; ++i) {
            assert(c->retx[i].sequence==sequence && c->retx[i].length);
            sequence+=c->retx[i].length;
        }
        assert(sequence==c->snd_nxt);
    }
}
static void hostile_inputs(void) {
    uint32_t random=12345;
    for (unsigned i=0; i<20000; ++i) {
        if (i%97==0 || a.state==TCP_CLOSED) { setup(false,random,random^0xa55a); handshake(); }
        random=random*1664525+1013904223;
        if (i%3==0 && !a.want_fin) (void)tcp_conn_queue(&a,offered,32);
        if (i%5==0) (void)send_one(&a,&b,true);
        ++now;
        uint32_t sequence=a.rcv_nxt+(random%20000)-10000;
        if (i%11==0) sequence=a.rcv_nxt;
        uint32_t ack=a.snd_una+(random>>16)%10000;
        if (i%7==0) ack=a.snd_nxt;
        uint8_t flags=(i%41==0) ? TCP_RST : (i%37==0) ? TCP_SYN|TCP_ACK :
                      (i%31==0) ? TCP_FIN|TCP_ACK : TCP_ACK;
        input(&a,sequence,ack,(uint16_t)random,flags,offered,i%23);
        if (a.rx_count && i%4==0) assert(!tcp_conn_consume(&a,a.rx_count/2));
        invariants(&a);
    }
    puts("[PASS] 20000 checksum-valid hostile segment events with byte/range/sequence invariants");
}

typedef struct { bool used; unsigned side; uint64_t due; size_t len; uint8_t bytes[1480]; } packet_t;
static packet_t packets[256];
static unsigned serial,drops,duplicates,reorders,local_failures;
static uint8_t pattern(unsigned side, size_t i) { return (uint8_t)((i*73+(i>>8)+side*101)&255); }
static void enqueue(unsigned side, size_t len, uint64_t due) {
    for (unsigned i=0; i<256; ++i) if (!packets[i].used) {
        packets[i].used=true; packets[i].side=side; packets[i].due=due;
        packets[i].len=len; memcpy(packets[i].bytes,wire,len); return;
    }
    assert(!"simulator packet bound exhausted");
}
static bool emit(unsigned side, bool faults) {
    tcp_conn_t *c=side ? &b : &a; tcp_action_t act;
    int result=tcp_conn_prepare(c,&act,scratch,sizeof(scratch));
    if (result==TCP_WOULD_BLOCK) return false;
    assert(!result); ++serial;
    if (faults && serial%23==0) {
        assert(tcp_conn_commit(c,&act,false)); ++local_failures; return true;
    }
    assert(!tcp_encode(wire,sizeof(wire),c->tuple.local_ip,c->tuple.remote_ip,&act.header,scratch,act.data_len));
    size_t len=(act.header.has_mss ? 24 : 20)+act.data_len;
    assert(tcp_conn_commit(c,&act,true));
    if (faults && serial%17==0) { ++drops; return true; }
    uint64_t delay=faults ? 1+serial%31 : 1;
    if (faults && serial%7==0) { delay+=50; ++reorders; }
    enqueue(side,len,now+delay);
    if (faults && serial%11==0) { enqueue(side,len,now+delay+7); ++duplicates; }
    return true;
}
static void simulate(bool faults, size_t total, unsigned seed) {
    setup(false,0xfffff000U,0xffffe000U); memset(packets,0,sizeof(packets));
    serial=seed; drops=duplicates=reorders=local_failures=0;
    size_t produced[2]={0},received[2]={0}; bool closed[2]={false};
    for (now=0; now<2000000; ++now) {
        tcp_conn_tick(&a,now); tcp_conn_tick(&b,now);
        for (unsigned i=0; i<256; ++i) if (packets[i].used && packets[i].due<=now) {
            unsigned side=packets[i].side;
            deliver(side ? &a : &b,side ? tb : ta,packets[i].bytes,packets[i].len);
            packets[i].used=false;
        }
        for (unsigned side=0; side<2; ++side) {
            tcp_conn_t *c=side ? &b : &a;
            assert(!c->error && c->tx_count<=TCP_TXBUF_MAX && c->rx_count<=TCP_RXBUF_MAX && c->retx_count<=32);
            if (produced[side]<total && (c->state==TCP_ESTABLISHED || c->state==TCP_CLOSE_WAIT)) {
                size_t n=total-produced[side]; if (n>sizeof(offered)) n=sizeof(offered);
                for (size_t i=0; i<n; ++i) offered[i]=pattern(side,produced[side]+i);
                int result=tcp_conn_queue(c,offered,n);
                assert(result>0 || result==TCP_WOULD_BLOCK);
                if (result>0) produced[side]+=(unsigned)result;
            }
            /* Intentionally pause application reads to exercise backpressure. */
            if (!faults || now%127<20) {
                int n=tcp_conn_peek(c,readbuf,sizeof(readbuf));
                if (n>0) {
                    for (int i=0; i<n; ++i) assert(readbuf[i]==pattern(1-side,received[side]+(unsigned)i));
                    received[side]+=(unsigned)n; assert(received[side]<=total);
                    assert(!tcp_conn_consume(c,(unsigned)n));
                }
            }
            if (produced[side]==total && !closed[side]) { assert(!tcp_conn_shutdown(c)); closed[side]=true; }
            for (unsigned i=0; i<32; ++i) if (!emit(side,faults)) break;
        }
        if (received[0]==total && received[1]==total &&
            (a.state==TCP_TIME_WAIT || a.state==TCP_CLOSED) &&
            (b.state==TCP_TIME_WAIT || b.state==TCP_CLOSED)) {
            if (faults) assert(drops && duplicates && reorders && local_failures);
            printf("[PASS] %zu bytes each direction, %s, time=%llu ms drops=%u duplicates=%u reorder=%u local failures=%u\n",
                total,faults ? "loss/reorder/backpressure" : "clean",(unsigned long long)now,drops,duplicates,reorders,local_failures);
            return;
        }
    }
    fprintf(stderr,"stalled: produced %zu/%zu received %zu/%zu states %d/%d tx %u/%u rx %u/%u\n",
        produced[0],produced[1],received[0],received[1],a.state,b.state,a.tx_count,b.tx_count,a.rx_count,b.rx_count);
    assert(!"bounded simulation did not complete");
}
int main(void) {
    transactions(); partial_ack(); receive_cases(); timers(); reset_and_close();
    windows_and_reno(); pool_cases(); control_edges(); control_loss(); hostile_inputs();
    simulate(false,2*1024*1024,0);
    simulate(true,256*1024,0); simulate(true,256*1024,16); simulate(true,256*1024,10);
    printf("Sizes: CB=%zu retransmission=%zu action=%zu TIME_WAIT=%zu pool=%zu\n",
        sizeof(tcp_conn_t),sizeof(tcp_retx_t),sizeof(tcp_action_t),sizeof(tcp_timewait_t),sizeof(tcp_pool_t));
    puts("TCP transport ASan/UBSan PASS (pure engine/fake time, not live socket or hardware acceptance)");
}
