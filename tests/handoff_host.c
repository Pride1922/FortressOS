#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include "handoff.h"
#include "thread.h"
void net_test_assert_unheld(void);
static pthread_mutex_t sleep_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static unsigned wakes;
void sched_wake_all(const void *channel) {
    (void)channel; net_test_assert_unheld();
    pthread_mutex_lock(&sleep_lock); ++wakes;
    pthread_cond_broadcast(&event); pthread_mutex_unlock(&sleep_lock);
}
void sched_wait_until(const void *channel,bool (*ready)(void *),void *arg) {
    (void)channel; net_test_assert_unheld();
    pthread_mutex_lock(&sleep_lock);
    while (!ready(arg)) pthread_cond_wait(&event,&sleep_lock);
    pthread_mutex_unlock(&sleep_lock);
}
static handoff_registration_t begin(uint64_t actor,bool control) {
    handoff_registration_t r={0};
    assert(handoff_begin(&r,(handoff_request_t){.actor=actor,.value=actor,.control=control})==0);
    return r;
}
static void drain(void) {
    for (unsigned i=0;i<16;++i) {
        handoff_acknowledge();
        for (unsigned j=0;j<8;++j) {
            handoff_token_t t; handoff_request_t req;
            if (!handoff_pending(&t,&req)) break;
            assert(handoff_cancelled(t));
            assert(handoff_finish(t,(handoff_reply_t){.status=-4},false));
        }
    }
}
static void empty(void) {
    drain(); handoff_stats_t s=handoff_stats();
    assert(!s.live && !s.queued && !s.data && !s.pending);
}
static void deterministic(void) {
    handoff_registration_t r={0}; handoff_reply_t reply;
    assert(handoff_begin(&r,(handoff_request_t){.actor=1})==HANDOFF_UNAVAILABLE);
    handoff_start();
    r=begin(1,false); handoff_token_t stale=r.token;
    handoff_cancel(r.token); drain();
    assert(handoff_collect(r.token,&reply)==0 && reply.status==-4);
    assert(handoff_publish(r.token)==HANDOFF_STALE);
    handoff_release(&r); handoff_abandon(&r); empty();
    assert(handoff_ready(&stale) && handoff_collect(stale,&reply)==HANDOFF_STALE);

    /* Cancel before claim has zero owner side effects. */
    r=begin(2,false); assert(handoff_publish(r.token)==0); handoff_cancel(r.token);
    handoff_token_t t; handoff_request_t req;
    assert(!handoff_claim(&t,&req)); drain();
    assert(handoff_collect(r.token,&reply)==0 && reply.status==-4);
    handoff_release(&r); empty();

    /* Claim beats cancellation: accepted positive progress is final. */
    r=begin(3,false); assert(handoff_publish(r.token)==0);
    assert(handoff_claim(&t,&req)); handoff_cancel(r.token);
    assert(handoff_cancelled(t));
    assert(handoff_finish(t,(handoff_reply_t){.status=7,.value=req.value},false));
    assert(!handoff_finish(t,(handoff_reply_t){0},false));
    assert(handoff_collect(r.token,&reply)==0 && reply.status==7 && reply.value==3);
    handoff_release(&r); empty();

    /* Owner retains claimed memory after caller death; no early recycling. */
    r=begin(4,false); assert(handoff_publish(r.token)==0); assert(handoff_claim(&t,&req));
    handoff_abandon(&r); drain(); assert(handoff_stats().live==1);
    assert(handoff_finish(t,(handoff_reply_t){.status=-4},false)); empty();
    r=begin(5,false); assert(handoff_publish(r.token)==0); assert(handoff_claim(&t,&req));
    assert(handoff_finish(t,(handoff_reply_t){0},true));
    for (unsigned i=0;i<8;++i) handoff_acknowledge();
    assert(!handoff_owner_ready(NULL)); /* Idle pending state must permit sleep. */
    handoff_notify(); assert(handoff_owner_ready(NULL));
    handoff_abandon(&r);
    for (unsigned i=0;i<8;++i) handoff_acknowledge();
    assert(handoff_stats().live==1 && handoff_stats().pending==1);
    empty();

    /* Full retained identities, data/control credits and per-class FIFO. */
    handoff_registration_t regs[64];
    for (unsigned i=0;i<64;++i) regs[i]=begin(100+i,i>=56);
    for (unsigned i=0;i<24;++i) assert(handoff_publish(regs[i].token)==0);
    assert(handoff_publish(regs[24].token)==HANDOFF_WAIT);
    for (unsigned i=56;i<64;++i) assert(handoff_publish(regs[i].token)==0);
    assert(handoff_stats().queued==32 && handoff_stats().data==24);
    handoff_registration_t extra={0};
    assert(handoff_begin(&extra,(handoff_request_t){.actor=999})==HANDOFF_WAIT);
    unsigned controls=0, ordinary=0;
    for (unsigned i=0;i<8;++i) {
        assert(handoff_claim(&t,&req));
        if (req.control) {
            assert(t.slot==regs[56+controls].token.slot); ++controls;
        } else {
            assert(t.slot==regs[ordinary].token.slot); ++ordinary;
        }
        assert(handoff_finish(t,(handoff_reply_t){0},true));
    }
    assert(controls==4 && ordinary==4);
    /* Cancel completed/pending/stopped/prepared in every record. */
    for (unsigned i=0;i<64;++i) handoff_abandon(&regs[i]);
    empty();
    assert(handoff_collect(stale,&reply)==HANDOFF_STALE);
    /* Two claims can complete in the opposite order without cross-delivery. */
    handoff_registration_t a=begin(800,false), b=begin(801,false);
    assert(handoff_publish(a.token)==0 && handoff_publish(b.token)==0);
    handoff_token_t ta,tb; handoff_request_t ra,rb;
    assert(handoff_claim(&ta,&ra) && handoff_claim(&tb,&rb));
    assert(handoff_finish(tb,(handoff_reply_t){.value=rb.value},false));
    assert(!handoff_ready(&ta) && handoff_ready(&tb));
    assert(handoff_finish(ta,(handoff_reply_t){.value=ra.value},false));
    assert(handoff_collect(a.token,&reply)==0 && reply.value==800);
    assert(handoff_collect(b.token,&reply)==0 && reply.value==801);
    handoff_release(&a); handoff_release(&b); empty();
}
static unsigned running;
static bool owner_ready(void *arg) {
    return !__atomic_load_n(&running,__ATOMIC_ACQUIRE) || handoff_owner_ready(arg);
}
static void *owner(void *arg) {
    (void)arg;
    while (__atomic_load_n(&running,__ATOMIC_ACQUIRE)) {
        handoff_acknowledge();
        for (unsigned i=0;i<8;++i) {
            handoff_token_t t; handoff_request_t req;
            if (!handoff_claim(&t,&req)) break;
            assert(handoff_finish(t,(handoff_reply_t){.value=req.value^0xabc},false));
        }
        sched_wait_until(&handoff_owner_channel,owner_ready,NULL);
    }
    drain(); return NULL;
}
static void *producer(void *arg) {
    uint64_t actor=(uintptr_t)arg;
    for (unsigned i=0;i<300;++i) {
        handoff_registration_t r={0};
        for (;;) {
            uint64_t seq=handoff_admission_snapshot();
            int rc=handoff_begin(&r,(handoff_request_t){.actor=actor,.value=i,.control=(actor&3)==0});
            if (!rc) break;
            assert(rc==HANDOFF_WAIT);
            sched_wait_until(&handoff_admission_channel,handoff_admissible,&seq);
        }
        for (;;) {
            uint64_t seq=handoff_admission_snapshot();
            int rc=handoff_publish(r.token); if (!rc) break;
            assert(rc==HANDOFF_WAIT);
            sched_wait_until(&handoff_admission_channel,handoff_admissible,&seq);
        }
        if (i%7==0) { handoff_abandon(&r); continue; }
        if (i%5==0) handoff_cancel(r.token);
        sched_wait_until(handoff_channel(r.token),handoff_ready,&r.token);
        handoff_reply_t reply;
        assert(handoff_collect(r.token,&reply)==0);
        assert(reply.status==-4 || (!reply.status && reply.value==(i^0xabc)));
        handoff_release(&r);
    }
    return NULL;
}
int main(void) {
    deterministic();
    pthread_t worker,producers[48];
    __atomic_store_n(&running,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<48;++i) assert(!pthread_create(&producers[i],NULL,producer,(void *)(uintptr_t)(1000+i)));
    /* Force real concurrent producers to fill all runnable credits before
     * owner startup; notification must survive the absence of an enrolled owner. */
    while (handoff_stats().queued!=HANDOFF_CREDITS) sched_yield();
    assert(!pthread_create(&worker,NULL,owner,NULL));
    for (unsigned i=0;i<48;++i) assert(!pthread_join(producers[i],NULL));
    __atomic_store_n(&running,0,__ATOMIC_RELEASE); sched_wake_all(&handoff_owner_channel);
    assert(!pthread_join(worker,NULL)); empty();
    handoff_test_exhaust(0,UINT64_MAX,0);
    handoff_registration_t r=begin(10000,false);
    handoff_abandon(&r); empty(); assert(handoff_stats().retired==1);
    handoff_test_exhaust(1,1,UINT64_MAX);
    assert(handoff_begin(&r,(handoff_request_t){.actor=10001})==HANDOFF_EXHAUSTED);
    handoff_test_exhaust(1,1,0); /* Isolate failure injection after exhaustion. */
    handoff_registration_t claimed=begin(11000,false), unclaimed=begin(11001,false);
    assert(handoff_publish(claimed.token)==0 && handoff_publish(unclaimed.token)==0);
    handoff_token_t t; handoff_request_t req;
    assert(handoff_claim(&t,&req));
    handoff_fail(); handoff_start();
    handoff_abandon(&claimed); handoff_abandon(&unclaimed);
    drain(); assert(handoff_stats().live==1); /* Failed owner storage quarantined. */
    assert(handoff_finish(t,(handoff_reply_t){.status=-4},false)); empty();
    assert(handoff_begin(&r,(handoff_request_t){.actor=10001})==HANDOFF_UNAVAILABLE);
    assert(handoff_stats().reclaimed==14474);
    printf("handoff PASS: deterministic claim/cancel/death/FIFO/credits/stale/exhaustion/failure; 14400 concurrent requests; reclaimed=%llu wakes=%u\n",
        (unsigned long long)handoff_stats().reclaimed,wakes);
}
