#include "thread.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

static tcb_t  g_main_thread;
static tcb_t *g_current_thread = NULL;
static tcb_t *g_runqueue_head  = NULL;
static tcb_t *g_runqueue_tail  = NULL;
static tcb_t *g_dead_threads   = NULL;
static uint64_t g_next_tid     = 1;

static void runqueue_push(tcb_t *t) {
    if (!t) return;
    t->next = NULL;
    if (!g_runqueue_head) {
        g_runqueue_head = t;
        g_runqueue_tail = t;
    } else {
        g_runqueue_tail->next = t;
        g_runqueue_tail = t;
    }
}

static tcb_t *runqueue_pop_next(void) {
    if (!g_runqueue_head) return NULL;
    tcb_t *t = g_runqueue_head;
    g_runqueue_head = g_runqueue_head->next;
    if (!g_runqueue_head) {
        g_runqueue_tail = NULL;
    }
    t->next = NULL;
    return t;
}

static void sched_reap_dead(void) {
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

void sched_init(void) {
    memset(&g_main_thread, 0, sizeof(tcb_t));
    g_main_thread.rsp = 0; /* Captured dynamically on first switch_context */
    g_main_thread.tid = 0;
    memcpy(g_main_thread.name, "main", 5);
    g_main_thread.state = THREAD_RUNNING;
    g_main_thread.kstack_base = NULL;
    g_main_thread.kstack_size = 0;
    g_main_thread.next = NULL;

    g_current_thread = &g_main_thread;
    g_runqueue_head = NULL;
    g_runqueue_tail = NULL;
    g_dead_threads = NULL;
    g_next_tid = 1;

    serial_puts("[ OK ] Cooperative thread scheduler initialized (main thread adopted)\n");
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

    t->tid = g_next_tid++;
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

    runqueue_push(t);
    return t;
}

void thread_yield(void) {
    sched_reap_dead();

    tcb_t *old = g_current_thread;
    tcb_t *next = runqueue_pop_next();
    if (!next) {
        /* No other thread ready; continue running current */
        return;
    }

    if (old->state == THREAD_RUNNING) {
        old->state = THREAD_READY;
        runqueue_push(old);
    }

    next->state = THREAD_RUNNING;
    g_current_thread = next;

    switch_context(&old->rsp, next->rsp);

    /* Clean up any dead threads when resuming execution */
    sched_reap_dead();
}

void thread_exit(void) {
    tcb_t *curr = g_current_thread;
    curr->state = THREAD_TERMINATED;

    /* Enqueue into dead list for reclamation */
    curr->next = g_dead_threads;
    g_dead_threads = curr;

    tcb_t *next = runqueue_pop_next();
    if (!next) {
        serial_puts("[FATAL] All threads terminated; no runnable threads remaining in scheduler!\n");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    next->state = THREAD_RUNNING;
    g_current_thread = next;

    uint64_t dummy_old_rsp = 0;
    switch_context(&dummy_old_rsp, next->rsp);

    /* Never reached */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

tcb_t *thread_current(void) {
    return g_current_thread;
}

size_t sched_ready_count(void) {
    size_t count = 0;
    tcb_t *curr = g_runqueue_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}
