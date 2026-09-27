#ifndef FORTRESS_SIGNAL_STATE_H
#define FORTRESS_SIGNAL_STATE_H
#include "signal_abi.h"
/* Embedded in TCB. Mutation under process lock; scheduler observes atomic masks.
 * Registry attachment lasts until exit/abort, which precedes TCB reclamation. */
typedef struct {
    uint64_t pending_mask, blocked_mask, ignored_mask;
    uint64_t action_masks[32];
} signal_state_t;
static inline bool signal_state_ready(const signal_state_t *s) {
    return (__atomic_load_n(&s->pending_mask, __ATOMIC_ACQUIRE) &
            ~__atomic_load_n(&s->blocked_mask, __ATOMIC_ACQUIRE) &
            ~__atomic_load_n(&s->ignored_mask, __ATOMIC_ACQUIRE)) != 0;
}
#endif
