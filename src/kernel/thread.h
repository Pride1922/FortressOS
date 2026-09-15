#ifndef FORTRESS_THREAD_H
#define FORTRESS_THREAD_H

#include "types.h"

#define KSTACK_SIZE           16384 /* 16 KiB */
#define DEFAULT_QUANTUM_TICKS 2     /* 20 ms at 100 Hz */

/* Freestanding Spinlock with Interrupt Flags Preservation */
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

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

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_TERMINATED
} thread_state_t;

typedef struct tcb {
    uint64_t       rsp;              /* Saved stack pointer (MUST be first field at offset 0) */
    uint64_t       tid;
    char           name[32];
    thread_state_t state;

    void          *kstack_base;      /* Base of allocated stack buffer */
    size_t         kstack_size;

    /* Preemption & Timeslice Accounting */
    int            ticks_remaining;  /* Remaining ticks in current quantum */
    uint64_t       total_ticks;      /* Total ticks consumed by this thread */
    bool           is_idle;          /* True if dedicated idle thread */

    struct tcb    *next;             /* Intrusive run queue link */
} tcb_t;

/* Public Scheduler & Thread API */
void   sched_init(void);
tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg);
void   thread_yield(void);
void   thread_exit(void);
tcb_t *thread_current(void);
size_t sched_ready_count(void);

/* Preemption Control & Timer Hook */
void   sched_enable_preemption(void);
void   sched_disable_preemption(void);
bool   sched_is_preemption_enabled(void);
void   sched_on_timer_tick(void);

/* Low-level Context Switch Assembly Primitives */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

#endif /* FORTRESS_THREAD_H */
