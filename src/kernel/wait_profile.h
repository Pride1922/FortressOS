#ifndef FORTRESS_WAIT_PROFILE_H
#define FORTRESS_WAIT_PROFILE_H
#include "wait_profile_abi.h"
typedef struct {
    bool enabled;
    unsigned stage;
    uint64_t stamp;
    wait_profile_t counters;
} wait_trace_t;
static inline void wait_trace_add(wait_trace_t *t, uint64_t *value, uint64_t amount) {
    if (amount > UINT64_MAX - *value) t->counters.valid = 0;
    else *value += amount;
}
static inline uint64_t wait_trace_delta(wait_trace_t *t, uint64_t now) {
    if (now < t->stamp) { t->counters.valid = 0; return 0; }
    return now - t->stamp;
}
static inline void wait_trace_block(wait_trace_t *t, uint64_t now) {
    if (!t->enabled) return;
    if (t->stage) t->counters.valid = 0;
    wait_trace_add(t, &t->counters.blocks, 1);
    t->stage = 1; t->stamp = now;
}
static inline void wait_trace_wake(wait_trace_t *t, uint64_t now) {
    if (!t->enabled || !t->stage) return;
    if (t->stage != 1) { t->counters.valid = 0; return; }
    wait_trace_add(t, &t->counters.blocked_cycles, wait_trace_delta(t, now));
    wait_trace_add(t, &t->counters.wakes, 1);
    t->stage = 2; t->stamp = now;
}
static inline void wait_trace_select(wait_trace_t *t, uint64_t now) {
    if (!t->enabled || !t->stage) return;
    if (t->stage != 2) { t->counters.valid = 0; return; }
    uint64_t delta = wait_trace_delta(t, now);
    wait_trace_add(t, &t->counters.ready_cycles, delta);
    if (delta > t->counters.ready_max) t->counters.ready_max = delta;
    wait_trace_add(t, &t->counters.selections, 1);
    t->stage = 3; t->stamp = now;
}
static inline void wait_trace_resume(wait_trace_t *t, uint64_t now) {
    if (!t->enabled || !t->stage) return;
    if (t->stage != 3) t->counters.valid = 0;
    else {
        wait_trace_add(t, &t->counters.resume_cycles, wait_trace_delta(t, now));
        wait_trace_add(t, &t->counters.resumes, 1);
    }
    t->stage = 0;
}
#endif
