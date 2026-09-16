#ifndef FORTRESS_THREAD_H
#define FORTRESS_THREAD_H

#include "types.h"
#include "spinlock.h"

#define KERNEL_STACKS_BASE    0xFFFFFFFFA0000000ULL
#define MAX_KERNEL_THREADS    64
#define STACK_GUARD_SIZE      4096ULL  /* 4 KiB unmapped guard page */
#define STACK_USABLE_SIZE     16384ULL /* 16 KiB usable mapped stack (4 pages) */
#define STACK_SLOT_SIZE       (STACK_GUARD_SIZE + STACK_USABLE_SIZE) /* 20 KiB */
#define DEFAULT_QUANTUM_TICKS 2        /* 20 ms at 100 Hz */

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

    int            stack_slot;       /* Slot index in kernel stack area (-1 for adopted main thread) */
    uintptr_t      kstack_guard;     /* Virtual address of unmapped guard page */
    uintptr_t      kstack_base;      /* Virtual base of usable mapped stack region */
    size_t         kstack_size;      /* Size of usable mapped stack region (16 KiB) */

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
uint64_t sched_get_active_stack_slots_mask(void);

/* Low-level Context Switch Assembly Primitives */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

#endif /* FORTRESS_THREAD_H */
