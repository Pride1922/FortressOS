#ifndef HOST_THREAD_H
#define HOST_THREAD_H

#include "types.h"
#include "spinlock.h"

typedef struct tcb {
    uint64_t  tid;
    uintptr_t cr3;
} tcb_t;

static inline tcb_t *thread_create_on_cpu(size_t c, const char *n, void (*entry)(void *), void *a) {
    (void)c; (void)n; (void)entry; (void)a;
    return NULL;
}
static inline void thread_yield(void) {}
static inline void thread_exit(void) {}
static inline void sched_reap_dead(void) {}
static inline uint64_t sched_get_active_stack_slots_mask(void) { return 0; }

#endif

