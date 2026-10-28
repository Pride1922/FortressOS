#include "net_control.h"
#include "net.h"
#include "net_ping.h"
#include "net_udp_owner.h"
#include "thread.h"
#include "apic.h"
#include "syscall_abi.h"
#include "string.h"
#if !defined(TEST_SMP_MEMORY) || defined(WIFI_OWNER_HOST)
#include "net_wifi_owner.h"
#endif
#ifndef TEST_SMP_MEMORY
#include "percpu.h"
#endif

typedef struct {
    net_control_value_t input, output;
    uint64_t probe;
} control_slot_t;
static control_slot_t values[HANDOFF_SLOTS];
static uint64_t owner_tid;
static uint64_t config_epoch;
void net_control_register_owner(void) {
#ifndef TEST_SMP_MEMORY
    if (cpu_current()->id) __builtin_trap();
#endif
    owner_tid=thread_current()->tid;
}
_Static_assert(sizeof(values)<=8192u+4096u,"Phase 2A value ledger");

void net_control_start(void) { handoff_start(); }

/* Signal handling can park STOP until CONT. Never wait for owner cancellation
 * while a caught/terminating signal keeps the scheduler predicate runnable. */
static int64_t interrupted(handoff_registration_t *r) {
    handoff_abandon(r);
    return SYSCALL_EINTR;
}
typedef struct { uint64_t sequence, deadline, floor; } admission_wait_t;
static bool admission_ready(void *arg) {
    admission_wait_t *w=arg;
    uint64_t now=apic_timer_get_bsp_ticks();
    return handoff_admissible(&w->sequence) ||
        now<w->floor || (w->deadline && now>=w->deadline);
}
int64_t net_control_call(unsigned kind, net_control_value_t *value) {
    tcb_t *self=thread_current();
    if (!self) return SYSCALL_EIO;
    handoff_registration_t *r=&self->handoff;
    uint64_t admitted=apic_timer_get_bsp_ticks();
    uint64_t deadline=kind==NETCTL_TRACE_PROBE ? value->trace.deadline_ticks : 0;
    int rc;
    for (;;) {
        if (process_signal_interrupt()) return interrupted(r);
        if (apic_timer_get_bsp_ticks()<admitted) return SYSCALL_EIO;
        if (deadline && apic_timer_get_bsp_ticks()>=deadline) return SYSCALL_ETIMEDOUT;
        admission_wait_t w={handoff_admission_snapshot(),deadline,admitted};
        rc=handoff_begin(r,(handoff_request_t){.actor=self->tid,.value=admitted,.kind=kind,.control=true,.deadline=deadline,
            .cancel_status=SYSCALL_EINTR});
        if (rc!=HANDOFF_WAIT) break;
        sched_wait_until(&handoff_admission_channel,admission_ready,&w);
    }
    if (rc) return SYSCALL_EIO;
    handoff_token_t t=r->token;
    /* PREPARED is invisible to owner operations. Publish synchronizes these
     * fixed copied inputs. No sleep/signal check during preparation or copy. */
    values[t.slot]=(control_slot_t){.input=*value};
    for (;;) {
        if (process_signal_interrupt()) return interrupted(r);
        if (apic_timer_get_bsp_ticks()<admitted) { handoff_abandon(r); return SYSCALL_EIO; }
        if (deadline && apic_timer_get_bsp_ticks()>=deadline) {
            handoff_abandon(r); return SYSCALL_ETIMEDOUT;
        }
        admission_wait_t w={handoff_admission_snapshot(),deadline,admitted};
        rc=handoff_publish(t);
        if (rc!=HANDOFF_WAIT) break;
        sched_wait_until(&handoff_admission_channel,admission_ready,&w);
    }
    if (rc) { handoff_abandon(r); return SYSCALL_EIO; }
    while (!handoff_ready(&t)) {
        if (process_signal_interrupt()) return interrupted(r);
        sched_wait_until(handoff_channel(t),handoff_ready,&t);
    }
    if (process_signal_interrupt()) return interrupted(r);
    handoff_reply_t reply;
    rc=handoff_collect(t,&reply);
    if (!rc && !reply.status) *value=values[t.slot].output;
    handoff_release(r);
    return rc ? SYSCALL_EIO : reply.status;
}

static void step(handoff_token_t t,handoff_request_t r) {
#if !defined(TEST_SMP_MEMORY) || defined(WIFI_OWNER_HOST)
    if (r.kind>=NET_OWNER_WIFI && r.kind<=NET_OWNER_WIFI_LAST) {
        net_wifi_step(t,r); return;
    }
#endif
    if (r.kind>=NET_OWNER_UDP_CREATE && r.kind<=NET_OWNER_TCP_ACCEPT) {
        net_udp_step(t,r); return;
    }
    control_slot_t *v=&values[t.slot];
    int64_t result=0;
    bool pending=false;
    if (handoff_cancelled(t) || apic_timer_get_bsp_ticks()<r.value) {
        if (v->probe) {
            net_ping_cancel(v->probe);
            /* Typed owner cleanup runs before terminal generic publication. */
            net_ping_worker_tick(apic_timer_get_bsp_ticks());
            v->probe=0;
        }
        result=handoff_cancelled(t) ? SYSCALL_EINTR : SYSCALL_EIO;
    } else if (r.kind==NETCTL_IFGET) result=net_get_ifconfig(&v->output.get);
    else if (r.kind==NETCTL_IFSET) {
        net_config_t config;
        result=net_validate_ifset(&v->input.set,&config);
        if (!result && config_epoch==UINT64_MAX) result=SYSCALL_EIO;
        if (!result) { net_set_config(&config); ++config_epoch; }
    } else if (r.kind==NETCTL_PING || r.kind==NETCTL_TRACE_PROBE) {
        if (!v->probe) result=r.kind==NETCTL_PING ?
            net_ping_submit(&v->input.ping,&v->probe) : net_trace_submit(&v->input.trace,&v->probe);
        if (!result) {
            if (!net_ping_ready(&v->probe)) pending=true;
            else {
                result=r.kind==NETCTL_PING ? net_ping_collect(v->probe,&v->output.ping) :
                    net_trace_collect(v->probe,&v->output.trace);
                if (result) {
                    net_ping_cancel(v->probe);
                    net_ping_worker_tick(apic_timer_get_bsp_ticks());
                }
                v->probe=0;
            }
        }
    } else result=SYSCALL_EINVAL;
    handoff_finish(t,(handoff_reply_t){.status=result},pending);
}
void net_control_assert_owner(void) {
    if (thread_current()->tid!=owner_tid) __builtin_trap();
#ifndef TEST_SMP_MEMORY
    if (cpu_current()->id) __builtin_trap();
#endif
}
void net_control_service(void) {
    net_control_assert_owner();
    /* Acknowledgement cannot reclaim pending protocol state. Typed steps do. */
    handoff_acknowledge();
    net_udp_maintenance();
#if !defined(TEST_SMP_MEMORY) || defined(WIFI_OWNER_HOST)
    net_wifi_maintenance();
#endif
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
    net_udp_refresh_work();
}
