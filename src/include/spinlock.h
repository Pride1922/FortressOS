#ifndef FORTRESS_SPINLOCK_H
#define FORTRESS_SPINLOCK_H

#include "types.h"

/*
 * Freestanding Spinlock with Interrupt Flags (RFLAGS) Preservation
 *
 * NOTE ON RECURSION:
 * These spinlocks are strictly NON-RECURSIVE. Acquiring the same lock twice
 * in the same execution context is rejected by the debug checker. Reentrancy is avoided by design
 * using internal unlocked helpers (e.g., kmalloc_unlocked) rather than recursive locks.
 *
 * SUBSYSTEM LOCK HIERARCHY & ORDERING:
 * To avoid deadlocks, locks must always be acquired in descending order:
 *   Level 1: g_sched_lock (Scheduler runqueue & thread state)
 *   Level 2: g_heap_lock  (Kernel heap & free list)
 *   Level 3: g_vmm_lock   (Page tables & virtual mapping)
 *   Level 4: g_pmm_lock   (Physical frame bitmap allocator)
 *   Level 5: g_console_lock (Leaf output lock)
 * ext2_lock also has rank 1 and cannot nest with g_sched_lock.
 *
 * CRITICAL CONSTRAINTS:
 * 1. Never acquire a higher-level lock while holding a lower-level lock.
 * 2. No spinlock may EVER remain held across switch_context().
 * 3. Dead thread reaping unlinks nodes under g_sched_lock, then drops
 *    g_sched_lock before calling kstack_free() (VMM/PMM) and kfree() (Heap).
 */
typedef struct {
    volatile uint32_t lock;
    unsigned rank;
    const char *name;
} spinlock_t;

#define SPINLOCK_RANKED(r, n) {0, r, n}

/* Single-CPU debug tracking; migrate to per-CPU storage before AP startup. */
void spin_debug_acquire(spinlock_t *lock);
void spin_debug_release(spinlock_t *lock);
void spin_debug_assert_unheld(void);
bool spin_debug_selftest(void);

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
    spin_debug_acquire(lock);
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    return rflags;
}

static inline void spin_unlock_noirq(spinlock_t *lock) {
    spin_debug_release(lock);
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags) {
    spin_unlock_noirq(lock);
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
}

#endif /* FORTRESS_SPINLOCK_H */
