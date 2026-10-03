#include <assert.h>
#include <stdio.h>
#include "net_ping.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
static uint64_t ticks, current;
static unsigned wakes, starts, cancels;
static bool complete;
uint64_t apic_timer_get_bsp_ticks(void) { return ticks; }
uint64_t apic_timer_get_frequency(void) { return 100; }
bool net_ipv4_unicast(uint32_t ip) { return ip!=0; }
void sched_wake_all(const void *channel) { assert(channel==&g_net_ping_channel); ++wakes; }
bool net_ipv4_ping_start(const net_ping_v1_t *req, uint64_t token, uint64_t now) {
    (void)req; (void)now; current=token; ++starts; return true;
}
bool net_ipv4_ping_take(net_ping_v1_t *result) {
    if (!complete) return false;
    *result=(net_ping_v1_t){.outcome=0,.echoed_bytes=32,.rtt_ticks=5,.tick_hz=100}; complete=false; return true;
}
void net_ipv4_ping_cancel(uint64_t token) { assert(token==current); ++cancels; }
bool net_ipv4_trace_start(const net_trace_v1_t *req, uint64_t token, uint32_t identity, uint64_t now) {
    (void)req; (void)token; (void)identity; (void)now; assert(!"unexpected trace"); return false;
}
bool net_ipv4_trace_take(net_trace_v1_t *result, int64_t *error) {
    (void)result; (void)error; assert(!"unexpected trace"); return false;
}
void net_ipv4_trace_cancel(uint64_t token) { (void)token; assert(!"unexpected trace"); }
int main(void) {
    net_ping_v1_t req={.version=1,.destination=123,.timeout_seconds=1}, result;
    uint64_t token, other;
    net_ping_init(false); assert(net_ping_submit(&req,&token)==SYSCALL_EIO);
    net_ping_init(true);
    req.reserved=1; assert(net_ping_submit(&req,&token)==SYSCALL_EINVAL); req.reserved=0;
    req.timeout_seconds=6; assert(net_ping_submit(&req,&token)==SYSCALL_EINVAL); req.timeout_seconds=1;
    assert(!net_ping_submit(&req,&token)); assert(!net_ping_ready(&token));
    assert(net_ping_submit(&req,&other)==SYSCALL_EAGAIN);
    net_ping_worker_tick(0); assert(starts==1 && !wakes);
    complete=true; net_ping_worker_tick(1); assert(net_ping_ready(&token) && wakes==1);
    assert(!net_ping_collect(token,&result) && result.echoed_bytes==32 && result.rtt_ticks==5);
    assert(net_ping_collect(token,&result)==SYSCALL_EINTR);
    assert(!net_ping_submit(&req,&other)); net_ping_worker_tick(2);
    net_ping_cancel(token); assert(!net_ping_ready(&other)); /* stale cancellation no effect */
    ticks=700; net_ping_worker_tick(ticks); assert(net_ping_ready(&other) && cancels==1);
    assert(net_ping_collect(other,&result)==SYSCALL_EINTR); /* abandoned owner reclaimed */
    assert(!net_ping_submit(&req,&token)); net_ping_worker_tick(701);
    complete=true; net_ping_worker_tick(702);
    ticks=1400; net_ping_worker_tick(ticks); assert(net_ping_collect(token,&result)==SYSCALL_EINTR);
    assert(!net_ping_submit(&req,&other)); net_ping_worker_tick(1401);
    net_ping_cancel(other); net_ping_worker_tick(1402); assert(net_ping_ready(&other));
    assert(cancels==3 && starts==4);
    puts("Ping mailbox ASan/UBSan PASS: ABI bounds, busy, publication, completion, finite abandoned-owner/result lease, stale token/cancel");
}
