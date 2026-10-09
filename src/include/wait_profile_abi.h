#ifndef FORTRESS_WAIT_PROFILE_ABI_H
#define FORTRESS_WAIT_PROFILE_ABI_H
#include "types.h"
/* Additional SYS_SPAWN_PROFILE actions; the original 160-byte spawn ABI stays
 * unchanged. Self-only, opt-in scheduler waits, never inherited by children.
 * Selection is stamped before CR3/context switch; resume is in the original
 * wait continuation. Intervals include preemption and require ordered TSC. */
#define WAIT_PROFILE_READ 3
#define WAIT_PROFILE_ENABLE 4
#define WAIT_PROFILE_DISABLE 5
typedef struct {
    uint64_t valid, blocks, wakes, selections, resumes;
    uint64_t blocked_cycles, ready_cycles, resume_cycles, ready_max;
} wait_profile_t;
_Static_assert(sizeof(wait_profile_t) == 72, "wait diagnostic ABI size");
#endif
