#include <assert.h>
#include "spinlock.h"
static _Thread_local unsigned held;
void net_test_assert_unheld(void) { assert(!held); }
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!held); assert(!pthread_mutex_lock(&lock->mutex)); held=1; return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(held==1); held=0; assert(!pthread_mutex_unlock(&lock->mutex));
}
