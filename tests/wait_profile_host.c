#include <assert.h>
#include <stdio.h>
#include "wait_profile.h"
int main(void) {
    wait_trace_t t = {0};
    wait_trace_block(&t, 10); wait_trace_wake(&t, 20);
    wait_trace_select(&t, 30); wait_trace_resume(&t, 40);
    assert(t.stage == 0 && t.counters.blocks == 0);
    t.enabled = true; t.counters.valid = 1;
    for (unsigned i=0;i<100;i++) {
        wait_trace_block(&t, i*100);
        wait_trace_wake(&t, i*100+10);
        wait_trace_select(&t, i*100+30);
        wait_trace_resume(&t, i*100+35);
        assert(t.stage == 0 && t.counters.valid);
    }
    assert(t.counters.blocks == 100 && t.counters.wakes == 100);
    assert(t.counters.selections == 100 && t.counters.resumes == 100);
    assert(t.counters.blocked_cycles == 1000 && t.counters.ready_cycles == 2000);
    assert(t.counters.resume_cycles == 500 && t.counters.ready_max == 20);
    wait_trace_block(&t, 100); wait_trace_wake(&t, 99);
    assert(!t.counters.valid);
    t = (wait_trace_t){.enabled=true, .counters.valid=1};
    wait_trace_block(&t, 100); wait_trace_select(&t, 110); wait_trace_resume(&t, 120);
    assert(!t.counters.valid && !t.stage);
    t = (wait_trace_t){.enabled=true, .counters.valid=1};
    t.counters.blocks=UINT64_MAX; wait_trace_block(&t, 100);
    assert(!t.counters.valid && t.counters.blocks==UINT64_MAX);
    t = (wait_trace_t){.enabled=true, .counters.valid=1};
    t.counters.ready_cycles=UINT64_MAX;
    wait_trace_block(&t, 100); wait_trace_wake(&t, 110); wait_trace_select(&t, 120);
    assert(!t.counters.valid && t.counters.ready_cycles==UINT64_MAX);
    puts("Wait trace: disabled, exact lifecycle intervals, repeated wakes, missing wake, reversed clocks and overflow PASS");
}
