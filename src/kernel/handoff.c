#include "handoff.h"
#include "spinlock.h"
#include "thread.h"

typedef struct {
    uint64_t generation, ticket;
    unsigned state;
    bool caller, cancel;
    handoff_request_t request;
    handoff_reply_t reply;
    unsigned channel;
} request_slot_t;
static request_slot_t slots[HANDOFF_SLOTS];
static spinlock_t lock=SPINLOCK_RANKED(1,"handoff");
static uint64_t next_ticket, reclaimed;
static unsigned queued, data, claim_cursor, pending_cursor, ack_cursor;
static bool online, started;
static bool prefer_control=true;
/* Sequence is a level-triggered owner hint. Clear under the same lock as
 * producers, then scan. Pending steps explicitly republish their work. */
static bool work;
static uint64_t admission_sequence;
const unsigned handoff_owner_channel=0, handoff_admission_channel=0;
_Static_assert(sizeof(request_slot_t)<=512,"request ledger");
_Static_assert(sizeof(slots)+4096<=32768+4096,"Phase 1 static ledger");
_Static_assert(sizeof(handoff_registration_t)<=32,"registration ledger");

static unsigned state(request_slot_t *s) { return __atomic_load_n(&s->state,__ATOMIC_ACQUIRE); }
static void set_state(request_slot_t *s,unsigned v) { __atomic_store_n(&s->state,v,__ATOMIC_RELEASE); }
static bool matches(handoff_token_t t) {
    return t.slot<HANDOFF_SLOTS && t.generation &&
        __atomic_load_n(&slots[t.slot].generation,__ATOMIC_ACQUIRE)==t.generation &&
        state(&slots[t.slot])!=HANDOFF_FREE && state(&slots[t.slot])!=HANDOFF_RETIRED;
}
static void hint(void) { __atomic_store_n(&work,true,__ATOMIC_RELEASE); }
static void wake(void) {
    sched_wake_all(&handoff_owner_channel);
    sched_wake_all(&handoff_admission_channel);
}
void handoff_notify(void) {
    uint64_t f=spin_lock_irqsave(&lock);
    hint();
    spin_unlock_irqrestore(&lock,f);
    wake();
}
static void drop_credit(request_slot_t *s) {
    --queued;
    if (!s->request.control) --data;
    __atomic_add_fetch(&admission_sequence,1,__ATOMIC_RELEASE);
}
static bool eligible(request_slot_t *s) {
    if (queued>=HANDOFF_CREDITS || (!s->request.control && data>=HANDOFF_DATA_CREDITS)) return false;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i)
        if (state(&slots[i])==HANDOFF_PREPARED && slots[i].ticket && slots[i].caller && !slots[i].cancel &&
            slots[i].request.control==s->request.control && slots[i].ticket<s->ticket) return false;
    return true;
}
void handoff_start(void) {
    /* Static zero initialization is intentional. No reinitialization API. */
    uint64_t f=spin_lock_irqsave(&lock);
    if (!started) { online=true; started=true; }
    spin_unlock_irqrestore(&lock,f);
    wake();
}
void handoff_fail(void) {
    uint64_t f=spin_lock_irqsave(&lock);
    online=false;
    __atomic_add_fetch(&admission_sequence,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        unsigned st=state(&slots[i]);
        if (st!=HANDOFF_FREE && st!=HANDOFF_RETIRED && st!=HANDOFF_COMPLETED) slots[i].cancel=true;
    }
    hint();
    spin_unlock_irqrestore(&lock,f);
    wake();
}
int handoff_begin(handoff_registration_t *r,handoff_request_t req) {
    if (!r || r->active || !req.actor) return HANDOFF_STALE;
    uint64_t f=spin_lock_irqsave(&lock);
    int result=HANDOFF_WAIT;
    if (!online) result=HANDOFF_UNAVAILABLE;
    else if (next_ticket==UINT64_MAX) result=HANDOFF_EXHAUSTED;
    else {
        for (unsigned i=0;i<HANDOFF_SLOTS;++i)
            if (state(&slots[i])!=HANDOFF_FREE && state(&slots[i])!=HANDOFF_RETIRED && slots[i].request.actor==req.actor) {
                spin_unlock_irqrestore(&lock,f); return HANDOFF_WAIT;
            }
        for (unsigned i=0;i<HANDOFF_SLOTS;++i) if (state(&slots[i])==HANDOFF_FREE) {
            request_slot_t *s=&slots[i];
            if (!s->generation) __atomic_store_n(&s->generation,1,__ATOMIC_RELEASE);
            s->request=req; s->ticket=0; s->caller=true; s->cancel=false;
            s->reply=(handoff_reply_t){0};
            r->token=(handoff_token_t){.generation=s->generation,.slot=i}; r->active=true;
            set_state(s,HANDOFF_PREPARED); result=HANDOFF_OK; break;
        }
    }
    spin_unlock_irqrestore(&lock,f);
    return result;
}
int handoff_publish(handoff_token_t t) {
    uint64_t f=spin_lock_irqsave(&lock);
    int result=HANDOFF_STALE;
    if (matches(t)) {
        request_slot_t *s=&slots[t.slot];
        if (state(s)==HANDOFF_PREPARED && s->caller && !s->cancel) {
            result=HANDOFF_WAIT;
            if (!s->ticket && next_ticket==UINT64_MAX) result=HANDOFF_EXHAUSTED;
            else if (online) {
                if (!s->ticket) s->ticket=++next_ticket;
                hint();
            }
            if (result!=HANDOFF_EXHAUSTED && online && eligible(s)) {
                ++queued; if (!s->request.control) ++data;
                set_state(s,HANDOFF_QUEUED); hint(); result=HANDOFF_OK;
            } else if (!online) result=HANDOFF_UNAVAILABLE;
        } else if (s->ticket && s->caller) result=HANDOFF_OK;
    }
    spin_unlock_irqrestore(&lock,f);
    if (result==HANDOFF_OK || result==HANDOFF_WAIT) wake();
    return result;
}
bool handoff_claim(handoff_token_t *t,handoff_request_t *req) {
    uint64_t f=spin_lock_irqsave(&lock);
    unsigned best=HANDOFF_SLOTS;
    unsigned control=HANDOFF_SLOTS, ordinary=HANDOFF_SLOTS;
    /* Alternate eligible classes, FIFO within each: neither an unanswered data
     * operation nor a continuous control producer may starve the other class. */
    for (unsigned j=0;j<HANDOFF_SLOTS;++j) {
        unsigned i=(claim_cursor+j)%HANDOFF_SLOTS;
        if (state(&slots[i])!=HANDOFF_QUEUED || slots[i].cancel) continue;
        unsigned *candidate=slots[i].request.control ? &control : &ordinary;
        if (*candidate==HANDOFF_SLOTS || slots[i].ticket<slots[*candidate].ticket) *candidate=i;
    }
    if (control<HANDOFF_SLOTS && ordinary<HANDOFF_SLOTS) best=prefer_control ? control : ordinary;
    else best=control<HANDOFF_SLOTS ? control : ordinary;
    bool found=best<HANDOFF_SLOTS;
    if (found) {
        request_slot_t *s=&slots[best]; drop_credit(s); set_state(s,HANDOFF_CLAIMED);
        *t=(handoff_token_t){.slot=best,.generation=s->generation}; *req=s->request;
        claim_cursor=(best+1)%HANDOFF_SLOTS;
        prefer_control=!s->request.control;
    }
    spin_unlock_irqrestore(&lock,f);
    if (found) sched_wake_all(&handoff_admission_channel);
    return found;
}
bool handoff_pending(handoff_token_t *t,handoff_request_t *req) {
    uint64_t f=spin_lock_irqsave(&lock);
    bool found=false;
    for (unsigned n=0;n<HANDOFF_SLOTS;++n) {
        unsigned i=pending_cursor++%HANDOFF_SLOTS;
        if (state(&slots[i])!=HANDOFF_PENDING) continue;
        set_state(&slots[i],HANDOFF_CLAIMED);
        *t=(handoff_token_t){.slot=i,.generation=slots[i].generation}; *req=slots[i].request;
        found=true; break;
    }
    spin_unlock_irqrestore(&lock,f); return found;
}
bool handoff_cancelled(handoff_token_t t) {
    uint64_t f=spin_lock_irqsave(&lock);
    bool result=!matches(t) || slots[t.slot].cancel;
    spin_unlock_irqrestore(&lock,f); return result;
}
bool handoff_finish(handoff_token_t t,handoff_reply_t reply,bool pending) {
    uint64_t f=spin_lock_irqsave(&lock);
    bool ok=matches(t) && state(&slots[t.slot])==HANDOFF_CLAIMED;
    if (ok) {
        request_slot_t *s=&slots[t.slot];
        if (pending) { set_state(s,HANDOFF_PENDING); if (s->cancel) hint(); }
        else { s->reply=reply; set_state(s,HANDOFF_COMPLETED); hint(); }
    }
    spin_unlock_irqrestore(&lock,f);
    if (ok) { sched_wake_all(handoff_channel(t)); wake(); }
    return ok;
}
void handoff_cancel(handoff_token_t t) {
    uint64_t f=spin_lock_irqsave(&lock);
    if (matches(t)) { slots[t.slot].cancel=true; hint(); }
    spin_unlock_irqrestore(&lock,f); wake();
}
void handoff_release(handoff_registration_t *r) {
    if (!r || !r->active) return;
    uint64_t f=spin_lock_irqsave(&lock);
    if (matches(r->token)) {
        request_slot_t *s=&slots[r->token.slot];
        s->caller=false;
        if (state(s)!=HANDOFF_COMPLETED) s->cancel=true;
        hint();
    }
    r->active=false;
    spin_unlock_irqrestore(&lock,f); wake();
}
void handoff_abandon(handoff_registration_t *r) { handoff_release(r); }
int handoff_collect(handoff_token_t t,handoff_reply_t *reply) {
    uint64_t f=spin_lock_irqsave(&lock);
    int result=HANDOFF_STALE;
    if (matches(t) && slots[t.slot].caller) {
        result=HANDOFF_WAIT;
        if (state(&slots[t.slot])==HANDOFF_COMPLETED) { *reply=slots[t.slot].reply; result=HANDOFF_OK; }
    }
    spin_unlock_irqrestore(&lock,f); return result;
}
unsigned handoff_acknowledge(void) {
    const void *channels[8]; unsigned count=0;
    uint64_t f=spin_lock_irqsave(&lock);
    __atomic_store_n(&work,false,__ATOMIC_RELEASE);
    for (unsigned n=0;n<8;++n) {
        request_slot_t *s=&slots[ack_cursor++%HANDOFF_SLOTS];
        unsigned st=state(s);
        /* PENDING may own subsystem reservations: only its typed owner step
         * may acknowledge cancellation after cleanup. Never auto-reclaim it. */
        if ((st==HANDOFF_PREPARED || st==HANDOFF_QUEUED) && s->cancel) {
            if (st==HANDOFF_QUEUED) drop_credit(s);
            s->reply=(handoff_reply_t){.status=s->request.cancel_status ? s->request.cancel_status : -4};
            set_state(s,HANDOFF_COMPLETED);
            channels[count++]=&s->channel;
        }
        if (state(s)==HANDOFF_COMPLETED && !s->caller) {
            ++reclaimed;
            if (s->generation==UINT64_MAX) set_state(s,HANDOFF_RETIRED);
            else {
                __atomic_add_fetch(&s->generation,1,__ATOMIC_RELEASE);
                set_state(s,HANDOFF_FREE);
            }
            __atomic_add_fetch(&admission_sequence,1,__ATOMIC_RELEASE);
        }
    }
    /* Offered callers need not run to receive their FIFO grant: STOP between
     * offer and admission must not obstruct the class behind them. */
    for (unsigned n=0;n<8 && online;++n) {
        unsigned best=HANDOFF_SLOTS;
        for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
            request_slot_t *s=&slots[i];
            if (state(s)!=HANDOFF_PREPARED || !s->ticket || !s->caller || s->cancel || !eligible(s)) continue;
            if (best==HANDOFF_SLOTS || (s->request.control && !slots[best].request.control) ||
                (s->request.control==slots[best].request.control && s->ticket<slots[best].ticket)) best=i;
        }
        if (best==HANDOFF_SLOTS) break;
        request_slot_t *s=&slots[best]; ++queued; if (!s->request.control) ++data;
        set_state(s,HANDOFF_QUEUED); __atomic_add_fetch(&admission_sequence,1,__ATOMIC_RELEASE);
    }
    /* Scan bounded static records to preserve a level hint across partial acks.
     * Completed interested callers are deliberately not runnable owner work. */
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        unsigned st=state(&slots[i]);
        if (st==HANDOFF_QUEUED || (st==HANDOFF_PENDING && slots[i].cancel) ||
            ((st==HANDOFF_PREPARED && (slots[i].cancel || slots[i].ticket)) ||
             (st==HANDOFF_COMPLETED && !slots[i].caller))) hint();
    }
    spin_unlock_irqrestore(&lock,f);
    for (unsigned i=0;i<count;++i) sched_wake_all(channels[i]);
    sched_wake_all(&handoff_admission_channel);
    return count;
}
handoff_stats_t handoff_stats(void) {
    handoff_stats_t v={0};
    uint64_t f=spin_lock_irqsave(&lock);
    v.queued=queued; v.data=data; v.reclaimed=reclaimed;
    for (unsigned i=0;i<HANDOFF_SLOTS;++i) {
        unsigned st=state(&slots[i]);
        v.live+=st!=HANDOFF_FREE && st!=HANDOFF_RETIRED;
        v.pending+=st==HANDOFF_PENDING; v.retired+=st==HANDOFF_RETIRED;
    }
    spin_unlock_irqrestore(&lock,f); return v;
}
bool handoff_ready(void *arg) {
    handoff_token_t t=*(handoff_token_t *)arg;
    return !matches(t) || state(&slots[t.slot])==HANDOFF_COMPLETED;
}
uint64_t handoff_admission_snapshot(void) { return __atomic_load_n(&admission_sequence,__ATOMIC_ACQUIRE); }
bool handoff_admissible(void *arg) {
    /* Wake sequence cannot prove an actual grant. Caller retries under lock.
     * Predicates must not inspect mutable non-atomic ticket/request fields. */
    uint64_t *sequence=arg;
    return __atomic_load_n(&admission_sequence,__ATOMIC_ACQUIRE)!=*sequence;
}
bool handoff_owner_ready(void *unused) { (void)unused; return __atomic_load_n(&work,__ATOMIC_ACQUIRE); }
const void *handoff_channel(handoff_token_t t) { return t.slot<HANDOFF_SLOTS ? &slots[t.slot].channel : &handoff_admission_channel; }
#ifdef TEST_SMP_MEMORY
void handoff_test_exhaust(unsigned i,uint64_t generation,uint64_t ticket) {
    uint64_t f=spin_lock_irqsave(&lock);
    if (i<HANDOFF_SLOTS && state(&slots[i])==HANDOFF_FREE) {
        __atomic_store_n(&slots[i].generation,generation,__ATOMIC_RELEASE); next_ticket=ticket;
    }
    spin_unlock_irqrestore(&lock,f);
}
#endif
