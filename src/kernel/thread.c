#include "thread.h"
#include "percpu.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "serial.h"
#include "gdt.h"
#include "elf.h"
#include "vfs.h"
#include "syscall.h"

extern uint8_t kernel_stack_guard[];

/* Process Exit Records Table */
typedef struct {
    uint64_t pid;
    uint64_t exit_code;
    uint64_t preempt_count;
    uint64_t total_ticks;
    bool     valid;
} exit_record_t;

#define MAX_EXIT_RECORDS 64

/* Reserved before a user spawn, retained until wait or parent exit. Separate
 * from the legacy kernel-test history, which is allowed to overwrite records. */
typedef struct {
    uint64_t parent, pid, status;
    bool used, done;
} child_record_t;

/* Storage only: AP scheduler execution remains prohibited until Pieces 3-4. */
static struct scheduler_cpu {
    exit_record_t g_exit_records[MAX_EXIT_RECORDS];
    size_t        g_exit_records_head;
    child_record_t g_child_records[MAX_EXIT_RECORDS];
    uint64_t      g_sched_runnable_switches;
    tcb_t         g_main_thread;
    tcb_t        *g_idle_thread;
    tcb_t        *g_runqueue_head;
    tcb_t        *g_runqueue_tail;
    tcb_t        *g_blocked_threads;
    tcb_t        *g_dead_threads;
    uint64_t      g_next_tid;
    spinlock_t    g_sched_lock;
    volatile bool g_preemption_enabled;
    uint64_t      g_stack_slots_bitmap;
} scheduler_cpus[MAX_DETECTED_CPUS] = { [0] = { .g_next_tid = 1, .g_sched_lock = SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, "sched") } };
/* Read-only debug metadata for host tests; these are addresses, not mirrors. */
const uintptr_t scheduler_debug_bsp[] = {
    (uintptr_t)&scheduler_cpus[0].g_blocked_threads,
    (uintptr_t)&cpu_locals[0].current_thread,
    (uintptr_t)&scheduler_cpus[0].g_stack_slots_bitmap
};
#define g_exit_records (scheduler_cpus[cpu_current()->id].g_exit_records)
#define g_exit_records_head (scheduler_cpus[cpu_current()->id].g_exit_records_head)
#define g_child_records (scheduler_cpus[cpu_current()->id].g_child_records)
#define g_sched_timer_preemptions (cpu_current()->preempt_count)
#define g_sched_runnable_switches (scheduler_cpus[cpu_current()->id].g_sched_runnable_switches)
#define g_main_thread (scheduler_cpus[cpu_current()->id].g_main_thread)
#define g_idle_thread (scheduler_cpus[cpu_current()->id].g_idle_thread)
#define g_runqueue_head (scheduler_cpus[cpu_current()->id].g_runqueue_head)
#define g_runqueue_tail (scheduler_cpus[cpu_current()->id].g_runqueue_tail)
#define g_blocked_threads (scheduler_cpus[cpu_current()->id].g_blocked_threads)
#define g_dead_threads (scheduler_cpus[cpu_current()->id].g_dead_threads)
#define g_next_tid (scheduler_cpus[cpu_current()->id].g_next_tid)
#define g_sched_lock (scheduler_cpus[cpu_current()->id].g_sched_lock)
#define g_preemption_enabled (scheduler_cpus[cpu_current()->id].g_preemption_enabled)
#define g_stack_slots_bitmap (scheduler_cpus[cpu_current()->id].g_stack_slots_bitmap)
#define g_current_thread (cpu_current()->current_thread)



static int kstack_alloc(uintptr_t *out_guard, uintptr_t *out_base, size_t *out_size) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    int slot = -1;
    for (int i = 0; i < MAX_KERNEL_THREADS; i++) {
        if (!(g_stack_slots_bitmap & (1ULL << i))) {
            g_stack_slots_bitmap |= (1ULL << i);
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, rflags);

    if (slot == -1) {
        serial_puts("[WARN] Thread stack slots exhausted (max 64 concurrent threads)!\n");
        return -1;
    }

    uintptr_t slot_addr  = KERNEL_STACKS_BASE + (uintptr_t)slot * STACK_SLOT_SIZE;
    uintptr_t guard_addr = slot_addr;
    uintptr_t base_addr  = slot_addr + STACK_GUARD_SIZE;

    uint64_t *pml4 = vmm_get_kernel_pml4_virt();

    /* Map 4 pages for usable stack region; guard page at slot_addr remains unmapped */
    for (size_t p = 0; p < (STACK_USABLE_SIZE / PAGE_SIZE); p++) {
        uintptr_t phys = pmm_alloc_page();
        if (phys == 0) {
            for (size_t r = 0; r < p; r++) {
                uintptr_t mapped_virt = base_addr + r * PAGE_SIZE;
                uintptr_t mapped_phys = vmm_get_physical_address(pml4, mapped_virt);
                vmm_unmap_page(pml4, mapped_virt);
                if (mapped_phys) pmm_free_page(mapped_phys);
            }
            rflags = spin_lock_irqsave(&g_sched_lock);
            g_stack_slots_bitmap &= ~(1ULL << slot);
            spin_unlock_irqrestore(&g_sched_lock, rflags);
            return -1;
        }

        int status = vmm_map_page(pml4, base_addr + p * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
        if (status != VMM_OK) {
            serial_puts("[WARN] vmm_map_page failed in kstack_alloc with error: ");
            serial_print_dec(status);
            serial_puts(" at virt: ");
            serial_print_hex(base_addr + p * PAGE_SIZE);
            serial_puts("\n");
            pmm_free_page(phys);
            for (size_t r = 0; r < p; r++) {
                uintptr_t mapped_virt = base_addr + r * PAGE_SIZE;
                uintptr_t mapped_phys = vmm_get_physical_address(pml4, mapped_virt);
                vmm_unmap_page(pml4, mapped_virt);
                if (mapped_phys) pmm_free_page(mapped_phys);
            }
            rflags = spin_lock_irqsave(&g_sched_lock);
            g_stack_slots_bitmap &= ~(1ULL << slot);
            spin_unlock_irqrestore(&g_sched_lock, rflags);
            return -1;
        }
    }

    *out_guard = guard_addr;
    *out_base  = base_addr;
    *out_size  = STACK_USABLE_SIZE;
    return slot;
}

static void kstack_free(int slot, uintptr_t base_addr) {
    if (slot < 0 || slot >= MAX_KERNEL_THREADS) return;
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();

    for (size_t p = 0; p < (STACK_USABLE_SIZE / PAGE_SIZE); p++) {
        uintptr_t virt = base_addr + p * PAGE_SIZE;
        uintptr_t phys = vmm_get_physical_address(pml4, virt);
        vmm_unmap_page(pml4, virt);
        if (phys) {
            pmm_free_page(phys);
        }
    }

    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    g_stack_slots_bitmap &= ~(1ULL << slot);
    spin_unlock_irqrestore(&g_sched_lock, rflags);
}

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

void sched_reap_dead(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    tcb_t *dead = g_dead_threads;
    g_dead_threads = NULL;
    spin_unlock_irqrestore(&g_sched_lock, rflags);

    while (dead) {
        tcb_t *next = dead->next;

        /* Invariant 1: The executing thread must never be the dead thread */
        if (dead == g_current_thread) {
            serial_puts("[FATAL] sched_reap_dead: attempt to reap currently executing thread!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }

        /* Invariant 2: The executing thread must not share the dead thread's stack slot */
        if (dead->stack_slot >= 0 && dead->stack_slot == g_current_thread->stack_slot) {
            serial_puts("[FATAL] sched_reap_dead: dead thread stack slot is currently active!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }

        /* Invariant 3: The active CR3 must never be the dead process's PML4.
         * The scheduler must have already switched to the next thread's CR3 (or kernel PML4)
         * during thread_exit()/thread_yield() before the dead process could ever be reaped.
         * If active CR3 matches dead->cr3, it indicates a critical scheduler lifecycle bug. */
        if (dead->is_user && dead->cr3 != 0) {
            if (vmm_get_current_pml4() == dead->cr3) {
                serial_puts("[FATAL] sched_reap_dead: active CR3 matches dead process PML4 (lifecycle bug)!\n");
                for (;;) { __asm__ volatile("cli; hlt"); }
            }
            vmm_destroy_pml4(dead->cr3, true);
            dead->cr3 = 0;
            dead->pml4_virt = NULL;
        }
        if (dead->stack_slot >= 0) {
            kstack_free(dead->stack_slot, dead->kstack_base);
        }
        fd_close_all(dead);
        kfree(dead);
        dead = next;
    }
}

static void idle_thread_entry(void *arg) {
    (void)arg;
    for (;;) {
        sched_reap_dead();
        if (sched_ready_count() > 0) {
            thread_yield();
        } else {
            __asm__ volatile("sti; hlt");
        }
    }
}

void sched_init(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);

    memset(&g_main_thread, 0, sizeof(tcb_t));
    g_main_thread.rsp = 0; /* Captured dynamically on first switch_context */
    g_main_thread.tid = 0;
    memcpy(g_main_thread.name, "main", 5);
    g_main_thread.state = THREAD_RUNNING;
    g_main_thread.stack_slot = -1; /* Adopted crt0 boot stack */
    g_main_thread.kstack_guard = (uintptr_t)kernel_stack_guard;
    g_main_thread.kstack_base = (uintptr_t)kernel_stack_guard + 4096;
    g_main_thread.kstack_size = 16384;
    g_main_thread.ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_main_thread.total_ticks = 0;
    g_main_thread.is_idle = false;
    g_main_thread.cr3 = vmm_get_kernel_pml4();
    g_main_thread.pml4_virt = vmm_get_kernel_pml4_virt();
    g_main_thread.is_user = false;
    g_main_thread.exit_code = 0;
    g_main_thread.has_exited = false;
    g_main_thread.next = NULL;

    memset(g_exit_records, 0, sizeof(g_exit_records));

    g_current_thread = &g_main_thread;
    g_runqueue_head = NULL;
    g_runqueue_tail = NULL;
    g_dead_threads = NULL;
    g_blocked_threads = NULL;
    g_next_tid = 1;
    g_preemption_enabled = false;
    g_stack_slots_bitmap = 0;

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

    serial_puts("[ OK ] Preemptive thread scheduler initialized (page-backed stack guard armed, main adopted, idle thread ready)\n");
}

tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return NULL;

    sched_reap_dead();

    tcb_t *t = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!t) {
        serial_puts("[FAIL] thread_create: kmalloc(tcb) failed\n");
        return NULL;
    }
    memset(t, 0, sizeof(tcb_t));

    uintptr_t guard_virt = 0;
    uintptr_t stack_base = 0;
    size_t    stack_size = 0;
    int slot = kstack_alloc(&guard_virt, &stack_base, &stack_size);
    if (slot < 0) {
        serial_puts("[FAIL] thread_create: kstack_alloc failed\n");
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
    t->stack_slot = slot;
    t->kstack_guard = guard_virt;
    t->kstack_base = stack_base;
    t->kstack_size = stack_size;
    t->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    t->total_ticks = 0;
    t->is_idle = false;
    t->cr3 = 0;
    t->pml4_virt = NULL;
    t->is_user = false;
    t->exit_code = 0;
    t->has_exited = false;

    /* Setup initial stack frame to match switch_context restore sequence:
     * switch_context pops: r15, r14, r13, r12, rbp, rbx, rflags, ret (rip)
     * Total: 8 qwords = 64 bytes.
     */
    uint8_t *stack_top = (uint8_t *)(stack_base + stack_size);
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
    sched_reap_dead();

    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);

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
        /* If both old and next are user processes, track switch between runnable user processes */
        if (old->is_user && next->is_user) {
            g_sched_runnable_switches++;
        }
    }

    next->state = THREAD_RUNNING;
    next->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_current_thread = next;

    /* Update TSS.RSP0 to target thread's kernel stack with interrupts disabled */
    gdt_set_tss_rsp0(next->kstack_base + next->kstack_size);

    /* Switch CR3 to target thread's address space with interrupts disabled */
    uintptr_t target_cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
    if (vmm_get_current_pml4() != target_cr3) {
        vmm_switch_pml4(target_cr3);
    }

    /*
     * SEPARATION OF LOCK AND INTERRUPT RULES:
     * 1. The scheduler spinlock (g_sched_lock) MUST be released before switch_context()
     *    to guarantee that NO lock is held across a context switch.
     * 2. Interrupts must remain DISABLED across the entire TSS.RSP0, CR3, and switch_context
     *    stack pointer exchange. They remain disabled here because spin_lock_irqsave
     *    executed 'cli', and switch_context() executes with 'cli' until the incoming thread's
     *    saved RFLAGS is popped from its stack.
     */
    spin_unlock_noirq(&g_sched_lock);
    spin_debug_assert_unheld();

    uint64_t suspended_irq_depth = cpu_current()->irq_depth;
    cpu_current()->irq_depth = 0;
    switch_context(&old->rsp, next->rsp);
    cpu_current()->irq_depth = suspended_irq_depth;

    /* Execution resumes here when old is switched back to.
     * Restore original caller interrupt state if interrupts were enabled before yield. */
    if (rflags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }

    sched_reap_dead();
}

/* Checking the event and publishing BLOCKED are one IRQ-disabled scheduler
 * transaction. Producers cannot slip a wakeup between these operations. */
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_sched_lock);
        if (ready(arg)) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return;
        }
        tcb_t *old = g_current_thread;
        tcb_t *next = runqueue_pop_next_locked();
        if (!next) next = g_idle_thread;
        if (!old || old->is_idle || !next) {
            serial_raw_puts("[FATAL] Invalid scheduler sleep context\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        old->state = THREAD_BLOCKED;
        old->wait_channel = channel;
        old->next = g_blocked_threads;
        g_blocked_threads = old;
        next->state = THREAD_RUNNING;
        next->ticks_remaining = DEFAULT_QUANTUM_TICKS;
        g_current_thread = next;
        gdt_set_tss_rsp0(next->kstack_base + next->kstack_size);
        uintptr_t cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
        if (vmm_get_current_pml4() != cr3) vmm_switch_pml4(cr3);
        spin_unlock_noirq(&g_sched_lock);
        spin_debug_assert_unheld();
        uint64_t suspended_irq_depth = cpu_current()->irq_depth;
        cpu_current()->irq_depth = 0;
        switch_context(&old->rsp, next->rsp);
        cpu_current()->irq_depth = suspended_irq_depth;
        if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
        /* Another reader may have consumed the event before we resumed. */
    }
}

/* IRQ-safe: enqueue only; the timer/idle path performs the actual switch, so
 * the hardware handler can finish and its dispatcher can acknowledge EOI. */
void sched_wake_all(const void *channel) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    tcb_t **link = &g_blocked_threads;
    while (*link) {
        tcb_t *t = *link;
        if (t->wait_channel != channel) {
            link = &t->next;
            continue;
        }
        *link = t->next;
        t->wait_channel = NULL;
        t->state = THREAD_READY;
        runqueue_push_locked(t);
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
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

    /* Update TSS.RSP0 to target thread's kernel stack */
    gdt_set_tss_rsp0(next->kstack_base + next->kstack_size);

    /* Switch CR3 to target thread's address space */
    uintptr_t target_cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
    if (vmm_get_current_pml4() != target_cr3) {
        vmm_switch_pml4(target_cr3);
    }

    spin_unlock_noirq(&g_sched_lock);
    spin_debug_assert_unheld();

    uint64_t dummy_old_rsp = 0;
    cpu_current()->irq_depth = 0; /* Exiting context never resumes. */
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

uint64_t sched_get_active_stack_slots_mask(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    uint64_t mask = g_stack_slots_bitmap;
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return mask;
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
            thread_yield();
        }
        return;
    }

    /* Timeslice accounting for normal threads */
    if (--g_current_thread->ticks_remaining <= 0) {
        g_current_thread->ticks_remaining = DEFAULT_QUANTUM_TICKS;

        /* If other threads are ready to run: preempt! */
        if (g_runqueue_head != NULL) {
            g_current_thread->preempt_count++;
            g_sched_timer_preemptions++;
            thread_yield();
        }
    }
}

uint64_t sched_get_timer_preempt_count(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    uint64_t val = g_sched_timer_preemptions;
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return val;
}

uint64_t sched_get_runnable_switches_count(void) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    uint64_t val = g_sched_runnable_switches;
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return val;
}

/* System V AMD64 ABI, Section 3.4.1 "Initial Stack and Register State":
 *   - RSP must be 16-byte aligned at _start entry (RSP % 16 == 0), matching glibc/musl expectations.
 *   - Layout: argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL.
 *   - Strings live at higher addresses than the pointer table.
 *   Do not "simplify" this without reading the spec. */
int process_setup_user_stack(uintptr_t stack_phys, int argc, const char *const argv[],
                             uintptr_t *out_user_rsp, uintptr_t *out_user_argv) {
    if (!stack_phys || !out_user_rsp || !out_user_argv) return -1;
    if (argc < 0 || argc > MAX_SPAWN_ARGS) return -1;

    uint8_t *stack_mem = (uint8_t *)vmm_phys_to_virt(stack_phys);

    if (argc == 0 || !argv) {
        /* Minimal empty stack frame with argc=0 */
        size_t table_bytes = 5 * sizeof(uint64_t);
        uintptr_t rsp = (USER_STACK_TOP_VIRT - table_bytes) & ~0xFULL;
        size_t page_offset = (size_t)(rsp - USER_STACK_PAGE_VIRT);
        uint64_t *table = (uint64_t *)(stack_mem + page_offset);
        table[0] = 0; /* argc = 0 */
        table[1] = 0; /* argv[0] = NULL */
        table[2] = 0; /* envp[0] = NULL */
        table[3] = 0; /* AT_NULL a_type = 0 */
        table[4] = 0; /* AT_NULL a_val = 0 */
        *out_user_rsp = rsp;
        *out_user_argv = rsp + sizeof(uint64_t);
        return 0;
    }

    /* 1. Calculate total string length including NUL terminators */
    size_t total_str_len = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return -1;
        size_t len = strlen(argv[i]) + 1;
        if (len > MAX_ARG_STRLEN) return -1;
        total_str_len += len;
    }
    if (total_str_len > MAX_TOTAL_ARGS_LEN) return -1;

    /* 2. Copy strings to high end of user stack page:
     * stack_top = USER_STACK_TOP_VIRT (one past end of page).
     * The last written byte is stack_mem[PAGE_SIZE - 1] (USER_STACK_TOP_VIRT - 1),
     * completely inside the allocated physical frame. */
    uintptr_t user_str_ptrs[MAX_SPAWN_ARGS];
    size_t cur_offset = PAGE_SIZE - total_str_len;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (cur_offset + len > PAGE_SIZE) return -1;
        memcpy(stack_mem + cur_offset, argv[i], len);
        user_str_ptrs[i] = USER_STACK_PAGE_VIRT + cur_offset;
        cur_offset += len;
    }
    if (cur_offset != PAGE_SIZE) return -1;

    /* 3. Compute 16-byte aligned RSP below strings for:
     *    argc (1) + argv[0..argc-1] (argc) + NULL (1) + envp NULL (1) + AT_NULL (2) = argc + 5
     */
    size_t table_entries = (size_t)(argc + 5);
    size_t table_bytes = table_entries * sizeof(uint64_t);
    uintptr_t str_virt_start = USER_STACK_TOP_VIRT - total_str_len;
    uintptr_t rsp = (str_virt_start - table_bytes) & ~0xFULL;

    /* Check bounds: RSP must be >= USER_STACK_PAGE_VIRT */
    if (rsp < USER_STACK_PAGE_VIRT) return -1;

    size_t table_page_offset = (size_t)(rsp - USER_STACK_PAGE_VIRT);
    uint64_t *table = (uint64_t *)(stack_mem + table_page_offset);

    size_t idx = 0;
    table[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        table[idx++] = (uint64_t)user_str_ptrs[i];
    }
    table[idx++] = 0; /* argv[argc] = NULL */
    table[idx++] = 0; /* envp[0] = NULL */
    table[idx++] = 0; /* AT_NULL a_type */
    table[idx++] = 0; /* AT_NULL a_val */

    *out_user_rsp = rsp;
    *out_user_argv = rsp + sizeof(uint64_t);
    return 0;
}

static tcb_t *process_spawn_internal(const char *name, const void *elf_data, size_t elf_size,
                                     int argc, const char *const argv[],
                                     uint64_t scalar_arg, int64_t *error) {
    *error = SYSCALL_ENOMEM;
    if (!elf_data || elf_size == 0) return NULL;

    sched_reap_dead();

    /* 1. Load ELF executable into a freshly created user address space */
    elf_loaded_process_t proc_info;
    int elf_status = elf_load_executable(elf_data, elf_size, &proc_info);
    if (elf_status != ELF_OK) {
        *error = elf_status == ELF_ERR_NOMEM ? SYSCALL_ENOMEM : SYSCALL_ENOEXEC;
        serial_puts("[EXEC] ELF loader rejected image with code ");
        serial_print_dec(elf_status);
        serial_puts("\n");
        return NULL;
    }

    /* 2. Setup user stack */
    uintptr_t user_rsp = 0;
    uintptr_t user_argv = 0;
    uint64_t rdi_val = 0;
    uint64_t rsi_val = 0;

    if (argv != NULL) {
        int setup_res = process_setup_user_stack(proc_info.stack_phys, argc, argv,
                                                &user_rsp, &user_argv);
        if (setup_res != 0) {
            *error = SYSCALL_E2BIG;
            vmm_destroy_pml4(proc_info.pml4_phys, true);
            return NULL;
        }
        rdi_val = (uint64_t)argc;
        rsi_val = (uint64_t)user_argv;
    } else {
        /* Compatibility fallback for kernel-internal scalar spawns (e.g. init.asm tests) */
        user_rsp = proc_info.user_stack_top & ~0xFULL;
        rdi_val = scalar_arg;
        rsi_val = 0;
    }

    /* 3. Allocate dedicated page-backed kernel stack */
    uintptr_t guard_virt = 0, stack_base = 0;
    size_t stack_size = 0;
    int slot = kstack_alloc(&guard_virt, &stack_base, &stack_size);
    if (slot < 0) {
        serial_puts("[FAIL] process_spawn: kstack_alloc failed\n");
        vmm_destroy_pml4(proc_info.pml4_phys, true);
        return NULL;
    }

    /* 4. Allocate Process / Thread Control Block */
    tcb_t *p = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!p) {
        serial_puts("[FAIL] process_spawn: kmalloc(tcb) failed\n");
        kstack_free(slot, stack_base);
        vmm_destroy_pml4(proc_info.pml4_phys, true);
        return NULL;
    }
    memset(p, 0, sizeof(tcb_t));

    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    p->tid = g_next_tid++;
    spin_unlock_irqrestore(&g_sched_lock, rflags);

    if (name) {
        size_t len = strlen(name);
        if (len >= sizeof(p->name)) len = sizeof(p->name) - 1;
        memcpy(p->name, name, len);
        p->name[len] = '\0';
    } else {
        memcpy(p->name, "user_proc", 10);
    }

    p->state = THREAD_READY;
    p->stack_slot = slot;
    p->kstack_guard = guard_virt;
    p->kstack_base = stack_base;
    p->kstack_size = stack_size;
    p->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    p->total_ticks = 0;
    p->is_idle = false;

    /* Process specifics */
    p->cr3 = proc_info.pml4_phys;
    p->pml4_virt = (uint64_t *)vmm_phys_to_virt(proc_info.pml4_phys);
    p->is_user = true;
    p->exit_code = 0;
    p->has_exited = false;

    /* 5. Set up initial kernel stack frame for first context switch to user_process_trampoline */
    uint8_t *stack_top = (uint8_t *)(stack_base + stack_size);
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);

    stack_top -= sizeof(uint64_t) * 8;
    uint64_t *frame = (uint64_t *)stack_top;

    frame[0] = rsi_val;                            /* r15 -> passed to user RSI (argv) */
    frame[1] = rdi_val;                            /* r14 -> passed to user RDI (argc or scalar arg) */
    frame[2] = (uint64_t)user_rsp;                 /* r13 -> user RSP */
    frame[3] = (uint64_t)proc_info.entry_point;    /* r12 -> user RIP */
    frame[4] = 0;                                  /* rbp */
    frame[5] = 0;                                  /* rbx */
    frame[6] = 0x202;                              /* rflags: IF=1 */
    frame[7] = (uint64_t)user_process_trampoline;  /* rip */

    p->rsp = (uint64_t)stack_top;

    rflags = spin_lock_irqsave(&g_sched_lock);
    runqueue_push_locked(p);
    spin_unlock_irqrestore(&g_sched_lock, rflags);

    return p;
}

tcb_t *process_spawn_with_arg(const char *name, const void *elf_data, size_t elf_size, uint64_t arg) {
    int64_t error;
    return process_spawn_internal(name, elf_data, elf_size, 0, NULL, arg, &error);
}

int64_t process_spawn_from_vfs(const char *path, int argc, const char *const argv[], int64_t *out_pid) {
    if (!path || !*path || !out_pid) return SYSCALL_EINVAL;
    /* Keep publication and PID capture atomic on this bootstrap-only CPU.
     * No scheduler lock is held across filesystem or loader operations. */
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    int64_t result = SYSCALL_ENOMEM;
    child_record_t *record = NULL;
    file_t *file = NULL;
    void *buffer = NULL;
    if (!g_current_thread || !g_current_thread->is_user) {
        result = SYSCALL_EINVAL;
        goto out;
    }
    for (unsigned i = 0; i < MAX_EXIT_RECORDS; i++) {
        if (!g_child_records[i].used) { record = &g_child_records[i]; break; }
    }
    if (!record) goto out;
    int err = 0;
    file = vfs_open_ext(path, VFS_O_RDONLY, &err);
    if (!file) {
        result = err == -VFS_ENOENT ? SYSCALL_ENOENT :
                 err == -VFS_ENOMEM ? SYSCALL_ENOMEM : SYSCALL_EIO;
        goto out;
    }
    if (file->node->type != VFS_FILE) { result = SYSCALL_EISDIR; goto out; }
    size_t size = file->node->size;
    if (!size) { result = SYSCALL_ENOEXEC; goto out; }
    if (size > MAX_ELF_FILE_SIZE) { result = SYSCALL_EFBIG; goto out; }
    const void *image = file->node->data;
    if (!image) {
        buffer = kmalloc(size);
        if (!buffer) goto out;
        size_t read = 0;
        while (read < size) {
            int64_t n = vfs_read(file, (uint8_t *)buffer + read, size - read);
            if (n <= 0 || (uint64_t)n > size - read) {
                result = n == -VFS_ENOMEM ? SYSCALL_ENOMEM : SYSCALL_EIO;
                goto out;
            }
            read += (size_t)n;
        }
        image = buffer;
    }
    tcb_t *child = process_spawn_internal(path, image, size, argc, argv, 0, &result);
    if (!child) goto out;
    *record = (child_record_t){ .parent = g_current_thread->tid,
                              .pid = child->tid, .used = true };
    *out_pid = (int64_t)child->tid;
    result = SYSCALL_SUCCESS;
out:
    if (buffer) kfree(buffer);
    if (file) vfs_close(file);
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
    return result;
}

/* Called only under sched lock: no nested public scheduler calls. */
static bool child_done(void *arg) {
    return ((child_record_t *)arg)->done;
}

bool process_wait_child(uint64_t pid, uint64_t *out_exit_code) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    child_record_t *record = NULL;
    for (unsigned i = 0; i < MAX_EXIT_RECORDS; i++) {
        if (g_child_records[i].used && g_child_records[i].pid == pid &&
            g_child_records[i].parent == g_current_thread->tid) {
            record = &g_child_records[i];
            break;
        }
    }
    spin_unlock_noirq(&g_sched_lock);
    if (record) {
        sched_wait_until(g_child_records, child_done, record);
        /* Parent is single-threaded and records cannot be evicted. */
        if (out_exit_code) *out_exit_code = record->status;
        record->used = false;
        sched_reap_dead();
    }
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
    return record != NULL;
}

tcb_t *process_spawn(const char *name, const void *elf_data, size_t elf_size) {
    return process_spawn_with_arg(name, elf_data, elf_size, 0);
}

int fd_alloc(tcb_t *proc, struct file *file) {
    if (!proc || !file) return -1;
    for (int i = 3; i < 32; i++) {
        if (proc->fd_table[i] == NULL) {
            proc->fd_table[i] = file;
            return i;
        }
    }
    return -6; /* EMFILE: Too many open files */
}

struct file *fd_get(tcb_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= 32) return NULL;
    return proc->fd_table[fd];
}

int fd_free(tcb_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= 32 || !proc->fd_table[fd]) return -1;
    vfs_close(proc->fd_table[fd]);
    proc->fd_table[fd] = NULL;
    return 0;
}

void fd_close_all(tcb_t *proc) {
    if (!proc) return;
    for (int i = 0; i < 32; i++) {
        if (proc->fd_table[i]) {
            vfs_close(proc->fd_table[i]);
            proc->fd_table[i] = NULL;
        }
    }
}

void process_exit(uint64_t exit_code) {
    /* Do not allow a woken parent to run/reap before thread_exit detaches us. */
    __asm__ volatile("cli" ::: "memory");
    tcb_t *curr = thread_current();
    if (curr) {
        /* Close all open descriptors and release references upon process exit */
        fd_close_all(curr);

        curr->exit_code = exit_code;
        curr->has_exited = true;

        /* Record in bounded circular exit records under lock */
        uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
        bool child_recorded = false;
        for (unsigned i = 0; i < MAX_EXIT_RECORDS; i++) {
            child_record_t *record = &g_child_records[i];
            if (!record->used) continue;
            if (record->pid == curr->tid) {
                record->status = exit_code;
                record->done = true;
                child_recorded = true;
            }
            /* Orphans continue running, but their parent can no longer wait. */
            if (record->parent == curr->tid) record->used = false;
        }
        if (!child_recorded) {
        int slot = -1;
        for (int i = 0; i < MAX_EXIT_RECORDS; i++) {
            if (!g_exit_records[i].valid) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            /* All slots full: replace oldest record in circular FIFO order */
            slot = (int)(g_exit_records_head % MAX_EXIT_RECORDS);
            g_exit_records_head++;
        }
        g_exit_records[slot].pid = curr->tid;
        g_exit_records[slot].exit_code = exit_code;
        g_exit_records[slot].preempt_count = curr->preempt_count;
        g_exit_records[slot].total_ticks = curr->total_ticks;
        g_exit_records[slot].valid = true;
        }
        spin_unlock_irqrestore(&g_sched_lock, rflags);
        sched_wake_all(g_child_records);
    }
    thread_exit();
}

bool process_is_alive(uint64_t pid) {
    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
    if (g_current_thread && g_current_thread->tid == pid && g_current_thread->state != THREAD_TERMINATED) {
        spin_unlock_irqrestore(&g_sched_lock, rflags);
        return true;
    }
    tcb_t *c = g_runqueue_head;
    while (c) {
        if (c->tid == pid && c->state != THREAD_TERMINATED) {
            spin_unlock_irqrestore(&g_sched_lock, rflags);
            return true;
        }
        c = c->next;
    }
    for (c = g_blocked_threads; c; c = c->next) {
        if (c->tid == pid) {
            spin_unlock_irqrestore(&g_sched_lock, rflags);
            return true;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, rflags);
    return false;
}

bool process_wait_extended(uint64_t pid, uint64_t *out_exit_code, uint64_t *out_preempt_count, uint64_t *out_total_ticks) {
    for (;;) {
        uint64_t rflags = spin_lock_irqsave(&g_sched_lock);

        /* 1. Check if captured in exit records */
        for (int i = 0; i < MAX_EXIT_RECORDS; i++) {
            if (g_exit_records[i].valid && g_exit_records[i].pid == pid) {
                if (out_exit_code) *out_exit_code = g_exit_records[i].exit_code;
                if (out_preempt_count) *out_preempt_count = g_exit_records[i].preempt_count;
                if (out_total_ticks) *out_total_ticks = g_exit_records[i].total_ticks;
                g_exit_records[i].valid = false;
                spin_unlock_irqrestore(&g_sched_lock, rflags);
                return true;
            }
        }

        /* 2. Check if process is still alive */
        bool alive = false;
        if (g_current_thread && g_current_thread->tid == pid && g_current_thread->state != THREAD_TERMINATED) {
            alive = true;
        } else {
            tcb_t *c = g_runqueue_head;
            while (c) {
                if (c->tid == pid && c->state != THREAD_TERMINATED) {
                    alive = true;
                    break;
                }
                c = c->next;
            }
        }
        for (tcb_t *c = g_blocked_threads; c; c = c->next) {
            if (c->tid == pid) alive = true;
        }
        spin_unlock_irqrestore(&g_sched_lock, rflags);

        if (!alive) {
            /* Process not found in alive queues or exit records */
            return false;
        }

        /* Yield CPU to allow the process or reaper to make progress */
        thread_yield();
    }
}

bool process_wait(uint64_t pid, uint64_t *out_exit_code) {
    return process_wait_extended(pid, out_exit_code, NULL, NULL);
}
