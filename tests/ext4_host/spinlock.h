#ifndef EXT4_HOST_SPINLOCK_H
#define EXT4_HOST_SPINLOCK_H
/* Pthread exclusion for actual filesystem callbacks; no IRQ/rank claim. */
#include "types.h"
#include <pthread.h>
#include <assert.h>
typedef struct { pthread_mutex_t mutex; unsigned rank; } spinlock_t;
#define SPINLOCK_RANKED(r,n) {PTHREAD_MUTEX_INITIALIZER,(r)}
static _Thread_local unsigned ext4_host_lock_depth;
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!ext4_host_lock_depth); assert(!pthread_mutex_lock(&lock->mutex));
    ext4_host_lock_depth=1; return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock,uint64_t flags) {
    (void)flags; assert(ext4_host_lock_depth==1); ext4_host_lock_depth=0;
    assert(!pthread_mutex_unlock(&lock->mutex));
}
static inline void spin_debug_assert_unheld(void) { assert(!ext4_host_lock_depth); }
#endif
