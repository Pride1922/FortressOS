#ifndef FORTRESS_SPINLOCK_H
#define FORTRESS_SPINLOCK_H

#include "types.h"

/* Freestanding Spinlock with Interrupt Flags (RFLAGS) Preservation */
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t){0})

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    return rflags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t rflags) {
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
}

#endif /* FORTRESS_SPINLOCK_H */
