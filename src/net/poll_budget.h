#ifndef FORTRESS_NET_POLL_BUDGET_H
#define FORTRESS_NET_POLL_BUDGET_H
#include "types.h"
typedef struct { uint64_t start,last,cycles; unsigned turns; bool active,failed; } net_poll_budget_t;
typedef struct { uint64_t last_rx; unsigned batches; } net_poll_adaptive_t;
/* Two RX batches within 20ms while TCP is active select 2ms grace. Quiet,
 * cold and non-TCP operation return to 1ms. Empty polls never add progress. */
static inline unsigned net_poll_adaptive_us(net_poll_adaptive_t *a,
    uint64_t tick,uint64_t tick_hz,bool tcp_active,bool rx) {
    uint64_t quiet=tick_hz/50u;
    if (!quiet) quiet=1;
    if (!tcp_active || tick<a->last_rx || tick-a->last_rx>=quiet) a->batches=0;
    if (tcp_active && rx) {
        if (a->batches<2) ++a->batches;
        a->last_rx=tick;
    }
    return a->batches>=2 ? 2000u : 1000u;
}
static inline void net_poll_budget_start_us(net_poll_budget_t *b,uint64_t now,
    uint64_t hz,unsigned us) {
    /* Only the two bounded profiles are admitted. Division before multiply
     * avoids overflow and leaves the 4096-turn fault backstop unchanged. */
    uint64_t divisor=us==2000u ? 500u : 1000u;
    *b=(net_poll_budget_t){.start=now,.last=now,.cycles=hz/divisor,.active=hz>=divisor};
}
static inline void net_poll_budget_start(net_poll_budget_t *b,uint64_t now,uint64_t hz) {
    net_poll_budget_start_us(b,now,hz,1000u);
}
/* 1–2 ms, finite iteration backstop even for a stalled clock. Backwards
 * readings disable this burst. Empty polls never restart the deadline. */
static inline bool net_poll_budget_continue(net_poll_budget_t *b,uint64_t now) {
    if (!b->active) return false;
    if (now<b->last || ++b->turns>=4096) {
        b->failed=true; b->active=false; return false;
    }
    if (now-b->start>=b->cycles) {
        b->active=false; return false;
    }
    b->last=now; return true;
}
#endif
