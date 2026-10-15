#include "net_udp_owner.h"
#include "net_ipv4.h"
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
    int64_t result;
    unsigned phase;
    int slab;
    bool active, nonblocking, pin;
} udp_request_t;
static udp_request_t values[HANDOFF_SLOTS];
/* UDP needs only 1472 bytes. Eight staging slabs reserve the campaign's eight
 * payload credits; later TCP integration must share these credits/storage. */
static uint8_t payload[8][NET_UDP_DATA_MAX];
static bool slab_used[8];
static uint64_t payload_sequence;
static unsigned copy_decisions, slab_waiters, free_slabs=8;
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
    return __atomic_load_n(&copy_decisions,__ATOMIC_ACQUIRE) ||
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
int64_t net_udp_begin(unsigned kind,file_t *file,const net_sockaddr_in_t *a,size_t len,bool nb) {
    tcb_t *self=thread_current();
    uint64_t floor=apic_timer_get_bsp_ticks();
    int rc;
    for (;;) {
        /* Before registration the caller still owns its fd and no transient
         * reference may survive an exit without a registered cleanup record. */
        if (process_signal_interrupt()) return SYSCALL_EINTR;
        uint64_t sequence=handoff_admission_snapshot();
        rc=handoff_begin(&self->handoff,(handoff_request_t){.actor=self->tid,.value=floor,
            .kind=kind,.control=kind==NET_OWNER_UDP_CREATE || kind==NET_OWNER_UDP_BIND,
            .retained=true,.cancel_status=SYSCALL_EINTR});
        if (rc!=HANDOFF_WAIT) break;
        if (nb) return SYSCALL_EAGAIN;
        sched_wait_until(&handoff_admission_channel,handoff_admissible,&sequence);
    }
    if (rc) return SYSCALL_EIO;
    handoff_token_t t=self->handoff.token;
    /* No signal/wait boundary until the retained reference is registered. */
    bool got=kind==NET_OWNER_UDP_CREATE || vfs_file_get(file);
    uint64_t identity=got && kind!=NET_OWNER_UDP_CREATE ? net_socket_identity(file) : 0;
    uint64_t f=spin_lock_irqsave(&lock);
    values[t.slot]=(udp_request_t){.generation=t.generation,.file=got ? file : NULL,
        .identity=identity,.floor=floor,.address=a ? *a : (net_sockaddr_in_t){0},
        .length=len,.nonblocking=nb,.slab=-1};
    spin_unlock_irqrestore(&lock,f);
    if (!got) { net_udp_abandon(); return SYSCALL_EBADF; }
    if (kind==NET_OWNER_UDP_CREATE) {
        file_t *prepared=NULL;
        int64_t result=net_socket_prepare(&prepared);
        f=spin_lock_irqsave(&lock); values[t.slot].file=prepared; spin_unlock_irqrestore(&lock,f);
        /* Owner checks endpoint capacity before reporting allocation failure,
         * preserving ENOSPC ahead of ENOMEM without caller-side reservation. */
        (void)result;
    }
    for (;;) {
        if (process_signal_interrupt()) { net_udp_abandon(); return SYSCALL_EINTR; }
        uint64_t sequence=handoff_admission_snapshot();
        rc=handoff_publish(t);
        if (rc!=HANDOFF_WAIT) break;
        if (nb) { net_udp_abandon(); return SYSCALL_EAGAIN; }
        sched_wait_until(&handoff_admission_channel,handoff_admissible,&sequence);
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
void net_udp_step(handoff_token_t t,handoff_request_t r) {
    net_control_assert_owner();
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
        if (!handoff_inspect(&t,&r,&st,&caller) || r.kind<NET_OWNER_UDP_CREATE || r.kind>NET_OWNER_UDP_RECV) continue;
        if (st!=HANDOFF_COMPLETED || caller) continue;
        udp_request_t *v=&values[t.slot];
        cleanup(v,false);
        uint64_t f=spin_lock_irqsave(&lock);
        if (v->pin) __builtin_trap();
        slab_put(v); file_t *file=v->file; v->file=NULL;
        spin_unlock_irqrestore(&lock,f);
        /* Final put invokes only detach publication; no lock is held. */
        if (file) vfs_close(file);
        handoff_cleaned(t);
    }
}
