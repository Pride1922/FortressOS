#ifndef FORTRESS_THREAD_H
#define FORTRESS_THREAD_H

#include "types.h"

#define KSTACK_SIZE 16384 /* 16 KiB */

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_TERMINATED
} thread_state_t;

typedef struct tcb {
    uint64_t       rsp;          /* Saved stack pointer (MUST be first field at offset 0) */
    uint64_t       tid;
    char           name[32];
    thread_state_t state;

    void          *kstack_base;  /* Base of allocated stack buffer */
    size_t         kstack_size;

    struct tcb    *next;         /* Intrusive run queue link */
} tcb_t;

/* Public Scheduler & Thread API */
void   sched_init(void);
tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg);
void   thread_yield(void);
void   thread_exit(void);
tcb_t *thread_current(void);
size_t sched_ready_count(void);

/* Low-level Context Switch Assembly Primitives */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

#endif /* FORTRESS_THREAD_H */
