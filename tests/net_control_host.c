/* Actual handoff/control/mailbox/IPv4/config code. Device, timer, scheduler,
 * credentials and descriptor adapters are host-only; no real IRQ claim. */
#define main config_baseline_main
#define thread_current baseline_thread_current
#define sched_wait_until baseline_sched_wait_until
#define sched_wake_all baseline_sched_wake_all
#define process_signal_interrupt baseline_signal_interrupt
#define apic_timer_get_bsp_ticks baseline_ticks
#include "net_ifconfig_host.c"
#undef main
#undef thread_current
#undef sched_wait_until
#undef sched_wake_all
#undef process_signal_interrupt
#undef apic_timer_get_bsp_ticks
#include "net_control.h"
#include "net_ping.h"
#include "net_ipv4.h"
#include <pthread.h>
#include <time.h>
#include <sched.h>
static _Thread_local tcb_t task;
static _Thread_local bool signal_pending;
static pthread_mutex_t wait_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static uint64_t clock_ticks;
static unsigned stop_owner, started_producers, finished_producers, interrupted_calls;
static bool owner_open;
static unsigned owner_parked;
static unsigned reset_clock;
tcb_t *thread_current(void) { return &task; }
uint64_t apic_timer_get_bsp_ticks(void) { return __atomic_load_n(&clock_ticks,__ATOMIC_ACQUIRE); }
bool process_signal_interrupt(void) { return signal_pending; }
void sched_wake_all(const void *channel) {
    (void)channel;
    pthread_mutex_lock(&wait_lock); pthread_cond_broadcast(&changed); pthread_mutex_unlock(&wait_lock);
}
void sched_wait_until(const void *channel,bool (*ready)(void *),void *arg) {
    (void)channel;
    pthread_mutex_lock(&wait_lock);
    while (!ready(arg) && !signal_pending) {
        if (task.tid==5000 && handoff_stats().pending) { signal_pending=true; break; }
        pthread_cond_wait(&changed,&wait_lock);
    }
    pthread_mutex_unlock(&wait_lock);
}
static void *owner(void *unused) {
    (void)unused; task.tid=9999;
    net_control_register_owner();
    while (!__atomic_load_n(&stop_owner,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&wait_lock);
        while (!owner_open) {
            __atomic_store_n(&owner_parked,1,__ATOMIC_RELEASE);
            pthread_cond_wait(&changed,&wait_lock);
        }
        __atomic_store_n(&owner_parked,0,__ATOMIC_RELEASE);
        pthread_mutex_unlock(&wait_lock);
        __atomic_add_fetch(&clock_ticks,1,__ATOMIC_RELEASE);
        uint64_t now=apic_timer_get_bsp_ticks();
        net_ping_worker_tick(now); net_ipv4_tick(now); net_ping_worker_tick(now);
        if (__atomic_load_n(&reset_clock,__ATOMIC_ACQUIRE)) {
            if (!handoff_stats().queued) { sched_yield(); continue; }
            __atomic_store_n(&clock_ticks,0,__ATOMIC_RELEASE);
            __atomic_store_n(&reset_clock,0,__ATOMIC_RELEASE);
        }
        net_control_service();
        sched_wake_all(&handoff_admission_channel);
        sched_yield();
    }
    return NULL;
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg; task.tid=id+1;
    __atomic_add_fetch(&started_producers,1,__ATOMIC_RELEASE);
    for (unsigned n=0;n<200;++n) {
        net_control_value_t v={0}; int64_t rc;
        if ((n+id)%11==0) {
            v.set=(netctl_ifset_t){.struct_version=1,.local_ipv4=htonl(0x0a00020f+id),
                .netmask_ipv4=htonl(0xffffff00),.gateway_ipv4=htonl(0x0a000202)};
            rc=net_control_call(NETCTL_IFSET,&v); assert(!rc);
        } else if ((n+id)%13==0) {
            v.ping=(net_ping_v1_t){.version=1,.destination=htonl(0x0a000202),.timeout_seconds=1};
            assert(!net_ping_validate(&v.ping)); /* Concurrent subnet snapshot. */
            rc=net_control_call(NETCTL_PING,&v);
            assert(!rc || rc==SYSCALL_EAGAIN);
        } else {
            v.get.struct_version=1;
            rc=net_control_call(NETCTL_IFGET,&v); assert(!rc);
            assert(v.get.netmask_ipv4==htonl(0xffffff00) && v.get.gateway_ipv4==htonl(0x0a000202));
        }
        if (n==75) {
            signal_pending=true;
            rc=net_control_call(NETCTL_IFGET,&v); assert(rc==SYSCALL_EINTR);
            __atomic_add_fetch(&interrupted_calls,1,__ATOMIC_RELEASE);
            signal_pending=false;
        }
    }
    __atomic_add_fetch(&finished_producers,1,__ATOMIC_RELEASE);
    return NULL;
}
int main(void) {
    assert(!config_baseline_main());
    net_config_t cfg; net_parse_config(NULL,0,&cfg); net_init(&dev,&cfg);
    net_ping_init(true); net_control_start();
    pthread_t worker, callers[48];
    assert(!pthread_create(&worker,NULL,owner,NULL));
    for (unsigned i=0;i<48;++i) assert(!pthread_create(&callers[i],NULL,producer,(void *)(uintptr_t)i));
    while (__atomic_load_n(&started_producers,__ATOMIC_ACQUIRE)!=48) sched_yield();
    while (handoff_stats().live!=48) sched_yield();
    assert(handoff_stats().queued==32); /* Offered records beyond execution credit. */
    pthread_mutex_lock(&wait_lock); owner_open=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&wait_lock);
    for (unsigned i=0;i<48;++i) assert(!pthread_join(callers[i],NULL));
    while (handoff_stats().live) sched_yield();
    assert(__atomic_load_n(&finished_producers,__ATOMIC_ACQUIRE)==48);
    assert(__atomic_load_n(&interrupted_calls,__ATOMIC_ACQUIRE)==48);
    handoff_stats_t s=handoff_stats(); assert(s.reclaimed==9600 && !s.queued && !s.pending);
    task.tid=5000;
    net_control_value_t v={.ping={.version=1,.destination=htonl(0x0a000202),.timeout_seconds=1}};
    assert(net_control_call(NETCTL_PING,&v)==SYSCALL_EINTR);
    signal_pending=false;
    while (handoff_stats().live) sched_yield();
    task.tid=5001;
    v=(net_control_value_t){.ping={.version=1,.destination=htonl(0x0a000202),.timeout_seconds=1}};
    assert(!net_control_call(NETCTL_PING,&v)); /* Mailbox/IPv4 exactly recovered. */
    while (handoff_stats().live) sched_yield();
    pthread_mutex_lock(&wait_lock); owner_open=false; pthread_mutex_unlock(&wait_lock);
    while (!__atomic_load_n(&owner_parked,__ATOMIC_ACQUIRE)) sched_yield();
    handoff_registration_t r={0};
    assert(!handoff_begin(&r,(handoff_request_t){.actor=6000,.kind=NETCTL_IFSET,.control=true,
        .cancel_status=SYSCALL_EINTR}));
    assert(!handoff_publish(r.token)); handoff_cancel(r.token);
    pthread_mutex_lock(&wait_lock); owner_open=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&wait_lock);
    while (!handoff_ready(&r.token)) sched_yield();
    handoff_reply_t reply; assert(!handoff_collect(r.token,&reply) && reply.status==SYSCALL_EINTR);
    handoff_release(&r);
    while (handoff_stats().live) sched_yield();
    assert(handoff_stats().reclaimed==9603);
    task.tid=7000;
    v=(net_control_value_t){.trace={.version=1,.destination=htonl(0x0a000202),
        .ttl=1,.timeout_seconds=1,.deadline_ticks=apic_timer_get_bsp_ticks()}};
    assert(net_control_call(NETCTL_TRACE_PROBE,&v)==SYSCALL_ETIMEDOUT);
    assert(handoff_stats().reclaimed==9603); /* No owner side effect or token. */
    pthread_mutex_lock(&wait_lock); owner_open=false; pthread_mutex_unlock(&wait_lock);
    while (!__atomic_load_n(&owner_parked,__ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&reset_clock,1,__ATOMIC_RELEASE);
    pthread_mutex_lock(&wait_lock); owner_open=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&wait_lock);
    v=(net_control_value_t){.get={.struct_version=1}};
    int64_t reset_result=net_control_call(NETCTL_IFGET,&v);
    fprintf(stderr,"clock reset result=%lld ticks=%llu flag=%u\n",(long long)reset_result,(unsigned long long)apic_timer_get_bsp_ticks(),__atomic_load_n(&reset_clock,__ATOMIC_ACQUIRE));
    assert(reset_result==SYSCALL_EIO);
    while (handoff_stats().live) sched_yield();
    assert(handoff_stats().reclaimed==9604);
    __atomic_store_n(&stop_owner,1,__ATOMIC_RELEASE); assert(!pthread_join(worker,NULL));
    printf("Phase 2A actual-code host PASS: 9600 concurrent requests, 48 producers, coherent configuration/probes, saturation, pending cleanup/preclaim cancellation, expired deadline/clock reset, exact 9604 reclamations\n");
    return 0;
}
