/* Snapshot-only resource/provenance oracle. No alternate owner. */
#include "net_control.h"
#include "thread.h"
#include "percpu.h"
#include "serial.h"
#include "apic.h"
static uint64_t caller_mask, kind_mask, owner_steps;
static uint64_t remote_wakes;
void net_control_fixture_woke(tcb_t *t,const void *channel) {
    /* Snapshot scheduler owns the blocked task. Atomic observation only. */
    if (t->cpu_affinity>0 && t->handoff.active &&
        channel==handoff_channel(t->handoff.token))
        __atomic_add_fetch(&remote_wakes,1,__ATOMIC_RELEASE);
}
void net_control_fixture_entry(void) {
    __atomic_fetch_or(&caller_mask,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
}
void net_control_fixture_step(unsigned kind) {
    if (cpu_current()->id) __builtin_trap();
    __atomic_fetch_or(&kind_mask,1ULL<<kind,__ATOMIC_RELEASE);
    __atomic_add_fetch(&owner_steps,1,__ATOMIC_RELEASE);
}
int64_t net_control_fixture_observe(unsigned mode) {
    extern bool handoff_fixture_empty(void *);
    extern bool handoff_fixture_pending(void *);
    sched_wait_until(&handoff_admission_channel,
        mode ? handoff_fixture_pending : handoff_fixture_empty,NULL);
    if (!mode) {
        extern bool net_control_fixture_clean(void);
        if (!net_control_fixture_clean()) __builtin_trap();
        serial_puts("[NET SMP2A MASK] "); serial_print_hex(__atomic_load_n(&caller_mask,__ATOMIC_ACQUIRE));
        serial_puts(" kinds="); serial_print_hex(__atomic_load_n(&kind_mask,__ATOMIC_ACQUIRE));
        serial_puts(" steps="); serial_print_hex(__atomic_load_n(&owner_steps,__ATOMIC_ACQUIRE));
        serial_puts(" reclaimed="); serial_print_hex(handoff_stats().reclaimed);
        serial_puts(" remote="); serial_print_hex(__atomic_load_n(&remote_wakes,__ATOMIC_ACQUIRE));
        serial_puts("\n");
    }
    return 0;
}
