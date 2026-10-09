#ifndef FORTRESS_RESCHED_POLICY_H
#define FORTRESS_RESCHED_POLICY_H
#include "types.h"
/* Internal scheduler reasons; not a user ABI. Coalescing retains urgent wakes
 * even when a fresh-work hint arrives before the same return boundary. */
#define RESCHED_URGENT 1U
#define RESCHED_WORK_HINT 2U
static inline bool resched_request_requires_yield(uint32_t reasons, bool all_work) {
    return (reasons & RESCHED_URGENT) || (all_work && (reasons & RESCHED_WORK_HINT));
}
#endif
