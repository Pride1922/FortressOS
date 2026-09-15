#include "thread.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

static tcb_t       g_main_thread;
static tcb_t      *g_idle_thread        = NULL;
static tcb_t      *g_current_thread     = NULL;
static tcb_t      *g_runqueue_head      = NULL;
static tcb_t      *g_runqueue_tail      = NULL;
static tcb_t      *g_dead_threads       = NULL;
static uint64_t    g_next_tid           = 1;
static spinlock_t  g_sched_lock         = {0};
static volatile bool g_preemption_enabled = false;

/* External LAPIC EOI and flag to avoid duplicate EOI */
extern void lapic_eoi(void);
volatile bool g_timer_eoi_handled = false;

static void runqueue_push_locked(tcb_t *t) {
    if (!t || t->is_idle) return;
    t->next = NULL;
    if (!g_runqueue_head) {
        g_runqueue_head = t;
        g_runqueue_tail = t;
    } else {
        g_runqueue_tail->next = t;
        g_runqueue_tail = t;
    }
}

static tcb_t *runqueue_pop_next_locked(void) {
    if (!g_runqueue_head) return NULL;
    tcb_t *t = g_runqueue_head;
    g_runqueue_head = g_runqueue_head->next;
    if (!g_runqueue_head) {
        g_runqueue_tail = NULL;
    }
    t->next = NULL;
    return t;
}

static void sched_reap_dead_locked(void) {
    tcb_t *dead = g_dead_threads;
    g_dead_threads = NULL;

    while (dead) {
        tcb_t *next = dead->next;
        if (dead->kstack_base) {
            kfree(dead->kstack_base);
        }
        kfree(dead);
        dead = next;
    }
}

static void idle_thread_entry(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

void sched_init(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);

    memset(&g_main_thread, 0, sizeof(tcb_t));
    g_main_thread.rsp = 0; /* Captured dynamically on first switch_context */
    g_main_thread.tid = 0;
    memcpy(g_main_thread.name, "main", 5);
    g_main_thread.state = THREAD_RUNNING;
    g_main_thread.kstack_base = NULL;
    g_main_thread.kstack_size = 0;
    g_main_thread.ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_main_thread.total_ticks = 0;
    g_main_thread.is_idle = false;
    g_main_thread.next = NULL;

    g_current_thread = &g_main_thread;
    g_runqueue_head = NULL;
    g_runqueue_tail = NULL;
    g_dead_threads = NULL;
    g_next_tid = 1;
    g_preemption_enabled = false;
    g_timer_eoi_handled = false;

    spin_unlock_irqrestore(&g_sched_lock, rflags);

    /* Create dedicated low-power idle thread */
    g_idle_thread = thread_create("idle", idle_thread_entry, NULL);
    if (g_idle_thread) {
        rflags = spin_lock_irqsave(&g_sched_lock);
        g_idle_thread->is_idle = true;
        /* Remove idle thread from normal runqueue so it only runs when queue is empty */
        if (g_runqueue_head == g_idle_thread) {
            g_runqueue_head = g_idle_thread->next;
            if (!g_runqueue_head) g_runqueue_tail = NULL;
            g_idle_thread->next = NULL;
        }
        spin_unlock_irqrestore(&g_sched_lock, rflags);
    }

    serial_puts("[ OK ] Preemptive thread scheduler initialized (main thread adopted, idle thread armed)\n");
}

tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return NULL;

    tcb_t *t = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(tcb_t));

    void *stack = kmalloc(KSTACK_SIZE);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    t->tid = g_next_tid++;
    spin_unlock_irqrestore(&g_sched_lock, rflags);

    if (name) {
        size_t len = strlen(name);
        if (len >= sizeof(t->name)) len = sizeof(t->name) - 1;
        memcpy(t->name, name, len);
        t->name[len] = '\0';
    } else {
        memcpy(t->name, "worker", 7);
    }

    t->state = THREAD_READY;
    t->kstack_base = stack;
    t->kstack_size = KSTACK_SIZE;
    t->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    t->total_ticks = 0;
    t->is_idle = false;

    /* Setup initial stack frame to match switch_context restore sequence:
     * switch_context pops: r15, r14, r13, r12, rbp, rbx, rflags, ret (rip)
     * Total: 8 qwords = 64 bytes.
     */
    uint8_t *stack_top = (uint8_t *)stack + KSTACK_SIZE;
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL); /* 16-byte alignment */

    stack_top -= sizeof(uint64_t) * 8;
    uint64_t *frame = (uint64_t *)stack_top;

    frame[0] = 0;                           /* r15 */
    frame[1] = 0;                           /* r14 */
    frame[2] = (uint64_t)arg;               /* r13 -> argument pointer */
    frame[3] = (uint64_t)entry;             /* r12 -> entry function pointer */
    frame[4] = 0;                           /* rbp */
    frame[5] = 0;                           /* rbx */
    frame[6] = 0x202;                       /* rflags: IF=1 (interrupts enabled), bit 1 reserved */
    frame[7] = (uint64_t)thread_trampoline; /* rip */

    t->rsp = (uint64_t)stack_top;

    rflags = spin_lock_irqsave(&g_sched_lock);
    runqueue_push_locked(t);
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return t;
}

void thread_yield(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    sched_reap_dead_locked();

    tcb_t *old = g_current_thread;
    tcb_t *next = runqueue_pop_next_locked();

    if (!next) {
        /* If no ready threads, pick idle thread (unless current is already idle) */
        if (!old->is_idle && g_idle_thread) {
            next = g_idle_thread;
        } else {
            /* Keep running current thread */
            spin_unlock_irqrestore(&g_sched_lock, rflags);
            return;
        }
    }

    if (old->state == THREAD_RUNNING && !old->is_idle) {
        old->state = THREAD_READY;
        runqueue_push_locked(old);
    }

    next->state = THREAD_RUNNING;
    next->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_current_thread = next;

    /* Release spinlock before context switch, but keep interrupts disabled */
    __atomic_clear(&g_sched_lock.lock, __ATOMIC_RELEASE);

    switch_context(&old->rsp, next->rsp);

    /* Execution resumes here when old is switched back to */
    rflags = spin_lock_irqsave(&g_sched_lock);
    sched_reap_dead_locked();
    spin_unlock_irqrestore(&g_sched_lock, rflags);
}

void thread_exit(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    (void)rflags;
    tcb_t *curr = g_current_thread;
    curr->state = THREAD_TERMINATED;

    /* Enqueue into dead list for reclamation */
    curr->next = g_dead_threads;
    g_dead_threads = curr;

    tcb_t *next = runqueue_pop_next_locked();
    if (!next) {
        if (g_idle_thread && curr != g_idle_thread) {
            next = g_idle_thread;
        } else {
            serial_puts("[FATAL] All threads terminated; no runnable threads remaining!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    next->state = THREAD_RUNNING;
    next->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_current_thread = next;

    __atomic_clear(&g_sched_lock.lock, __ATOMIC_RELEASE);

    uint64_t dummy_old_rsp = 0;
    switch_context(&dummy_old_rsp, next->rsp);

    /* Never reached */
    for (;;) { __asm__ volatile("cli; hlt"); }
}

tcb_t *thread_current(void) {
    return g_current_thread;
}

size_t sched_ready_count(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    size_t count = 0;
    tcb_t *curr = g_runqueue_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return count;
}

void sched_enable_preemption(void) {
    g_preemption_enabled = true;
}

void sched_disable_preemption(void) {
    g_preemption_enabled = false;
}

bool sched_is_preemption_enabled(void) {
    return g_preemption_enabled;
}

void sched_on_timer_tick(void) {
    if (!g_preemption_enabled || !g_current_thread) {
        return;
    }

    g_current_thread->total_ticks++;

    /* If currently in idle thread and work arrived in runqueue: preempt idle */
    if (g_current_thread->is_idle) {
        if (g_runqueue_head != NULL) {
            lapic_eoi();
            g_timer_eoi_handled = true;
            thread_yield();
        }
        return;
    }

    /* Timeslice accounting for normal threads */
    if (--g_current_thread->ticks_remaining <= 0) {
        g_current_thread->ticks_remaining = DEFAULT_QUANTUM_TICKS;

        /* If other threads are ready to run: preempt! */
        if (g_runqueue_head != NULL) {
            /* CRITICAL: Send EOI before switching context so APIC timer
             * priority threshold is cleared and new thread receives timer ticks! */
            lapic_eoi();
            g_timer_eoi_handled = true;
            thread_yield();
        }
    }
}
