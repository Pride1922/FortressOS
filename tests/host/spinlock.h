/* Most host fixtures are single-threaded; TEST_VMM_HOST supplies pthread locks.
 * Real IRQ/rank behavior is tested in QEMU. */
#ifndef HOST_SPINLOCK_H
#define HOST_SPINLOCK_H
#include "types.h"
enum lock_kind {
    LOCK_KIND_ORDINARY = 0,
    LOCK_KIND_SCHED    = 1,
};
typedef struct { unsigned rank; uint8_t kind; const char *name; uint64_t acquire_count; uint64_t contention_count; } spinlock_t;
#define SPINLOCK_RANKED(r, n) {(r), LOCK_KIND_ORDINARY, (n), 0, 0}
#define SPINLOCK_RANKED_KIND(r, k, n) {(r), (k), (n), 0, 0}
#ifdef TEST_VMM_HOST
uint64_t vmm_host_lock(spinlock_t *lock);
void vmm_host_unlock(spinlock_t *lock);
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) { return vmm_host_lock(lock); }
#else
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; return 0; }
#endif
static inline void spin_lock_noirq(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock_noirq(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t f) {
    (void)f;
#ifdef TEST_VMM_HOST
    vmm_host_unlock(lock);
#else
    (void)lock;
#endif
}
static inline void spin_debug_assert_held(spinlock_t *lock) { (void)lock; }
static inline void spin_debug_assert_unheld(void) {}
#endif
