#ifndef FORTRESS_RESCHED_RETURN_H
#define FORTRESS_RESCHED_RETURN_H
#include "idt.h"
/* Shared eligibility gate for post-EOI IRQ and normal syscall returns.
 * Saved IF excludes isolated IF=0 Ring 3 test fixtures. Current IF must stay
 * clear throughout the existing scheduler/CR3/stack exchange. */
static inline bool resched_return_allowed(const interrupt_frame_t *frame,
                                          bool irq_disabled, uint64_t irq_depth,
                                          uint32_t lock_depth, bool enabled,
                                          bool user_running) {
    return frame && frame->vector >= 32 && frame->vector < 256 &&
           (frame->cs & 3) == 3 && (frame->rflags & (1ULL << 9)) &&
           irq_disabled && !irq_depth && !lock_depth && enabled && user_running;
}
#endif
