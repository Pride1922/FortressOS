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

void net_ping_init(bool available) {
    s_available=available; s_used=s_submitted=s_cancelled=false;
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
    *token=s_generation; s_request=*request;
    s_expire=apic_timer_get_bsp_ticks()+(uint64_t)(request->timeout_seconds+6)*apic_timer_get_frequency();
    s_used=s_submitted=true; s_cancelled=false;
    __atomic_store_n(&s_done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_active,*token,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_lock,irq);
    return 0;
}
bool net_ping_ready(void *token) {
    uint64_t t=*(uint64_t *)token;
    return __atomic_load_n(&s_active,__ATOMIC_ACQUIRE)!=t ||
        __atomic_load_n(&s_done,__ATOMIC_ACQUIRE)==t;
}
int64_t net_ping_collect(uint64_t token, net_ping_v1_t *result) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    int64_t ret=0;
    if (!s_used || s_active!=token) ret=SYSCALL_EINTR;
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
    if (s_cancelled || now>=s_expire) {
        s_used=s_submitted=s_cancelled=false;
        __atomic_store_n(&s_active,0,__ATOMIC_RELEASE);
        spin_unlock_irqrestore(&s_lock,irq);
        net_ipv4_ping_cancel(token);
        sched_wake_all(&g_net_ping_channel); return;
    }
    bool submit=s_submitted; s_submitted=false;
    net_ping_v1_t copy=s_request;
    bool completed=s_done==token;
    spin_unlock_irqrestore(&s_lock,irq);
    if (completed) return;
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
