#include "net_ping.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
#include "spinlock.h"
#include "thread.h"
#include "apic.h"

const char g_net_ping_channel=0;
static spinlock_t s_lock=SPINLOCK_RANKED(1,"net_ping");
static net_ping_v1_t s_request, s_result;
static uint64_t s_generation, s_active, s_done, s_expire;
static bool s_available, s_submitted, s_cancelled, s_used;
static net_trace_v1_t s_trace_request, s_trace_result;
static bool s_trace_mode;
static int64_t s_trace_error;
static uint32_t s_trace_identity; /* Never reset/reuse during this boot. */
#ifdef NET_TRACE_HOST_TEST
bool net_trace_test_identity(uint32_t value) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    bool idle=!s_used;
    if (idle) s_trace_identity=value;
    spin_unlock_irqrestore(&s_lock,irq); return idle;
}
#endif

void net_ping_init(bool available) {
    s_available=available; s_used=s_submitted=s_cancelled=false;
    s_trace_mode=false;
    __atomic_store_n(&s_active,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_done,0,__ATOMIC_RELEASE);
}
int64_t net_ping_submit(const net_ping_v1_t *request, uint64_t *token) {
    /* Local config is immutable; no static scratch touched by this validation. */
    if (request->version!=1 || request->timeout_seconds<1 || request->timeout_seconds>5 ||
        request->sequence>65535 || request->reserved || request->outcome || request->echoed_bytes ||
        request->rtt_ticks || request->tick_hz ||
        (request->start_delay_ms && request->start_delay_ms!=1000)) return SYSCALL_EINVAL;
    if (!s_available || !apic_timer_get_frequency()) return SYSCALL_EIO;
    if (!net_ipv4_unicast(request->destination)) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (s_used) { spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_EAGAIN; }
    if (++s_generation==0) ++s_generation;
    *token=s_generation; s_request=*request; s_trace_mode=false;
    s_expire=apic_timer_get_bsp_ticks()+(uint64_t)(request->timeout_seconds+6)*apic_timer_get_frequency();
    s_used=s_submitted=true; s_cancelled=false;
    __atomic_store_n(&s_done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_active,*token,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_lock,irq);
    return 0;
}
int64_t net_trace_submit(const net_trace_v1_t *request, uint64_t *token) {
    uint64_t hz=apic_timer_get_frequency(), now=apic_timer_get_bsp_ticks();
    if (request->version!=1 || request->ttl<1 || request->ttl>30 || request->timeout_seconds<1 ||
        request->timeout_seconds>5 || request->sequence>65535 || request->reserved0 || request->reserved1 ||
        request->outcome || request->responder || request->icmp_type || request->icmp_code ||
        request->rtt_ticks || request->tick_hz || !net_ipv4_unicast(request->destination) ||
        (hz && (hz>UINT64_MAX/(NETTRACE_HORIZON_SECONDS+2) ||
        (request->deadline_ticks>now && request->deadline_ticks-now>NETTRACE_HORIZON_SECONDS*hz))))
        return SYSCALL_EINVAL;
    if (!s_available || !hz) return SYSCALL_EIO;
    if (request->deadline_ticks<=now) return SYSCALL_ETIMEDOUT;
    if (request->deadline_ticks>UINT64_MAX-2*hz) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (s_used || s_trace_identity==UINT32_MAX) {
        spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_EAGAIN;
    }
    if (++s_generation==0) ++s_generation;
    ++s_trace_identity; *token=s_generation;
    s_trace_request=*request; s_trace_error=0; s_trace_mode=true;
    s_expire=request->deadline_ticks+2*hz;
    s_used=s_submitted=true; s_cancelled=false;
    __atomic_store_n(&s_done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_active,*token,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_lock,irq); return 0;
}
int64_t net_trace_collect(uint64_t token, net_trace_v1_t *result) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    int64_t ret=0;
    if (!s_used || s_active!=token || !s_trace_mode) ret=SYSCALL_EINTR;
    else if (s_done!=token) ret=SYSCALL_EAGAIN;
    else {
        ret=s_trace_error;
        if (!ret) *result=s_trace_result;
        s_used=false; __atomic_store_n(&s_active,0,__ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&s_lock,irq); return ret;
}
bool net_ping_ready(void *token) {
    uint64_t t=*(uint64_t *)token;
    return __atomic_load_n(&s_active,__ATOMIC_ACQUIRE)!=t ||
        __atomic_load_n(&s_done,__ATOMIC_ACQUIRE)==t;
}
int64_t net_ping_collect(uint64_t token, net_ping_v1_t *result) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    int64_t ret=0;
    if (!s_used || s_active!=token || s_trace_mode) ret=SYSCALL_EINTR;
    else if (s_done!=token) ret=SYSCALL_EAGAIN;
    else {
        *result=s_result; s_used=false;
        __atomic_store_n(&s_active,0,__ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&s_lock,irq); return ret;
}
void net_ping_cancel(uint64_t token) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (s_used && s_active==token) s_cancelled=true;
    spin_unlock_irqrestore(&s_lock,irq);
}
void net_ping_worker_tick(uint64_t now) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (!s_used) { spin_unlock_irqrestore(&s_lock,irq); return; }
    uint64_t token=s_active;
    bool trace_mode=s_trace_mode;
    if (s_cancelled || now>=s_expire) {
        s_used=s_submitted=s_cancelled=false;
        __atomic_store_n(&s_active,0,__ATOMIC_RELEASE);
        spin_unlock_irqrestore(&s_lock,irq);
        if (trace_mode) net_ipv4_trace_cancel(token); else net_ipv4_ping_cancel(token);
        sched_wake_all(&g_net_ping_channel); return;
    }
    bool submit=s_submitted; s_submitted=false;
    net_ping_v1_t copy=s_request;
    net_trace_v1_t trace=s_trace_request;
    uint32_t identity=s_trace_identity;
    bool completed=s_done==token;
    spin_unlock_irqrestore(&s_lock,irq);
    if (completed) return;
    if (trace_mode) {
        int64_t error=0;
        if (submit && !net_ipv4_trace_start(&trace,token,identity,now)) {
            trace.outcome=NETTRACE_TX_FAILED; trace.tick_hz=apic_timer_get_frequency();
        } else if (!net_ipv4_trace_take(&trace,&error)) return;
        irq=spin_lock_irqsave(&s_lock);
        bool publish=s_used && s_active==token && s_trace_mode && !s_cancelled;
        if (publish) {
            s_trace_result=trace; s_trace_error=error;
            /* Result grace starts on publication, not on the command deadline. */
            uint64_t grace=2*apic_timer_get_frequency();
            s_expire=grace>UINT64_MAX-now ? UINT64_MAX:now+grace;
            __atomic_store_n(&s_done,token,__ATOMIC_RELEASE);
        }
        spin_unlock_irqrestore(&s_lock,irq);
        if (publish) sched_wake_all(&g_net_ping_channel);
        return;
    }
    if (submit && !net_ipv4_ping_start(&copy,token,now)) {
        copy.outcome=NETPING_TX_FAILED; copy.tick_hz=apic_timer_get_frequency();
    } else if (!net_ipv4_ping_take(&copy)) return;
    irq=spin_lock_irqsave(&s_lock);
    bool publish=s_used && s_active==token && !s_cancelled;
    if (publish) {
        s_result=copy;
        __atomic_store_n(&s_done,token,__ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&s_lock,irq);
    if (publish) sched_wake_all(&g_net_ping_channel);
}
