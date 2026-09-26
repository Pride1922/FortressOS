#ifndef PIPE_HOST_SPINLOCK_H
#define PIPE_HOST_SPINLOCK_H
/* Single-threaded adapter: detect lock-held scheduler calls; QEMU tests sleep. */
#include "types.h"
#include <assert.h>
typedef struct { unsigned rank; bool held; } spinlock_t;
#define SPINLOCK_RANKED(r, n) {(r), false}
static unsigned pipe_host_lock_depth;
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held && pipe_host_lock_depth == 0);
    lock->held = true; pipe_host_lock_depth++; return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(lock->held && pipe_host_lock_depth == 1);
    lock->held = false; pipe_host_lock_depth--;
}
static inline void spin_debug_assert_unheld(void) { assert(!pipe_host_lock_depth); }
#endif
