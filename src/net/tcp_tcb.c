#include "tcp_tcb.h"
#include "string.h"

static uint32_t min32(uint32_t a, uint32_t b) { return a<b ? a : b; }
static uint64_t deadline(uint64_t now, uint32_t delay) {
    return now>UINT64_MAX-delay ? UINT64_MAX : now+delay;
}
bool tcp_seq_before(uint32_t a, uint32_t b) { return a-b>0x80000000U; }
static int64_t offset(uint32_t a, uint32_t b) {
    uint32_t d=a-b;
    return d<=0x7fffffffU ? (int64_t)d : -(int64_t)(uint32_t)(b-a);
}
static void changed(tcp_cb_t *c) {
    ++c->revision; c->action_pending=false;
}
static uint16_t window(const tcp_cb_t *c) {
    return c->eof ? 0 : (uint16_t)(TCP_BUFFER_SIZE-c->rx_count);
}
static tcp_header_t header(const tcp_cb_t *c) {
    return (tcp_header_t){.source=c->tuple.local_port,.destination=c->tuple.remote_port,
        .sequence=c->snd_nxt,.acknowledgment=c->rcv_nxt,.flags=TCP_ACK,.window=window(c)};
}
static void reset_reply(tcp_cb_t *c, const tcp_header_t *h, size_t len) {
    c->reset_header=header(c); c->reset_header.window=0;
    c->reset_header.flags=(h->flags&TCP_ACK) ? TCP_RST : TCP_RST|TCP_ACK;
    c->reset_header.sequence=(h->flags&TCP_ACK) ? h->acknowledgment : 0;
    c->reset_header.acknowledgment=(h->flags&TCP_ACK) ? 0 :
        h->sequence+(uint32_t)len+((h->flags&TCP_SYN)!=0)+((h->flags&TCP_FIN)!=0);
    c->reset_pending=true;
    if (c->state==TCP_CLOSED && !c->user_deadline) c->user_deadline=deadline(c->now_ms,1000);
}
static void fail(tcp_cb_t *c, int error, bool reset) {
    if (reset) { c->reset_header=header(c); c->reset_header.flags=TCP_RST|TCP_ACK; }
    c->reset_pending=reset; c->state=TCP_CLOSED; c->error=error;
    c->retx_count=0; c->tx_count=0; c->ack_pending=false;
    c->rto_deadline=c->persist_deadline=c->handshake_deadline=0;
    c->user_deadline=c->finwait2_deadline=0; c->retransmit_pending=false;
    /* A best-effort reset must not pin an orphan forever on local NIC failure. */
    if (reset) c->user_deadline=deadline(c->now_ms,1000);
}
static void timewait(tcp_cb_t *c) {
    c->state=TCP_TIME_WAIT; c->timewait_deadline=deadline(c->now_ms,TCP_TIMEWAIT_MS);
    c->rto_deadline=c->persist_deadline=c->finwait2_deadline=c->user_deadline=0;
}
int tcp_cb_init(tcp_cb_t *c, tcp_tuple_t tuple, uint64_t generation,
                uint32_t isn, uint16_t mtu, bool active, uint64_t now) {
    if (!c || !generation || !tuple.local_port || !tuple.remote_port || mtu<44) return TCP_INVALID;
    memset(c,0,sizeof(*c)); c->tuple=tuple; c->generation=generation; c->revision=1;
    c->now_ms=now; c->iss=c->snd_una=c->snd_nxt=isn; c->tx_sequence=isn+1;
    c->local_mss=(uint16_t)min32(mtu-40,TCP_MSS_MAX); c->peer_mss=536;
    c->mss=(uint16_t)min32(c->local_mss,c->peer_mss);
    c->cwnd=c->mss; c->ssthresh=TCP_BUFFER_SIZE; c->rto_ms=1000;
    c->state=active ? TCP_SYN_SENT : TCP_LISTEN;
    if (active) c->handshake_deadline=deadline(now,TCP_HANDSHAKE_MS);
    return TCP_OK;
}
static void receive_syn(tcp_cb_t *c, const tcp_header_t *h) {
    c->irs=h->sequence; c->rcv_nxt=c->rx_sequence=h->sequence+1;
    c->snd_wnd=h->window; c->snd_wl1=h->sequence; c->snd_wl2=h->acknowledgment;
    c->peer_mss=h->has_mss ? h->mss : 536;
    c->mss=(uint16_t)min32(c->local_mss,c->peer_mss); c->cwnd=c->mss;
}
static void rtt(tcp_cb_t *c, uint64_t elapsed) {
    uint32_t sample=(uint32_t)(elapsed>60000 ? 60000 : elapsed);
    if (!sample) sample=1;
    if (!c->have_rtt) {
        c->srtt_ms=sample; c->rttvar_ms=sample/2; c->have_rtt=true;
    } else {
        uint32_t diff=sample>c->srtt_ms ? sample-c->srtt_ms : c->srtt_ms-sample;
        c->rttvar_ms=(3*c->rttvar_ms+diff)/4;
        c->srtt_ms=(7*c->srtt_ms+sample)/8;
    }
    uint32_t variance=4*c->rttvar_ms;
    c->rto_ms=min32(60000,c->srtt_ms+(variance ? variance : 1));
    if (c->rto_ms<1000) c->rto_ms=1000;
}
static void ack_new(tcp_cb_t *c, uint32_t ack) {
    bool ambiguous=false, sample=false, syn_retry=false;
    uint64_t elapsed=0;
    for (unsigned i=0; i<c->retx_count; ++i) {
        tcp_retx_t *r=&c->retx[i];
        if (tcp_seq_before(r->sequence,ack)) {
            if (r->retransmitted) ambiguous=true;
            if (r->retransmitted && (r->flags&TCP_SYN)) syn_retry=true;
            if (!sample && (uint32_t)(ack-r->sequence)>=r->length) {
                elapsed=c->now_ms-r->sent_ms; sample=true;
            }
        }
    }
    uint32_t bytes=0;
    if (!tcp_seq_before(ack,c->tx_sequence)) bytes=min32(ack-c->tx_sequence,c->tx_count);
    c->tx_head=(uint16_t)((c->tx_head+bytes)%TCP_BUFFER_SIZE);
    c->tx_count=(uint16_t)(c->tx_count-bytes); c->tx_sequence+=bytes;
    unsigned removed=0;
    while (removed<c->retx_count) {
        tcp_retx_t *r=&c->retx[removed];
        uint32_t n=ack-r->sequence;
        if (tcp_seq_before(ack,r->sequence) || !n) break;
        if (n<r->length) { r->sequence+=n; r->length=(uint16_t)(r->length-n); break; }
        ++removed;
    }
    if (removed) {
        memmove(c->retx,c->retx+removed,(c->retx_count-removed)*sizeof(c->retx[0]));
        c->retx_count=(uint8_t)(c->retx_count-removed);
    }
    c->snd_una=ack; c->retries=0; c->dupacks=0;
    c->retransmit_pending=false; c->retransmit_timeout=false;
    if (sample && !ambiguous) rtt(c,elapsed);
    if (syn_retry && c->rto_ms<3000) c->rto_ms=3000;
    c->rto_deadline=c->retx_count ? deadline(c->now_ms,c->rto_ms) : 0;
    if (bytes) {
        if (c->fast_recovery) { c->cwnd=c->ssthresh; c->fast_recovery=false; }
        else if (c->cwnd<c->ssthresh) c->cwnd+=min32(bytes,c->mss);
        else {
            c->ca_acked+=bytes;
            if (c->ca_acked>=c->cwnd) { c->ca_acked-=c->cwnd; c->cwnd+=c->mss; }
        }
        c->cwnd=min32(c->cwnd,TCP_BUFFER_SIZE);
    }
    if (c->fin_sent && ack==c->fin_sequence+1) {
        c->fin_acked=true;
        if (c->state==TCP_FIN_WAIT_1) {
            c->state=TCP_FIN_WAIT_2;
            if (c->orphan) c->finwait2_deadline=deadline(c->now_ms,TCP_FINWAIT2_MS);
        } else if (c->state==TCP_CLOSING) timewait(c);
        else if (c->state==TCP_LAST_ACK) c->state=TCP_CLOSED;
    }
}
static void process_ack(tcp_cb_t *c, const tcp_header_t *h, size_t len) {
    uint32_t old_window=c->snd_wnd;
    if (tcp_seq_before(c->snd_wl1,h->sequence) ||
        (c->snd_wl1==h->sequence && !tcp_seq_before(h->acknowledgment,c->snd_wl2))) {
        c->snd_wnd=h->window; c->snd_wl1=h->sequence; c->snd_wl2=h->acknowledgment;
        if (c->snd_wnd) {
            c->persist_deadline=0; c->persist_backoff=0;
            if (!old_window && c->retx_count) c->rto_deadline=deadline(c->now_ms,c->rto_ms);
        }
    }
    if (tcp_seq_before(c->snd_una,h->acknowledgment)) ack_new(c,h->acknowledgment);
    else if (h->acknowledgment==c->snd_una && c->retx_count && !len &&
             !(h->flags&(TCP_SYN|TCP_FIN)) && old_window==c->snd_wnd) {
        if (c->dupacks<255) ++c->dupacks;
        if (c->dupacks==3 && !c->fast_recovery &&
            !(c->retx[0].flags&(TCP_SYN|TCP_FIN))) {
            uint32_t half=(c->snd_nxt-c->snd_una)/2;
            c->ssthresh=half>2U*c->mss ? half : 2U*c->mss;
            c->cwnd=min32(c->ssthresh+3U*c->mss,TCP_BUFFER_SIZE);
            c->fast_recovery=true; c->retransmit_pending=true; c->retransmit_timeout=false;
        } else if (c->dupacks>3 && c->fast_recovery) c->cwnd=min32(c->cwnd+c->mss,TCP_BUFFER_SIZE);
    }
}
static bool bit(const tcp_cb_t *c, unsigned i) { return (c->rx_valid[i/8]&(1U<<(i%8)))!=0; }
static void setbit(tcp_cb_t *c, unsigned i, bool value) {
    uint8_t mask=(uint8_t)(1U<<(i%8));
    if (value) c->rx_valid[i/8]|=mask; else c->rx_valid[i/8]&=(uint8_t)~mask;
}
static void received_fin(tcp_cb_t *c) {
    if (!c->remote_fin_pending || c->eof || c->rcv_nxt!=c->remote_fin_sequence) return;
    c->eof=true; ++c->rcv_nxt; c->ack_pending=true;
    if (c->state==TCP_ESTABLISHED) c->state=TCP_CLOSE_WAIT;
    else if (c->state==TCP_FIN_WAIT_1) c->state=TCP_CLOSING;
    else if (c->state==TCP_FIN_WAIT_2) timewait(c);
}
static void receive_data(tcp_cb_t *c, const tcp_header_t *h, const uint8_t *data, size_t len) {
    if (c->eof) return;
    int64_t start=offset(h->sequence,c->rx_sequence);
    size_t skip=start<0 ? (size_t)(-start) : 0;
    if (skip<len && start+(int64_t)skip<TCP_BUFFER_SIZE) {
        unsigned pos=(unsigned)(start+(int64_t)skip);
        size_t n=len-skip;
        if (n>TCP_BUFFER_SIZE-pos) n=TCP_BUFFER_SIZE-pos;
        for (size_t i=0; i<n; ++i) {
            uint32_t sequence=h->sequence+(uint32_t)(skip+i);
            if (c->remote_fin_pending && !tcp_seq_before(sequence,c->remote_fin_sequence)) break;
            unsigned index=(c->rx_head+pos+(unsigned)i)%TCP_BUFFER_SIZE;
            if (!bit(c,index)) { c->rx[index]=data[skip+i]; setbit(c,index,true); }
        }
    }
    if ((h->flags&TCP_FIN) && !c->remote_fin_pending) {
        uint32_t fin=h->sequence+(uint32_t)len;
        if (fin-c->rcv_nxt<window(c)) {
            c->remote_fin_sequence=fin; c->remote_fin_pending=true;
        }
    }
    while (c->rx_count<TCP_BUFFER_SIZE &&
           (!c->remote_fin_pending || c->rcv_nxt!=c->remote_fin_sequence) &&
           bit(c,(c->rx_head+c->rx_count)%TCP_BUFFER_SIZE)) {
        ++c->rx_count; ++c->rcv_nxt;
    }
    received_fin(c);
    if (len || (h->flags&TCP_FIN)) c->ack_pending=true;
}
void tcp_cb_tick(tcp_cb_t *c, uint64_t now) {
    if (!c || now<c->now_ms) return;
    changed(c); c->now_ms=now;
    if (c->state==TCP_CLOSED) {
        if (c->reset_pending && c->user_deadline && now>=c->user_deadline) c->reset_pending=false;
        return;
    }
    if (c->state==TCP_TIME_WAIT) {
        if (now>=c->timewait_deadline) { c->state=TCP_CLOSED; c->ack_pending=false; }
        return;
    }
    if ((c->handshake_deadline && now>=c->handshake_deadline) ||
        (c->user_deadline && now>=c->user_deadline) ||
        (c->finwait2_deadline && now>=c->finwait2_deadline)) { fail(c,TCP_TIMEOUT,true); return; }
    bool persist=!c->snd_wnd && c->state!=TCP_SYN_SENT && c->state!=TCP_SYN_RCVD &&
                 (c->tx_count || (c->want_fin && !c->fin_acked));
    if (persist && !c->persist_deadline) c->persist_deadline=deadline(now,c->rto_ms);
    if (!persist) c->persist_deadline=0;
    if (!persist && c->retx_count && c->rto_deadline && now>=c->rto_deadline) {
        if (c->retries>=8) { fail(c,TCP_TIMEOUT,true); return; }
        c->retransmit_pending=true; c->retransmit_timeout=true;
    }
}
void tcp_cb_input(tcp_cb_t *c, const tcp_header_t *h, const uint8_t *data,
                   size_t len, uint64_t now) {
    if (!c || !h || (!data && len) || len>TCP_IPV4_SEGMENT_MAX ||
        h->source!=c->tuple.remote_port || h->destination!=c->tuple.local_port ||
        (h->has_mss && !h->mss) || now<c->now_ms) return;
    tcp_cb_tick(c,now);
    if (c->state==TCP_CLOSED) {
        if (!(h->flags&TCP_RST)) reset_reply(c,h,len);
        return;
    }
    if (c->state==TCP_TIME_WAIT) {
        if (!(h->flags&TCP_RST) && (len || (h->flags&(TCP_SYN|TCP_FIN)))) {
            c->ack_pending=true;
            if ((h->flags&TCP_FIN) && h->sequence+(uint32_t)len==c->rcv_nxt-1)
                c->timewait_deadline=deadline(now,TCP_TIMEWAIT_MS);
        }
        return;
    }
    if (c->state==TCP_LISTEN) {
        if (h->flags&TCP_RST) return;
        if (h->flags&TCP_ACK) {
            reset_reply(c,h,len); return;
        }
        if ((h->flags&TCP_SYN) && !len && !(h->flags&TCP_FIN)) {
            receive_syn(c,h); c->state=TCP_SYN_RCVD; c->syn_ack=true;
            c->handshake_deadline=deadline(now,TCP_HANDSHAKE_MS);
        }
        return;
    }
    if (c->state==TCP_SYN_SENT) {
        bool valid=(h->flags&TCP_ACK) && c->syn_sent && h->acknowledgment==c->snd_nxt;
        if (h->flags&TCP_RST) { if (valid) fail(c,TCP_RESET,false); return; }
        if ((h->flags&TCP_ACK) && !valid) { reset_reply(c,h,len); return; }
        if (!(h->flags&TCP_SYN) || (h->flags&TCP_FIN) || len) return;
        receive_syn(c,h);
        if (valid) {
            ack_new(c,h->acknowledgment); c->state=TCP_ESTABLISHED;
            c->handshake_deadline=0; c->ack_pending=true;
        } else {
            c->state=TCP_SYN_RCVD; c->syn_ack=true;
            if (c->syn_sent) {
                c->retx[0].flags=TCP_SYN|TCP_ACK;
                c->retransmit_pending=true; c->retransmit_timeout=false;
            }
        }
        return;
    }
    if (c->state==TCP_SYN_RCVD && (h->flags&TCP_SYN) && h->sequence==c->irs &&
        !(h->flags&(TCP_RST|TCP_FIN)) && !len) {
        if ((h->flags&TCP_ACK) && c->syn_sent && h->acknowledgment==c->snd_nxt) {
            ack_new(c,h->acknowledgment); c->state=TCP_ESTABLISHED;
            c->handshake_deadline=0; c->ack_pending=true;
        } else if (!(h->flags&TCP_ACK) && c->syn_sent) {
            c->retransmit_pending=true; c->retransmit_timeout=false;
        }
        return;
    }
    /* Duplicate FIN is outside the new receive sequence but still needs ACK. */
    if (c->eof && (h->flags&TCP_FIN) && !(h->flags&(TCP_RST|TCP_SYN)) &&
        h->sequence+(uint32_t)len==c->rcv_nxt-1) {
        if ((h->flags&TCP_ACK) && h->acknowledgment-c->snd_una<=c->snd_nxt-c->snd_una)
            process_ack(c,h,len);
        c->ack_pending=true; return;
    }
    uint32_t space=(uint32_t)len+((h->flags&TCP_SYN)!=0)+((h->flags&TCP_FIN)!=0);
    uint32_t wnd=window(c), off=h->sequence-c->rcv_nxt;
    bool acceptable=space ? (wnd && (off<wnd || h->sequence+space-1-c->rcv_nxt<wnd)) :
                           (wnd ? off<wnd : !off);
    if (!acceptable) { if (!(h->flags&TCP_RST)) c->ack_pending=true; return; }
    if (h->flags&TCP_RST) {
        if (h->sequence==c->rcv_nxt) fail(c,TCP_RESET,false); else c->ack_pending=true;
        return;
    }
    if (h->flags&TCP_SYN) { c->ack_pending=true; return; }
    if (!(h->flags&TCP_ACK) || (h->flags&TCP_URG)) return;
    if (c->state==TCP_SYN_RCVD && (!c->syn_sent || h->acknowledgment!=c->snd_nxt)) {
        reset_reply(c,h,len); return;
    }
    if (tcp_seq_before(c->snd_nxt,h->acknowledgment) ||
        (h->acknowledgment-c->snd_una==0x80000000U)) { c->ack_pending=true; return; }
    if (c->state==TCP_SYN_RCVD) {
        ack_new(c,h->acknowledgment); c->state=TCP_ESTABLISHED; c->handshake_deadline=0;
        process_ack(c,h,len);
    } else if (!tcp_seq_before(h->acknowledgment,c->snd_una)) process_ack(c,h,len);
    if (c->state!=TCP_CLOSED) receive_data(c,h,data,len);
}
int tcp_cb_queue(tcp_cb_t *c, const void *data, size_t len) {
    if (!c || (!data && len)) return TCP_INVALID;
    if (c->error) return c->error;
    if (c->want_fin) return TCP_WRITE_CLOSED;
    if (c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT) return TCP_NOT_CONNECTED;
    if (!len) return 0;
    size_t n=TCP_BUFFER_SIZE-c->tx_count; if (n>len) n=len;
    if (!n) return TCP_WOULD_BLOCK;
    changed(c);
    for (size_t i=0; i<n; ++i) c->tx[(c->tx_head+c->tx_count+i)%TCP_BUFFER_SIZE]=((const uint8_t *)data)[i];
    c->tx_count=(uint16_t)(c->tx_count+n); return (int)n;
}
int tcp_cb_peek(const tcp_cb_t *c, void *data, size_t capacity) {
    if (!c || (!data && capacity)) return TCP_INVALID;
    if (!capacity) return 0;
    size_t n=c->rx_count; if (n>capacity) n=capacity;
    if (!n) return c->error ? c->error : c->eof ? 0 : TCP_WOULD_BLOCK;
    for (size_t i=0; i<n; ++i) ((uint8_t *)data)[i]=c->rx[(c->rx_head+i)%TCP_BUFFER_SIZE];
    return (int)n;
}
int tcp_cb_consume(tcp_cb_t *c, size_t len) {
    if (!c || len>c->rx_count) return TCP_INVALID;
    if (!len) return 0;
    changed(c);
    for (size_t i=0; i<len; ++i) setbit(c,(c->rx_head+(unsigned)i)%TCP_BUFFER_SIZE,false);
    c->rx_head=(uint16_t)((c->rx_head+len)%TCP_BUFFER_SIZE);
    c->rx_count=(uint16_t)(c->rx_count-len); c->rx_sequence+=(uint32_t)len;
    if (!c->eof && c->state!=TCP_CLOSED) c->ack_pending=true;
    return 0;
}
int tcp_cb_shutdown(tcp_cb_t *c) {
    if (!c) return TCP_INVALID;
    if (c->want_fin) return 0;
    if (c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT) return TCP_NOT_CONNECTED;
    changed(c); c->want_fin=true; return 0;
}
void tcp_cb_detach(tcp_cb_t *c, uint64_t now) {
    if (!c || now<c->now_ms) return;
    tcp_cb_tick(c,now); c->orphan=true;
    if (c->state==TCP_LISTEN) { c->state=TCP_CLOSED; return; }
    if (c->state==TCP_SYN_SENT || c->state==TCP_SYN_RCVD) { fail(c,TCP_RESET,c->syn_sent); return; }
    if (c->state==TCP_ESTABLISHED || c->state==TCP_CLOSE_WAIT) c->want_fin=true;
    if (c->state!=TCP_TIME_WAIT && c->state!=TCP_CLOSED) c->user_deadline=deadline(now,TCP_ORPHAN_MS);
    if (c->state==TCP_FIN_WAIT_2) c->finwait2_deadline=deadline(now,TCP_FINWAIT2_MS);
}
static void copy_tx(const tcp_cb_t *c, uint32_t seq, uint8_t *data, size_t n) {
    unsigned off=(unsigned)(seq-c->tx_sequence);
    for (size_t i=0; i<n; ++i) data[i]=c->tx[(c->tx_head+off+i)%TCP_BUFFER_SIZE];
}
int tcp_cb_prepare(tcp_cb_t *c, tcp_action_t *out, void *data, size_t capacity) {
    if (!c || !out || c->action_pending) return TCP_INVALID;
    tcp_action_t a={.generation=c->generation,.revision=c->revision,.header=header(c),.kind=TCP_ACTION_ACK};
    if (c->reset_pending) { a.kind=TCP_ACTION_RESET; a.header=c->reset_header; }
    else if (c->state==TCP_CLOSED || c->state==TCP_LISTEN) return TCP_WOULD_BLOCK;
    else if ((c->state==TCP_SYN_SENT || c->state==TCP_SYN_RCVD) && !c->syn_sent) {
        a.kind=TCP_ACTION_NEW; a.header.sequence=c->iss;
        a.header.flags=(uint8_t)(TCP_SYN|(c->syn_ack ? TCP_ACK : 0));
        a.header.has_mss=true; a.header.mss=c->local_mss;
    } else if (c->retransmit_pending && c->retx_count) {
        tcp_retx_t *r=&c->retx[0]; a.kind=TCP_ACTION_RETX;
        a.header.sequence=r->sequence; a.header.flags=r->flags;
        a.timeout=c->retransmit_timeout;
        if (r->flags&TCP_SYN) { a.header.has_mss=true; a.header.mss=c->local_mss; }
        if (!(r->flags&(TCP_SYN|TCP_FIN))) a.data_len=r->length;
    } else if (c->ack_pending) { /* ACK/window update takes precedence over new bytes. */ }
    else if (c->persist_deadline && c->now_ms>=c->persist_deadline) {
        a.kind=TCP_ACTION_PROBE;
        a.header.sequence=c->snd_una;
        if (c->tx_count) a.data_len=1; else a.header.sequence=c->snd_una-1;
    } else {
        if (c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT) return TCP_WOULD_BLOCK;
        uint32_t flight=c->snd_nxt-c->snd_una;
        uint32_t limit=min32(c->snd_wnd,c->cwnd);
        if (limit<=flight || c->retx_count==TCP_RETX_MAX) return TCP_WOULD_BLOCK;
        uint32_t sent=c->snd_nxt-c->tx_sequence;
        if (sent<c->tx_count) {
            a.kind=TCP_ACTION_NEW; a.data_len=(uint16_t)min32(c->tx_count-sent,min32(c->mss,limit-flight));
            a.header.flags=TCP_ACK|TCP_PSH;
        } else if (c->want_fin && !c->fin_sent && !flight && !c->tx_count) {
            a.kind=TCP_ACTION_NEW; a.header.flags=TCP_FIN|TCP_ACK;
        } else return TCP_WOULD_BLOCK;
    }
    if (a.data_len) {
        if (!data || capacity<a.data_len) return TCP_NO_SPACE;
        copy_tx(c,a.header.sequence,data,a.data_len);
    }
    c->action=a; c->action_pending=true; *out=a; return 0;
}
static bool same_action(const tcp_action_t *a, const tcp_action_t *b) {
    const tcp_header_t *x=&a->header, *y=&b->header;
    /* Never compare C struct padding: assignments need not preserve it. */
    return a->kind==b->kind && a->timeout==b->timeout && a->data_len==b->data_len &&
        x->source==y->source && x->destination==y->destination && x->sequence==y->sequence &&
        x->acknowledgment==y->acknowledgment && x->window==y->window && x->urgent==y->urgent &&
        x->flags==y->flags && x->header_length==y->header_length && x->has_mss==y->has_mss &&
        x->has_wscale==y->has_wscale && x->mss==y->mss && x->wscale==y->wscale;
}
bool tcp_cb_commit(tcp_cb_t *c, const tcp_action_t *a, bool submitted) {
    if (!c || !a || !c->action_pending || a->generation!=c->generation ||
        a->revision!=c->revision || !same_action(a,&c->action)) return false;
    c->action_pending=false;
    if (!submitted) { ++c->revision; return true; }
    ++c->revision;
    if (a->kind==TCP_ACTION_RESET) {
        c->reset_pending=false;
        if (c->state==TCP_CLOSED) c->user_deadline=0;
        return true;
    }
    if (a->header.flags&TCP_ACK) c->ack_pending=false;
    if (a->kind==TCP_ACTION_RETX) {
        c->retx[0].retransmitted=true;
        if (a->timeout) {
            c->ssthresh=(c->snd_nxt-c->snd_una)/2;
            if (c->ssthresh<2U*c->mss) c->ssthresh=2U*c->mss;
            c->cwnd=c->mss; c->ca_acked=0; c->fast_recovery=false; c->dupacks=0;
            c->rto_ms=min32(60000,c->rto_ms*2); ++c->retries;
        }
        c->rto_deadline=deadline(c->now_ms,c->rto_ms);
        c->retransmit_pending=c->retransmit_timeout=false;
        return true;
    }
    bool new_probe=a->kind==TCP_ACTION_PROBE && a->data_len && a->header.sequence==c->snd_nxt;
    if (a->kind==TCP_ACTION_NEW || new_probe) {
        unsigned n=a->data_len;
        if (a->header.flags&(TCP_SYN|TCP_FIN)) n=1;
        if (c->retx_count>=TCP_RETX_MAX) return false; /* prepare prevents this. */
        c->retx[c->retx_count++]=(tcp_retx_t){.sent_ms=c->now_ms,
            .sequence=a->header.sequence,.length=(uint16_t)n,.flags=a->header.flags};
        c->snd_nxt+=n;
        if (!c->rto_deadline) c->rto_deadline=deadline(c->now_ms,c->rto_ms);
        if (a->header.flags&TCP_SYN) c->syn_sent=true;
        if (a->header.flags&TCP_FIN) {
            c->fin_sent=true; c->fin_sequence=a->header.sequence;
            c->state=c->state==TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        }
    }
    if (a->kind==TCP_ACTION_PROBE) {
        if (c->retx_count) c->retx[0].retransmitted=true; /* Karn: probe ACK is ambiguous. */
        uint32_t interval=c->rto_ms;
        if (c->persist_backoff<6) ++c->persist_backoff;
        for (unsigned i=0; i<c->persist_backoff && interval<60000; ++i) interval=min32(interval*2,60000);
        c->persist_deadline=deadline(c->now_ms,interval);
    }
    return true;
}
static bool same_tuple(tcp_tuple_t a, tcp_tuple_t b) {
    return a.local_ip==b.local_ip && a.remote_ip==b.remote_ip &&
           a.local_port==b.local_port && a.remote_port==b.remote_port;
}
void tcp_pool_init(tcp_pool_t *p) { if (p) memset(p,0,sizeof(*p)); }
int tcp_pool_open(tcp_pool_t *p, tcp_tuple_t tuple, uint32_t isn,
                  uint16_t mtu, bool active, uint64_t now) {
    if (!p) return TCP_INVALID;
    unsigned slot=TCP_CB_MAX, tw=TCP_TIMEWAIT_MAX;
    for (unsigned i=0; i<TCP_TIMEWAIT_MAX; ++i) {
        if (p->timewait[i].used && same_tuple(p->timewait[i].tuple,tuple)) return TCP_NO_SPACE;
        if (!p->timewait[i].used && tw==TCP_TIMEWAIT_MAX) tw=i;
    }
    for (unsigned i=0; i<TCP_CB_MAX; ++i) if (!p->used[i]) { slot=i; break; }
    if (slot==TCP_CB_MAX || tw==TCP_TIMEWAIT_MAX || p->generation==UINT64_MAX) return TCP_NO_SPACE;
    int result=tcp_cb_init(&p->blocks[slot],tuple,p->generation+1,isn,mtu,active,now);
    if (result) return result;
    ++p->generation; p->used[slot]=true; p->tw_slot[slot]=(uint8_t)tw;
    p->timewait[tw]=(tcp_timewait_t){.tuple=tuple,.generation=p->generation,.used=true};
    return (int)slot;
}
void tcp_pool_tick(tcp_pool_t *p, uint64_t now) {
    if (!p) return;
    for (unsigned i=0; i<TCP_TIMEWAIT_MAX; ++i)
        if (p->timewait[i].active && now>=p->timewait[i].expires_ms) memset(&p->timewait[i],0,sizeof(p->timewait[i]));
    for (unsigned i=0; i<TCP_CB_MAX; ++i) if (p->used[i]) {
        tcp_cb_t *c=&p->blocks[i]; tcp_cb_tick(c,now);
        if (!c->orphan || c->action_pending || c->reset_pending || c->ack_pending) continue;
        tcp_timewait_t *tw=&p->timewait[p->tw_slot[i]];
        if (c->state==TCP_TIME_WAIT) {
            tw->active=true; tw->expires_ms=c->timewait_deadline;
            tw->snd_nxt=c->snd_nxt; tw->rcv_nxt=c->rcv_nxt;
        } else if (c->state==TCP_CLOSED) memset(tw,0,sizeof(*tw));
        else continue;
        memset(c,0,sizeof(*c)); p->used[i]=false;
    }
}
bool tcp_pool_timewait_input(tcp_pool_t *p, tcp_tuple_t tuple, const tcp_header_t *h,
                              size_t len, uint64_t now, tcp_header_t *ack) {
    if (!p || !h || !ack || len>TCP_IPV4_SEGMENT_MAX || (h->flags&TCP_RST) ||
        h->source!=tuple.remote_port || h->destination!=tuple.local_port ||
        (!len && !(h->flags&(TCP_SYN|TCP_FIN)))) return false;
    for (unsigned i=0; i<TCP_TIMEWAIT_MAX; ++i) {
        tcp_timewait_t *tw=&p->timewait[i];
        if (!tw->active || now>=tw->expires_ms || !same_tuple(tw->tuple,tuple)) continue;
        if ((h->flags&TCP_FIN) && h->sequence+(uint32_t)len==tw->rcv_nxt-1) {
            uint64_t expires=deadline(now,TCP_TIMEWAIT_MS);
            if (expires>tw->expires_ms) tw->expires_ms=expires;
        }
        *ack=(tcp_header_t){.source=tuple.local_port,.destination=tuple.remote_port,
            .sequence=tw->snd_nxt,.acknowledgment=tw->rcv_nxt,.flags=TCP_ACK};
        return true;
    }
    return false;
}
