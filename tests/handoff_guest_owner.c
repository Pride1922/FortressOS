/* Copied into src/kernel ONLY in the disposable Phase 1 snapshot. */
#include "handoff.h"
#include "thread.h"
#include "percpu.h"
#include "serial.h"
#include "apic.h"
#include "smp.h"
static unsigned hold_open, frozen, arm_idle, idle_window;
static unsigned post_gate[HANDOFF_SLOTS], post_mode[HANDOFF_SLOTS], post_woken[HANDOFF_SLOTS];
unsigned handoff_fixture_frozen_ack;
static uint64_t owner_steps, post_wakes;
static uint64_t caller_mask, hold_completed;
static void note(const char *s,uint64_t value) {
    serial_puts(s); serial_print_hex(value); serial_puts("\n");
}
void handoff_fixture_woke(tcb_t *t,const void *channel) {
    /* Called by snapshot scheduler while owning its lock; no logging/locks. */
    if (t->handoff.active && channel==handoff_channel(t->handoff.token) &&
        __atomic_load_n(&t->context_busy,__ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&post_wakes,1,__ATOMIC_RELEASE);
        __atomic_store_n(&post_woken[t->handoff.token.slot],1,__ATOMIC_RELEASE);
    }
}
void handoff_fixture_switch(tcb_t *old,const void *channel) {
    if (!old->handoff.active || channel!=handoff_channel(old->handoff.token) ||
        !__atomic_load_n(&post_mode[old->handoff.token.slot],__ATOMIC_ACQUIRE)) return;
    unsigned slot=old->handoff.token.slot;
    __atomic_store_n(&post_gate[slot],1,__ATOMIC_RELEASE);
    handoff_notify();
    /* Force wake of BLOCKED, context_busy task BEFORE its stack save. AP only.
     * Bounded test rendezvous, never part of the transport or release scheduler. */
    uint64_t end=apic_timer_get_bsp_ticks()+1000;
    while (!__atomic_load_n(&post_woken[slot],__ATOMIC_ACQUIRE) && apic_timer_get_bsp_ticks()<end)
        __asm__ volatile("pause");
    if (!__atomic_load_n(&post_woken[slot],__ATOMIC_ACQUIRE)) __builtin_trap();
    __atomic_store_n(&post_mode[slot],0,__ATOMIC_RELEASE);
}
static void step(handoff_token_t t,handoff_request_t r) {
    if (cpu_current()->id) __builtin_trap();
    __atomic_add_fetch(&owner_steps,1,__ATOMIC_RELEASE);
    if (handoff_cancelled(t)) {
        handoff_finish(t,(handoff_reply_t){.status=-4},false); return;
    }
    bool pending=(r.kind==4 && !__atomic_load_n(&hold_open,__ATOMIC_ACQUIRE)) ||
        (r.kind==3 && !__atomic_load_n(&post_gate[t.slot],__ATOMIC_ACQUIRE));
    handoff_finish(t,(handoff_reply_t){.value=r.value^0xabc},pending);
}
static bool fixture_ready(void *arg) {
    return __atomic_load_n(&frozen,__ATOMIC_ACQUIRE) || __atomic_load_n(&arm_idle,__ATOMIC_ACQUIRE) || handoff_owner_ready(arg);
}
static void owner(void *arg) {
    (void)arg; note("[HANDOFF OWNER CPU] ",cpu_current()->id);
    for (;;) {
        if (__atomic_load_n(&frozen,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&handoff_fixture_frozen_ack,1,__ATOMIC_RELEASE);
            thread_yield(); continue;
        }
        __atomic_store_n(&handoff_fixture_frozen_ack,0,__ATOMIC_RELEASE);
        handoff_acknowledge();
        for (unsigned i=0;i<8;++i) {
            handoff_token_t t; handoff_request_t r;
            if (!handoff_claim(&t,&r)) break;
            step(t,r);
        }
        for (unsigned i=0;i<8;++i) {
            handoff_token_t t; handoff_request_t r;
            if (!handoff_pending(&t,&r)) break;
            step(t,r);
        }
        thread_yield();
        if (__atomic_exchange_n(&arm_idle,0,__ATOMIC_ACQ_REL)) {
            __atomic_store_n(&idle_window,1,__ATOMIC_RELEASE);
            note("[HANDOFF IDLE WINDOW] ",1);
            uint64_t end=apic_timer_get_bsp_ticks()+3000;
            while (!handoff_owner_ready(NULL) && apic_timer_get_bsp_ticks()<end) thread_yield();
            if (!handoff_owner_ready(NULL)) __builtin_trap();
            note("[HANDOFF PUBLISH BEFORE SLEEP] ",1);
            __atomic_store_n(&idle_window,0,__ATOMIC_RELEASE);
        }
        sched_wait_until(&handoff_owner_channel,fixture_ready,NULL);
    }
}
void handoff_fixture_start(void) {
    handoff_start();
    if (!thread_create_on_cpu(0,"handoff_inert",owner,NULL)) __builtin_trap();
}
static bool deadline(uint64_t end) { return apic_timer_get_bsp_ticks()>end; }
int64_t handoff_fixture_call(unsigned mode) {
    tcb_t *self=thread_current();
    __atomic_fetch_or(&caller_mask,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
    if (mode!=4) note("[HANDOFF ENTRY CPU] ",cpu_current()->id);
    uint64_t end=apic_timer_get_bsp_ticks()+6000;
    if (mode==12 || mode==13 || mode==14) {
        unsigned target=mode==12 ? 1 : 7;
        while (mode==14 ? __atomic_load_n(&hold_completed,__ATOMIC_ACQUIRE)!=8 : handoff_stats().live!=target) {
            if (deadline(end)) return -1;
            thread_yield();
        }
        return 0;
    }
    if (mode==11) {
        __atomic_store_n(&arm_idle,1,__ATOMIC_RELEASE); sched_wake_all(&handoff_owner_channel);
        while (!__atomic_load_n(&idle_window,__ATOMIC_ACQUIRE)) {
            if (deadline(end)) return -1;
            thread_yield();
        }
        return 0;
    }
    if (mode==8) { __atomic_store_n(&hold_open,1,__ATOMIC_RELEASE); handoff_notify(); return 0; }
    if (mode==9) { __atomic_store_n(&hold_open,0,__ATOMIC_RELEASE); return 0; }
    if (mode==6) {
        while (handoff_stats().live) { if (deadline(end)) return -1; thread_yield(); }
        note("[HANDOFF RECOVERED] ",handoff_stats().reclaimed);
        note("[HANDOFF WAKE BEFORE SAVE] ",__atomic_load_n(&post_wakes,__ATOMIC_ACQUIRE));
        note("[HANDOFF OWNER STEPS] ",__atomic_load_n(&owner_steps,__ATOMIC_ACQUIRE));
        note("[HANDOFF CALLER MASK] ",__atomic_load_n(&caller_mask,__ATOMIC_ACQUIRE));
        return 0;
    }
    if (mode==10) {
        handoff_registration_t regs[64]={0};
        __atomic_store_n(&frozen,1,__ATOMIC_RELEASE);
        sched_wake_all(&handoff_owner_channel);
        /* Owner is CPU 0 and may already be in a turn. Wait for it to enter
         * frozen state via an explicit acknowledgement added below. */
        extern unsigned handoff_fixture_frozen_ack;
        while (!__atomic_load_n(&handoff_fixture_frozen_ack,__ATOMIC_ACQUIRE)) {
            if (deadline(end)) return -1;
            thread_yield();
        }
        bool ok=true;
        for (unsigned i=0;i<64;++i) {
            int rc=handoff_begin(&regs[i],(handoff_request_t){.actor=0x100000+i,.control=i>=56});
            if (rc) { ok=false; break; }
            rc=handoff_publish(regs[i].token);
            if (rc!=(i<24 || i>=56 ? HANDOFF_OK : HANDOFF_WAIT)) ok=false;
        }
        handoff_stats_t s=handoff_stats();
        ok=ok && s.live==64 && s.queued==32 && s.data==24;
        note("[HANDOFF SATURATION LIVE] ",s.live);
        note("[HANDOFF SATURATION QUEUED] ",s.queued);
        for (unsigned i=0;i<64;++i) handoff_abandon(&regs[i]);
        __atomic_store_n(&frozen,0,__ATOMIC_RELEASE); sched_wake_all(&handoff_owner_channel);
        while (handoff_stats().live) { if (deadline(end)) return -1; thread_yield(); }
        return ok ? 0 : -1;
    }
    for (unsigned n=0;n<(mode==0 ? 24u : 1u);++n) {
        for (;;) {
            if (process_signal_interrupt()) {
                handoff_abandon(&self->handoff);
                return -4;
            }
            uint64_t seq=handoff_admission_snapshot();
            int rc=handoff_begin(&self->handoff,(handoff_request_t){.actor=self->tid,.value=n,.kind=mode,.control=(self->tid%4)==0});
            if (!rc) break;
            if (rc!=HANDOFF_WAIT || deadline(end)) return -1;
            sched_wait_until(&handoff_admission_channel,handoff_admissible,&seq);
        }
        handoff_token_t t=self->handoff.token;
        __atomic_store_n(&post_gate[t.slot],0,__ATOMIC_RELEASE);
        __atomic_store_n(&post_woken[t.slot],0,__ATOMIC_RELEASE);
        __atomic_store_n(&post_mode[t.slot],mode==3,__ATOMIC_RELEASE);
        for (;;) {
            if (process_signal_interrupt()) {
                handoff_abandon(&self->handoff);
                return -4;
            }
            uint64_t seq=handoff_admission_snapshot();
            int rc=handoff_publish(t); if (!rc) break;
            if (rc!=HANDOFF_WAIT || deadline(end)) return -1;
            sched_wait_until(&handoff_admission_channel,handoff_admissible,&seq);
        }
        if (mode==2) {
            while (!handoff_ready(&t)) { if (deadline(end)) return -1; thread_yield(); }
            note("[HANDOFF COMPLETE BEFORE ENROLLMENT] ",1);
        }
        if (mode==7) handoff_cancel(t);
        while (!handoff_ready(&t)) {
            if (process_signal_interrupt()) {
                handoff_cancel(t);
                handoff_release(&self->handoff);
                return -4; /* Owner acknowledgement survives caller unwind. */
            }
            if (deadline(end)) return -1;
            sched_wait_until(handoff_channel(t),handoff_ready,&t);
        }
        handoff_reply_t reply;
        if (handoff_collect(t,&reply) || (reply.status!=-4 && (reply.status || reply.value!=(n^0xabc)))) return -1;
        handoff_release(&self->handoff);
    }
    if (mode==4) __atomic_add_fetch(&hold_completed,1,__ATOMIC_RELEASE);
    return 0;
}
