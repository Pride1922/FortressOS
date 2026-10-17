#include "net_udp_owner.h"
#include "net_ipv4.h"
#include "net_tcp.h"
#include "net_control.h"
#include "thread.h"
#include "apic.h"
#include "spinlock.h"
#include "syscall_abi.h"
#include "string.h"

enum { UDP_IO, UDP_COPY, UDP_SUBMITTED, UDP_COMMIT, UDP_ABORT };
typedef struct {
    uint64_t generation, identity, floor, deadline, lease, payload_ticket;
    file_t *file; /* Owned checked operation ref, never a borrowed fd. */
    net_sockaddr_in_t address, source;
    net_socket_wait_t wait;
    size_t length;
    int64_t result, cancel_status;
    unsigned phase, kind;
    int slab;
    bool active, nonblocking, pin;
    bool timed, started, reserved, tcp_pending;
    uint64_t tcp_generation;
    net_tcp_child_t child;
    file_t *accepted; /* Checked rollback ref until owner cleanup acknowledgement. */
} udp_request_t;
static udp_request_t values[HANDOFF_SLOTS];
/* One shared pool for UDP and TCP. No peer wait retains a send slab. */
static uint8_t payload[8][TCP_RXBUF_MAX];
static bool slab_used[8];
static uint64_t payload_sequence;
static unsigned copy_decisions, slab_waiters, free_slabs=8;
static bool tcp_runnable;
static spinlock_t lock=SPINLOCK_RANKED(1,"udp_handoff_values");
_Static_assert(sizeof(udp_request_t)<=512,"UDP copied record ledger");
_Static_assert(sizeof(values)+sizeof(payload)+32768+12288+4096<589824,"handoff static ceiling");

static void phase(udp_request_t *v,unsigned p) { __atomic_store_n(&v->phase,p,__ATOMIC_RELEASE); }
static void credit_put(unsigned *value) {
    if (!__atomic_fetch_sub(value,1,__ATOMIC_ACQ_REL)) __builtin_trap();
}
bool net_udp_owner_work(void) {
    /* Level publication survives a generic acknowledgement clearing its hint
     * before the rotating pending budget reaches this typed operation. */
    return __atomic_load_n(&tcp_runnable,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&copy_decisions,__ATOMIC_ACQUIRE) ||
        (__atomic_load_n(&slab_waiters,__ATOMIC_ACQUIRE) &&
         __atomic_load_n(&free_slabs,__ATOMIC_ACQUIRE));
}
static int slab_get(udp_request_t *v) {
    if (!v->payload_ticket) {
        if (payload_sequence==UINT64_MAX) return -2;
        v->payload_ticket=++payload_sequence;
        __atomic_add_fetch(&slab_waiters,1,__ATOMIC_RELEASE);
    }
    /* New claims cannot repeatedly take every released slab ahead of older
     * eligible pending work. Only ready-to-copy requests obtain a ticket. */
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (values[i].payload_ticket && values[i].payload_ticket<v->payload_ticket) return -1;
    for (unsigned i=0;i<8;++i) if (!slab_used[i]) {
        slab_used[i]=true; v->payload_ticket=0;
        credit_put(&slab_waiters); credit_put(&free_slabs); return (int)i;
    }
    return -1;
}
static void slab_put(udp_request_t *v) {
    if (v->slab>=0) {
        slab_used[v->slab]=false;
        if (__atomic_fetch_add(&free_slabs,1,__ATOMIC_RELEASE)>=8) __builtin_trap();
    }
    v->slab=-1;
    if (v->payload_ticket) credit_put(&slab_waiters);
    v->payload_ticket=0;
}
void net_udp_abandon(void) { handoff_abandon(&thread_current()->handoff); }
void net_udp_release(void) { handoff_release(&thread_current()->handoff); }
typedef struct { uint64_t sequence, floor, deadline; bool timed; } admission_t;
static bool admission_ready(void *arg) {
    admission_t *w=arg;
    uint64_t now=apic_timer_get_bsp_ticks();
    return handoff_admissible(&w->sequence) || (w->timed &&
        (!net_tcp_deadline_clock_ok() || now<w->floor || now>=w->deadline));
}
static int64_t admission_error(unsigned kind,file_t *file,bool timed,uint64_t deadline,uint64_t *floor) {
    if (kind==NET_OWNER_TCP_SEND || kind==NET_OWNER_TCP_RECV) {
        net_tcp_wait_t wait;
        if (!net_socket_stream(file)) return SYSCALL_EBADF;
        if (!net_tcp_snapshot(net_socket_index(file),&wait)) return SYSCALL_ENOTCONN;
        if (!net_socket_available()) return SYSCALL_EIO;
    }
    if (!timed) return 0;
    uint64_t now=apic_timer_get_bsp_ticks();
    if (!net_tcp_deadline_clock_ok() || now<*floor) return SYSCALL_EIO;
    *floor=now;
    return now>=deadline ? SYSCALL_ETIMEDOUT : 0;
}
static void cancel_offered_connect(int64_t error) {
    /* A FIFO offer can be granted and claimed while its caller is stopped in
     * admission. Resolve that published cancellation on the owner before a
     * live caller can retry CONNECT on this endpoint. No peer wait is deferred. */
    handoff_token_t t=thread_current()->handoff.token;
    net_tcp_owner_cancel(error);
    handoff_reply_t reply;
    while (net_udp_collect(&reply)>0)
        sched_wait_until_ack(handoff_channel(t),handoff_ready,&t);
    net_udp_release();
}
int64_t net_udp_begin(unsigned kind,file_t *file,const net_sockaddr_in_t *a,size_t len,bool nb) {
    return net_tcp_owner_begin(kind,file,a,len,nb,false,0,apic_timer_get_bsp_ticks());
}
int64_t net_tcp_owner_begin(unsigned kind,file_t *file,const net_sockaddr_in_t *a,
                           size_t len,bool nb,bool timed,uint64_t deadline,uint64_t floor) {
    size_t capacity=kind==NET_OWNER_TCP_SEND ? 4096 : kind==NET_OWNER_TCP_RECV ? TCP_RXBUF_MAX : NET_UDP_DATA_MAX;
    if (kind<NET_OWNER_UDP_CREATE || kind>NET_OWNER_TCP_ACCEPT || len>capacity) return SYSCALL_EINVAL;
    tcb_t *self=thread_current();
    bool create=kind==NET_OWNER_UDP_CREATE || kind==NET_OWNER_TCP_CREATE;
    int rc;
    for (;;) {
        /* Before registration the caller still owns its fd and no transient
         * reference may survive an exit without a registered cleanup record. */
        if (process_signal_interrupt()) return SYSCALL_EINTR;
        int64_t error=admission_error(kind,file,timed,deadline,&floor);
        if (error) { if (self->handoff.active) net_udp_abandon(); return error; }
        admission_t w={handoff_admission_snapshot(),floor,deadline,timed};
        rc=handoff_begin(&self->handoff,(handoff_request_t){.actor=self->tid,.value=floor,
            .kind=kind,.control=create || kind==NET_OWNER_UDP_BIND || kind==NET_OWNER_TCP_BIND || kind==NET_OWNER_TCP_SHUTDOWN || kind==NET_OWNER_TCP_LISTEN,
            .retained=true,.cancel_status=SYSCALL_EINTR});
        if (rc!=HANDOFF_WAIT) break;
        if (nb) return SYSCALL_EAGAIN;
        sched_wait_until(&handoff_admission_channel,admission_ready,&w);
    }
    if (rc) return SYSCALL_EIO;
    handoff_token_t t=self->handoff.token;
    /* No signal/wait boundary until the retained reference is registered. */
    bool got=create || vfs_file_get(file);
    uint64_t identity=got && !create ? net_socket_identity(file) : 0;
    uint64_t f=spin_lock_irqsave(&lock);
    values[t.slot]=(udp_request_t){.generation=t.generation,.file=got ? file : NULL,
        .identity=identity,.floor=floor,.address=a ? *a : (net_sockaddr_in_t){0},
        .kind=kind,.length=len,.nonblocking=nb,.slab=-1,.timed=timed,.deadline=deadline};
    spin_unlock_irqrestore(&lock,f);
    if (!got) { net_udp_abandon(); return SYSCALL_EBADF; }
    if (create) {
        file_t *prepared=NULL;
        int64_t result=net_socket_prepare(&prepared);
        f=spin_lock_irqsave(&lock); values[t.slot].file=prepared; spin_unlock_irqrestore(&lock,f);
        /* Owner checks endpoint capacity before reporting allocation failure,
         * preserving ENOSPC ahead of ENOMEM without caller-side reservation. */
        (void)result;
    }
    bool offered=false;
    for (;;) {
        if (process_signal_interrupt()) {
            if (offered && kind==NET_OWNER_TCP_CONNECT) cancel_offered_connect(SYSCALL_EINTR);
            else net_udp_abandon();
            return SYSCALL_EINTR;
        }
        int64_t error=admission_error(kind,file,timed,deadline,&floor);
        if (error) {
            if (offered && kind==NET_OWNER_TCP_CONNECT) cancel_offered_connect(error);
            else if (self->handoff.active) net_udp_abandon();
            return error;
        }
        admission_t w={handoff_admission_snapshot(),floor,deadline,timed};
        if (!offered) {
            f=spin_lock_irqsave(&lock); values[t.slot].floor=floor; spin_unlock_irqrestore(&lock,f);
        }
        rc=handoff_publish(t); offered=true;
        if (rc!=HANDOFF_WAIT) break;
        if (nb) { net_udp_abandon(); return SYSCALL_EAGAIN; }
        sched_wait_until(&handoff_admission_channel,admission_ready,&w);
    }
    if (rc) { net_udp_abandon(); return SYSCALL_EIO; }
    return 0;
}
bool net_udp_ready(void *arg) {
    handoff_token_t t=*(handoff_token_t *)arg;
    return handoff_ready(arg) ||
        (__atomic_load_n(&values[t.slot].generation,__ATOMIC_ACQUIRE)==t.generation &&
         __atomic_load_n(&values[t.slot].phase,__ATOMIC_ACQUIRE)==UDP_COPY);
}
int64_t net_udp_collect(handoff_reply_t *reply) {
    int rc=handoff_collect(thread_current()->handoff.token,reply);
    return rc==HANDOFF_WAIT ? 1 : rc ? SYSCALL_EIO : 0;
}
void net_tcp_owner_cancel(int64_t status) {
    handoff_token_t t=thread_current()->handoff.token;
    uint64_t f=spin_lock_irqsave(&lock);
    if (values[t.slot].generation==t.generation) values[t.slot].cancel_status=status;
    spin_unlock_irqrestore(&lock,f);
    handoff_cancel(t);
}
bool net_tcp_owner_connect_ack(void) {
    handoff_token_t t=thread_current()->handoff.token;
    uint64_t f=spin_lock_irqsave(&lock);
    udp_request_t *v=&values[t.slot];
    bool ok=v->generation==t.generation && v->kind==NET_OWNER_TCP_CONNECT && v->phase==UDP_COPY;
    if (ok) {
        phase(v,UDP_COMMIT);
        __atomic_add_fetch(&copy_decisions,1,__ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&lock,f);
    if (ok) handoff_notify();
    return ok;
}
bool net_tcp_owner_accept_pin(net_sockaddr_in_t *peer) {
    handoff_token_t t=thread_current()->handoff.token;
    uint64_t f=spin_lock_irqsave(&lock);
    udp_request_t *v=&values[t.slot];
    bool ok=v->generation==t.generation && v->kind==NET_OWNER_TCP_ACCEPT &&
        v->phase==UDP_COPY && !v->pin;
    if (ok) { v->pin=true; *peer=v->source; }
    spin_unlock_irqrestore(&lock,f); return ok;
}
void net_tcp_owner_accept_decide(file_t *prepared,int64_t error) {
    handoff_token_t t=thread_current()->handoff.token;
    /* Caller owns prepared (fd or local reference). No signal boundary before
     * storing this checked reference in the registered cleanup record. */
    if (prepared && !vfs_file_get(prepared)) __builtin_trap();
    uint64_t f=spin_lock_irqsave(&lock);
    udp_request_t *v=&values[t.slot];
    if (v->generation!=t.generation || v->kind!=NET_OWNER_TCP_ACCEPT || !v->pin || v->accepted) __builtin_trap();
    v->accepted=prepared; v->cancel_status=error; v->pin=false;
    phase(v,error ? UDP_ABORT : UDP_COMMIT);
    __atomic_add_fetch(&copy_decisions,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&lock,f); handoff_notify();
}
bool net_udp_pin(const uint8_t **data,int64_t *result,net_sockaddr_in_t *source) {
    handoff_token_t t=thread_current()->handoff.token;
    uint64_t f=spin_lock_irqsave(&lock);
    udp_request_t *v=&values[t.slot];
    bool ok=v->generation==t.generation && v->phase==UDP_COPY && !v->pin && v->slab>=0;
    if (ok) {
        v->pin=true; *data=payload[v->slab]; *result=v->result;
        if (source) *source=v->source;
    }
    spin_unlock_irqrestore(&lock,f); return ok;
}
void net_udp_unpin(bool commit) {
    handoff_token_t t=thread_current()->handoff.token;
    uint64_t f=spin_lock_irqsave(&lock);
    udp_request_t *v=&values[t.slot];
    if (v->generation!=t.generation || !v->pin) __builtin_trap();
    v->pin=false; phase(v,commit ? UDP_COMMIT : UDP_ABORT);
    __atomic_add_fetch(&copy_decisions,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&lock,f); handoff_notify();
}
file_t *net_udp_created(void) {
    handoff_token_t t=thread_current()->handoff.token;
    file_t *file=values[t.slot].file;
    return vfs_file_get(file) ? file : NULL;
}

static void cleanup(udp_request_t *v,bool consume) {
    if (v->active) {
        net_socket_finish(&v->wait,consume);
        if (!v->wait.receive) net_ipv4_udp_sync(v->wait.slot,0);
        v->active=false;
    }
}
static bool reader_reserved(udp_request_t *v) {
    /* Owner-only reservation metadata. Other readers cannot consume the head
     * until commit/abort/lease release; new RX appends behind it. */
    bool reserved=false;
    uint64_t f=spin_lock_irqsave(&lock);
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (&values[i]!=v && values[i].reserved && values[i].identity==v->identity &&
            values[i].file && net_socket_index(values[i].file)==net_socket_index(v->file)) {
            reserved=true; break;
        }
    spin_unlock_irqrestore(&lock,f);
    return reserved;
}
static unsigned accept_excluded(udp_request_t *self) {
    unsigned excluded=0;
    uint64_t f=spin_lock_irqsave(&lock);
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        udp_request_t *v=&values[i];
        if (v!=self && v->kind==NET_OWNER_TCP_ACCEPT && v->reserved &&
            v->child.block>=0 && v->child.block<(int)TCP_CB_MAX)
            excluded|=1U<<v->child.block;
    }
    spin_unlock_irqrestore(&lock,f); return excluded;
}
static void accept_step(handoff_token_t t) {
    udp_request_t *v=&values[t.slot];
    uint64_t now=apic_timer_get_bsp_ticks(), hz=apic_timer_get_frequency();
    bool cancel=handoff_cancelled(t);
    uint64_t f=spin_lock_irqsave(&lock);
    if (v->pin) { spin_unlock_irqrestore(&lock,f); handoff_finish(t,(handoff_reply_t){0},true); return; }
    unsigned p=v->phase;
    bool decision=p==UDP_COMMIT || p==UDP_ABORT;
    int64_t cancellation=v->cancel_status;
    if (decision) credit_put(&copy_decisions);
    bool expiry=p==UDP_COPY && (now>=v->lease || now<v->floor);
    /* Close the pin/decision race before leaving the lock. Unpinned offers may
     * be revoked; a published decision is never revoked by lease or signals. */
    if (expiry || cancel || decision) phase(v,UDP_IO);
    if (expiry && !cancel && !decision) {
        v->reserved=false;
        spin_unlock_irqrestore(&lock,f);
        /* Give competing pending acceptors a turn before offering this child
         * again to a stopped caller. Expiry is internal retry, not a timeout. */
        v->tcp_pending=true; handoff_notify();
        handoff_finish(t,(handoff_reply_t){0},true); return;
    }
    spin_unlock_irqrestore(&lock,f);
    int64_t result=0; bool pending=false;
    unsigned slot=net_socket_index(v->file);
    if (p==UDP_COMMIT || (p==UDP_ABORT && v->accepted && cancellation==SYSCALL_EMFILE)) {
        result=net_socket_activate_staged(v->accepted);
        if (!result && p==UDP_ABORT) result=SYSCALL_EMFILE;
        else if (!result) {
            result=net_tcp_accept_commit(slot,net_socket_index(v->accepted),v->child);
            if (!result) net_socket_finish_adopt(v->accepted);
        }
    } else if (p==UDP_ABORT || cancel) result=cancellation ? cancellation : SYSCALL_EINTR;
    else if (!net_socket_identity_live(v->file,v->identity)) result=SYSCALL_EBADF;
    else {
        net_tcp_wait_t wait;
        if (!net_tcp_snapshot(slot,&wait) || (v->tcp_generation && wait.generation!=v->tcp_generation)) result=SYSCALL_EBADF;
        else if (!net_socket_available()) result=SYSCALL_EIO;
        else if (p==UDP_COPY && !expiry) pending=true;
        else {
            v->reserved=false;
            v->tcp_generation=wait.generation;
            net_tcp_child_t child; net_sockaddr_in_t peer;
            result=net_tcp_accept_offer(slot,accept_excluded(v),&child,&peer);
            if (!result) {
                if (!hz || now>UINT64_MAX-hz) result=SYSCALL_EIO;
                else {
                    f=spin_lock_irqsave(&lock);
                    v->child=child; v->source=peer; v->reserved=true;
                    v->lease=now+hz; v->floor=now; phase(v,UDP_COPY);
                    spin_unlock_irqrestore(&lock,f); pending=true;
                    sched_wake_all(handoff_channel(t));
                }
            } else if (result==SYSCALL_EAGAIN) {
                if (v->nonblocking) {
                    result=SYSCALL_EAGAIN;
                    pending=false;
                } else {
                    pending=true;
                }
            }
        }
    }
    if (!pending) {
        f=spin_lock_irqsave(&lock);
        if (v->pin || (!decision && (v->phase==UDP_COMMIT || v->phase==UDP_ABORT))) {
            spin_unlock_irqrestore(&lock,f); v->tcp_pending=true;
            handoff_finish(t,(handoff_reply_t){0},true); return;
        }
        phase(v,UDP_IO); v->reserved=false;
        file_t *accepted=v->accepted; v->accepted=NULL;
        spin_unlock_irqrestore(&lock,f);
        /* Adoption/rollback is resolved; a live provisional fd owns its own
         * reference. Dead callers have closed theirs, so final put publishes
         * detach here without requiring another caller or protocol callback. */
        if (accepted) vfs_close(accepted);
        handoff_notify();
    }
    v->tcp_pending=pending;
    handoff_finish(t,(handoff_reply_t){.status=result},pending);
}
static void tcp_step(handoff_token_t t,handoff_request_t r) {
    if (r.kind==NET_OWNER_TCP_ACCEPT) { accept_step(t); return; }
    udp_request_t *v=&values[t.slot];
    uint64_t now=apic_timer_get_bsp_ticks(), hz=apic_timer_get_frequency();
    bool cancel=handoff_cancelled(t);
    uint64_t f=spin_lock_irqsave(&lock);
    unsigned p=v->phase;
    if (v->pin) { spin_unlock_irqrestore(&lock,f); handoff_finish(t,(handoff_reply_t){0},true); return; }
    bool commit=p==UDP_COMMIT, abort=p==UDP_ABORT;
    int64_t cancellation=v->cancel_status ? v->cancel_status : SYSCALL_EINTR;
    if (commit || abort) credit_put(&copy_decisions);
    bool expiry=p==UDP_COPY && now>=v->lease;
    /* Revocation must exclude a new AP pin/ack under the same lock. Merely
     * sampling an unpinned offer and revoking it later can free pinned bytes. */
    if (p==UDP_COPY && (expiry || cancel || (v->timed &&
        (!net_tcp_deadline_clock_ok() || now<v->floor || now>=v->deadline)))) phase(v,UDP_IO);
    spin_unlock_irqrestore(&lock,f);
    int64_t result=0; bool pending=false;
    bool receive=r.kind==NET_OWNER_TCP_RECV, send=r.kind==NET_OWNER_TCP_SEND;
    unsigned slot=v->file && v->identity ? net_socket_index(v->file) : 0;
    net_tcp_wait_t precedence;
    if (receive && commit) {
        /* Already copied positive bytes are irrevocable, including after a
         * clock reversal or signal. Retained file fences final detach. */
        if (!v->reserved || net_tcp_consume(slot,(size_t)v->result)) __builtin_trap();
        result=v->result; v->reserved=false;
    } else if (cancel || abort) result=cancellation;
    else if (r.kind!=NET_OWNER_TCP_CREATE && !net_socket_identity_live(v->file,v->identity)) result=SYSCALL_ENOTCONN;
    else if ((send || receive) && (!net_tcp_snapshot(slot,&precedence) ||
        (v->tcp_generation && precedence.generation!=v->tcp_generation))) result=SYSCALL_ENOTCONN;
    else if ((send || receive) && !net_socket_available()) result=SYSCALL_EIO;
    else if (v->timed && (!net_tcp_deadline_clock_ok() || now<v->floor)) result=SYSCALL_EIO;
    else if (v->timed && now>=v->deadline) result=SYSCALL_ETIMEDOUT;
    else if (r.kind==NET_OWNER_TCP_CREATE) result=net_socket_activate_stream(v->file);
    else if (r.kind==NET_OWNER_TCP_BIND) result=net_socket_bind(v->file,&v->address);
    else if (r.kind==NET_OWNER_TCP_SHUTDOWN) result=net_tcp_shutdown(slot);
    else if (r.kind==NET_OWNER_TCP_LISTEN) result=net_socket_available() ?
        net_tcp_listen(slot,(unsigned)v->length) : SYSCALL_EIO;
    else if (!net_socket_available()) result=SYSCALL_EIO;
    else {
        net_tcp_wait_t wait;
        if (!net_tcp_snapshot(slot,&wait) || (v->tcp_generation && v->tcp_generation!=wait.generation))
            result=SYSCALL_ENOTCONN;
        else {
            v->tcp_generation=wait.generation;
            if (v->timed) {
                wait.timed=true; wait.deadline_ticks=v->deadline; wait.clock_floor=v->floor;
                if (!net_tcp_deadline_register(&wait)) result=SYSCALL_ENOTCONN;
            }
            if (r.kind==NET_OWNER_TCP_CONNECT) {
                if (!v->started) {
                    result=v->timed ? net_tcp_connect_until(slot,v->address.address,ntohs(v->address.port),v->deadline) :
                        net_tcp_connect(slot,v->address.address,ntohs(v->address.port));
                    if (!result) v->started=true;
                }
                if (!result && p==UDP_COPY && !expiry) pending=true;
                else if (!result && commit) result=0;
                else if (!result) {
                    result=net_tcp_connected(slot);
                    if (!result) {
                        if (!hz || now>UINT64_MAX-hz) result=SYSCALL_EIO;
                        else {
                            /* Handshake readiness is an offer, not a syscall
                             * return. Caller signal/STOP handling precedes its
                             * bounded acknowledgement; CONNECT is never restarted. */
                            f=spin_lock_irqsave(&lock);
                            v->lease=now+hz; phase(v,UDP_COPY);
                            spin_unlock_irqrestore(&lock,f); pending=true;
                        }
                    }
                }
            } else if (send || receive) {
                if (expiry) {
                    v->reserved=false;
                    f=spin_lock_irqsave(&lock); slab_put(v); phase(v,UDP_IO); spin_unlock_irqrestore(&lock,f);
                    p=UDP_IO;
                }
                if (p==UDP_COPY) pending=true;
                else if (send && commit) {
                    result=net_tcp_send(slot,payload[v->slab],v->length);
                    /* If a competing sender used the space while we copied,
                     * release credit and requeue. No accepted bytes are retried. */
                    f=spin_lock_irqsave(&lock); slab_put(v); phase(v,UDP_IO); spin_unlock_irqrestore(&lock,f);
                } else {
                    result=receive && reader_reserved(v) ? SYSCALL_EAGAIN : net_tcp_probe(slot,receive);
                    if (result>0) {
                        f=spin_lock_irqsave(&lock); v->slab=slab_get(v); spin_unlock_irqrestore(&lock,f);
                        if (v->slab<0) result=v->slab==-2 ? SYSCALL_EIO : SYSCALL_EAGAIN;
                        else if (!hz || now>UINT64_MAX-hz) result=SYSCALL_EIO;
                        else {
                            result=receive ? net_tcp_peek(slot,payload[v->slab],v->length) : (int64_t)v->length;
                            if (result>0) {
                                v->reserved=receive;
                                f=spin_lock_irqsave(&lock);
                                v->result=result; v->lease=now+hz; phase(v,UDP_COPY);
                                spin_unlock_irqrestore(&lock,f); pending=true;
                                sched_wake_all(handoff_channel(t));
                            }
                        }
                    }
                }
            } else result=SYSCALL_EINVAL;
        }
    }
    if (now>=v->floor) v->floor=now;
    if (result==SYSCALL_EAGAIN && !v->nonblocking) pending=true;
    if (!pending) {
        if (r.kind==NET_OWNER_TCP_CONNECT && v->started && result)
            net_tcp_cancel_connect(slot,v->tcp_generation);
        f=spin_lock_irqsave(&lock);
        /* An AP may have pinned after the initial phase snapshot but before a
         * newly observed link/error result. Defer terminal cleanup until its
         * commit/abort; a positive copied receive still wins that race. */
        if (v->pin || ((!commit && !abort) &&
            (v->phase==UDP_COMMIT || v->phase==UDP_ABORT))) {
            /* A complete AP copy can also land after the snapshot. Keep its
             * decision credit and reservation for the next owner turn; only
             * checking pin would lose an already-unpinned positive receive. */
            spin_unlock_irqrestore(&lock,f);
            v->tcp_pending=true;
            handoff_finish(t,(handoff_reply_t){0},true); return;
        }
        phase(v,UDP_IO); slab_put(v); v->reserved=false;
        spin_unlock_irqrestore(&lock,f);
        handoff_notify(); /* Reservation/slab release makes another caller eligible. */
    }
    v->tcp_pending=pending;
    handoff_finish(t,(handoff_reply_t){.status=result},pending);
}
void net_udp_step(handoff_token_t t,handoff_request_t r) {
    net_control_assert_owner();
    if (r.kind>=NET_OWNER_TCP_CREATE) { tcp_step(t,r); return; }
    udp_request_t *v=&values[t.slot];
    uint64_t now=apic_timer_get_bsp_ticks(),hz=apic_timer_get_frequency();
    bool cancel=handoff_cancelled(t);
    uint64_t f=spin_lock_irqsave(&lock);
    /* Detect reversal after any earlier owner turn, not only before admission.
     * Keep the old floor on failure so a pinned copy cannot hide the reset. */
    if (now>=v->floor) v->floor=now;
    unsigned p=v->phase;
    if (v->pin) { spin_unlock_irqrestore(&lock,f); handoff_finish(t,(handoff_reply_t){0},true); return; }
    if (p==UDP_COMMIT || p==UDP_ABORT) credit_put(&copy_decisions);
    bool abort=p==UDP_ABORT;
    bool commit=p==UDP_COMMIT;
    bool expiry=p==UDP_COPY && now>=v->lease;
    if (expiry || cancel || abort || now<v->floor) phase(v,UDP_IO);
    spin_unlock_irqrestore(&lock,f);
    int64_t result=0;
    bool pending=false;
    if (r.kind==NET_OWNER_UDP_RECV && commit) {
        /* Copy commit is irrevocable even when cancellation arrives before
         * this owner turn. No signal boundary occurs inside the copy pin. */
        result=v->result; cleanup(v,true);
    } else if (cancel || abort || now<v->floor) {
        result=now<v->floor ? SYSCALL_EIO : SYSCALL_EINTR;
        if (r.kind==NET_OWNER_UDP_SEND && v->active && net_socket_ready(&v->wait)) {
            int64_t accepted=net_socket_result(&v->wait,NULL,NULL);
            if (accepted>=0) result=accepted;
        }
        cleanup(v,false);
    } else if (r.kind==NET_OWNER_UDP_CREATE) result=net_socket_activate(v->file);
    else if (!net_socket_identity_live(v->file,v->identity)) result=SYSCALL_EBADF;
    else if (r.kind==NET_OWNER_UDP_BIND) result=net_socket_bind(v->file,&v->address);
    else if (r.kind==NET_OWNER_UDP_SEND) {
        if (expiry) { f=spin_lock_irqsave(&lock); slab_put(v); spin_unlock_irqrestore(&lock,f); p=UDP_IO; }
        if (p==UDP_IO) {
            f=spin_lock_irqsave(&lock);
            if (!net_socket_available()) result=SYSCALL_EIO;
            else v->slab=slab_get(v);
            if (v->slab>=0 && hz && now<=UINT64_MAX-hz) {
                v->result=(int64_t)v->length; v->lease=now+hz; phase(v,UDP_COPY);
            } else if (v->slab>=0) { slab_put(v); result=SYSCALL_EIO; }
            else if (v->slab==-2) result=SYSCALL_EIO;
            spin_unlock_irqrestore(&lock,f);
            pending=!result;
        } else if (p==UDP_COMMIT) {
            result=net_socket_send(v->file,&v->address,payload[v->slab],v->length,&v->wait);
            if (!result) net_socket_retain(&v->wait,0);
            f=spin_lock_irqsave(&lock); slab_put(v); phase(v,UDP_SUBMITTED); spin_unlock_irqrestore(&lock,f);
            v->active=!result; pending=v->active;
        } else if (p==UDP_SUBMITTED) {
            if (!net_socket_ready(&v->wait)) pending=true;
            else { result=net_socket_result(&v->wait,NULL,NULL); cleanup(v,false); }
        } else pending=true;
    } else if (r.kind==NET_OWNER_UDP_RECV) {
        if (commit) { result=v->result; cleanup(v,true); }
        else {
            if (expiry) {
                cleanup(v,false);
                f=spin_lock_irqsave(&lock); slab_put(v); spin_unlock_irqrestore(&lock,f);
                p=UDP_IO;
            }
            if (!v->deadline) {
                if (!hz || hz>UINT64_MAX/5 || now>UINT64_MAX-5*hz) result=SYSCALL_EIO;
                else v->deadline=now+5*hz;
            }
            if (!result && !v->active) {
                if (!net_socket_available()) result=SYSCALL_EIO;
                else if (now>=v->deadline) result=SYSCALL_EAGAIN;
                else {
                    /* Nonblocking admission must not autobind an endpoint
                     * before discovering internal payload-credit saturation. */
                    if (v->nonblocking && v->slab<0) {
                        f=spin_lock_irqsave(&lock); v->slab=slab_get(v); spin_unlock_irqrestore(&lock,f);
                        if (v->slab<0) result=v->slab==-2 ? SYSCALL_EIO : SYSCALL_EAGAIN;
                    }
                    if (!result) result=net_socket_receive(v->file,v->nonblocking,&v->wait);
                    v->active=!result;
                    if (v->active) net_socket_retain(&v->wait,v->deadline);
                }
            }
            if (!result && p==UDP_COPY && !expiry) pending=true;
            else if (!result && v->active) {
                if (!net_socket_ready(&v->wait)) pending=true;
                else {
                    const uint8_t *data=NULL; net_sockaddr_in_t source;
                    result=net_socket_result(&v->wait,&data,&source);
                    if (result>=0) {
                        f=spin_lock_irqsave(&lock); int slab=v->slab>=0 ? v->slab : slab_get(v); spin_unlock_irqrestore(&lock,f);
                        if (slab<0) {
                            if (slab==-2) result=SYSCALL_EIO;
                            else if (v->nonblocking) result=SYSCALL_EAGAIN;
                            else pending=true;
                        }
                        else {
                            if (result) memcpy(payload[slab],data,(size_t)result);
                            f=spin_lock_irqsave(&lock);
                            v->slab=slab; v->source=source; v->result=result; v->lease=now+hz;
                            phase(v,UDP_COPY); spin_unlock_irqrestore(&lock,f); pending=true;
                        }
                    }
                    if (!pending) cleanup(v,false);
                }
            }
        }
    } else result=SYSCALL_EINVAL;
    if (!pending) {
        f=spin_lock_irqsave(&lock); slab_put(v); phase(v,UDP_IO); spin_unlock_irqrestore(&lock,f);
    }
    handoff_finish(t,(handoff_reply_t){.status=result},pending);
    if (expiry || commit) handoff_notify(); /* Released payload/reservation credit. */
}
void net_udp_maintenance(void) {
    net_control_assert_owner();
    for (unsigned i=0;i<8;++i) {
        handoff_token_t t; handoff_request_t r; handoff_state_t st; bool caller;
        if (!handoff_inspect(&t,&r,&st,&caller) || r.kind<NET_OWNER_UDP_CREATE || r.kind>NET_OWNER_TCP_ACCEPT) continue;
        if (st!=HANDOFF_COMPLETED || caller) continue;
        udp_request_t *v=&values[t.slot];
        cleanup(v,false);
        uint64_t f=spin_lock_irqsave(&lock);
        if (v->pin) __builtin_trap();
        slab_put(v); file_t *file=v->file, *accepted=v->accepted; v->file=NULL; v->accepted=NULL;
        spin_unlock_irqrestore(&lock,f);
        /* Final put invokes only detach publication; no lock is held. */
        if (file) vfs_close(file);
        if (accepted) vfs_close(accepted);
        handoff_cleaned(t);
    }
}

void net_udp_refresh_work(void) {
    net_control_assert_owner();
    bool runnable=false;
    uint64_t now=apic_timer_get_bsp_ticks();
    /* Bounded readiness scan, no payload copy or protocol mutation. Keep work
     * level-published even if the rotating eight-step budget misses a record. */
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        udp_request_t *v=&values[i];
        uint64_t f=spin_lock_irqsave(&lock);
        if (!v->tcp_pending) { spin_unlock_irqrestore(&lock,f); continue; }
        unsigned p=v->phase; bool pinned=v->pin;
        spin_unlock_irqrestore(&lock,f);
        if (pinned) continue;
        if (p==UDP_COMMIT || p==UDP_ABORT || (p==UDP_COPY && now>=v->lease) ||
            (v->timed && (now<v->floor || now>=v->deadline || !net_tcp_deadline_clock_ok()))) {
            runnable=true; break;
        }
        if (p!=UDP_IO || !v->file || !v->identity ||
            (v->kind==NET_OWNER_TCP_RECV && reader_reserved(v))) continue;
        int64_t available;
        if (v->kind==NET_OWNER_TCP_ACCEPT) {
            net_tcp_child_t child; net_sockaddr_in_t peer;
            available=net_tcp_accept_offer(net_socket_index(v->file),accept_excluded(v),&child,&peer);
        } else available=v->kind==NET_OWNER_TCP_CONNECT ? net_tcp_connected(net_socket_index(v->file)) :
            net_tcp_probe(net_socket_index(v->file),v->kind==NET_OWNER_TCP_RECV);
        /* EOF/errors and CONNECT decisions need no payload credit. Only a
         * positive data offer waits for one of the eight shared slabs. */
        if (available!=SYSCALL_EAGAIN && (available<=0 ||
            v->kind==NET_OWNER_TCP_CONNECT || __atomic_load_n(&free_slabs,__ATOMIC_ACQUIRE))) {
            runnable=true; break;
        }
    }
    __atomic_store_n(&tcp_runnable,runnable,__ATOMIC_RELEASE);
}
