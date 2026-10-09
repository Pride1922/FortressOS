#include <assert.h>
#include <stdio.h>
#include "resched_return.h"
#include "resched_policy.h"
int main(void) {
    interrupt_frame_t f = {.vector=0x80,.cs=0x23,.rflags=0x202};
    assert(resched_return_allowed(&f,true,0,0,true,true));
    assert(!resched_return_allowed(NULL,true,0,0,true,true));
    for (unsigned bits=0; bits<64; bits++) {
        f.cs = bits & 1 ? 0x23 : 0x08;
        f.rflags = bits & 2 ? 0x202 : 2;
        bool allowed=resched_return_allowed(&f,!!(bits&4),bits&8 ? 1:0,bits&16 ? 1:0,!!(bits&32),true);
        assert(allowed == (bits == 39));
    }
    f.cs=0x23; f.rflags=0x202;
    assert(!resched_return_allowed(&f,true,0,0,true,false));
    for (unsigned vector=0;vector<257;vector++) {
        f.vector=vector;
        assert(resched_return_allowed(&f,true,0,0,true,true) == (vector>=32 && vector<256));
    }
    puts("Reschedule return gate: user/IF/nesting/locks/preemption/task boundary cases PASS");
    for (unsigned reasons=0; reasons<8; reasons++) {
        assert(resched_request_requires_yield(reasons,false) == !!(reasons & RESCHED_URGENT));
        assert(resched_request_requires_yield(reasons,true) == !!(reasons & (RESCHED_URGENT|RESCHED_WORK_HINT)));
    }
    uint32_t pending=RESCHED_URGENT;
    __atomic_fetch_or(&pending,RESCHED_WORK_HINT,__ATOMIC_RELEASE);
    assert(resched_request_requires_yield(__atomic_exchange_n(&pending,0,__ATOMIC_ACQ_REL),false));
    assert(!pending);
    puts("Request policy: fresh work versus urgent wake/signal, mixed coalescing and control PASS");
}
