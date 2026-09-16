/* Host parser tests are single-threaded. Real IRQ/rank behavior is tested in QEMU. */
#ifndef HOST_SPINLOCK_H
#define HOST_SPINLOCK_H
#include "types.h"
typedef struct { unsigned rank; } spinlock_t;
#define SPINLOCK_RANKED(r, n) {r}
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t f) { (void)lock; (void)f; }
#endif
