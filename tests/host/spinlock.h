/* Host parser tests are single-threaded. Real IRQ/rank behavior is tested in QEMU. */
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
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; return 0; }
static inline void spin_lock_noirq(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock_noirq(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t f) { (void)lock; (void)f; }
static inline void spin_debug_assert_held(spinlock_t *lock) { (void)lock; }
static inline void spin_debug_assert_unheld(void) {}
#endif
