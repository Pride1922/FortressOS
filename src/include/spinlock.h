#ifndef FORTRESS_SPINLOCK_H
#define FORTRESS_SPINLOCK_H

#include "types.h"

#ifdef TEST_SMP_MEMORY
#include <pthread.h>

typedef struct spinlock {
    pthread_mutex_t mutex;
    unsigned rank;
    const char *name;
} spinlock_t;

#define SPINLOCK_RANKED(r, n) {PTHREAD_MUTEX_INITIALIZER, (r), (n)}

uint64_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);

#else  /* !TEST_SMP_MEMORY */

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
enum lock_kind {
    LOCK_KIND_ORDINARY = 0,
    LOCK_KIND_SCHED    = 1,
};

typedef struct spinlock {
    volatile uint32_t lock;
    uint8_t rank;
    uint8_t kind;
    const char *name;
    uint64_t acquire_count;
    uint64_t contention_count;
    uint64_t max_spin_iters;
} spinlock_t;

#define SPINLOCK_RANKED(r, n) {0, (r), LOCK_KIND_ORDINARY, (n), 0, 0, 0}
#define SPINLOCK_RANKED_KIND(r, k, n) {0, (r), (k), (n), 0, 0, 0}

/* Per-CPU lock discipline & tracking */
void spin_debug_acquire(spinlock_t *lock);
void spin_debug_release(spinlock_t *lock);
void spin_debug_assert_held(spinlock_t *lock);
void spin_debug_assert_unheld(void);
bool spin_debug_selftest(void);
void spin_debug_warn_high_contention(spinlock_t *lock, uint64_t iters);

#define SPINLOCK_WARN_THRESHOLD 1000000ULL

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
    spin_debug_acquire(lock);
    lock->acquire_count++;
    if (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        lock->contention_count++;
        uint64_t iters = 1;
        while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
            iters++;
            if (iters == SPINLOCK_WARN_THRESHOLD) {
                spin_debug_warn_high_contention(lock, iters);
            }
        }
        if (iters > lock->max_spin_iters) {
            lock->max_spin_iters = iters;
        }
    }
    return rflags;
}

static inline void spin_lock_noirq(spinlock_t *lock) {
    spin_debug_acquire(lock);
    lock->acquire_count++;
    if (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        lock->contention_count++;
        uint64_t iters = 1;
        while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
            iters++;
            if (iters == SPINLOCK_WARN_THRESHOLD) {
                spin_debug_warn_high_contention(lock, iters);
            }
        }
        if (iters > lock->max_spin_iters) {
            lock->max_spin_iters = iters;
        }
    }
}

static inline void spin_unlock_noirq(spinlock_t *lock) {
    spin_debug_release(lock);
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags) {
    spin_unlock_noirq(lock);
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
}

#endif  /* TEST_SMP_MEMORY */

#endif /* FORTRESS_SPINLOCK_H */
