#ifndef HEAP_HOST_SPINLOCK_H
#define HEAP_HOST_SPINLOCK_H
#include "types.h"
#include <assert.h>
typedef struct { unsigned rank; bool held; uint64_t acquire_count, contention_count; } spinlock_t;
#define SPINLOCK_RANKED(r, n) {(r), false, 0, 0}
static unsigned heap_host_lock_depth;
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held && heap_host_lock_depth == 0);
    lock->held = true;
    lock->acquire_count++;
    heap_host_lock_depth++;
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    assert(lock->held && heap_host_lock_depth == 1);
    lock->held = false;
    heap_host_lock_depth--;
}
#endif
