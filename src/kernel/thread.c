#include "thread.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "serial.h"
#include "gdt.h"
#include "elf.h"
#include "vfs.h"

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
static exit_record_t g_exit_records[MAX_EXIT_RECORDS];
static size_t        g_exit_records_head = 0;

static uint64_t      g_sched_timer_preemptions = 0;
static uint64_t      g_sched_runnable_switches = 0;

static tcb_t         g_main_thread;
static tcb_t        *g_idle_thread        = NULL;
static tcb_t        *g_current_thread     = NULL;
static tcb_t        *g_runqueue_head      = NULL;
static tcb_t        *g_runqueue_tail      = NULL;
static tcb_t        *g_blocked_threads    = NULL;
static tcb_t        *g_dead_threads       = NULL;
static uint64_t      g_next_tid           = 1;
static spinlock_t    g_sched_lock         = SPINLOCK_RANKED(1, "sched");
static volatile bool g_preemption_enabled = false;

/* 64-slot Page-Backed Thread Stack Allocator */
static uint64_t      g_stack_slots_bitmap = 0;

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

    switch_context(&old->rsp, next->rsp);

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
        switch_context(&old->rsp, next->rsp);
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

tcb_t *process_spawn_with_arg(const char *name, const void *elf_data, size_t elf_size, uint64_t arg) {
    if (!elf_data || elf_size == 0) return NULL;

    sched_reap_dead();

    /* 1. Load ELF executable into a freshly created user address space */
    elf_loaded_process_t proc_info;
    int elf_status = elf_load_executable(elf_data, elf_size, &proc_info);
    if (elf_status != ELF_OK) {
        serial_puts("[FAIL] process_spawn: elf_load_executable failed with code ");
        serial_print_dec(elf_status);
        serial_puts("\n");
        return NULL;
    }

    /* 2. Allocate dedicated page-backed kernel stack */
    uintptr_t guard_virt = 0, stack_base = 0;
    size_t stack_size = 0;
    int slot = kstack_alloc(&guard_virt, &stack_base, &stack_size);
    if (slot < 0) {
        serial_puts("[FAIL] process_spawn: kstack_alloc failed\n");
        vmm_destroy_pml4(proc_info.pml4_phys, true);
        return NULL;
    }

    /* 3. Allocate Process / Thread Control Block */
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

    /* 4. Set up initial kernel stack frame for first context switch to user_process_trampoline */
    uint8_t *stack_top = (uint8_t *)(stack_base + stack_size);
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);

    stack_top -= sizeof(uint64_t) * 8;
    uint64_t *frame = (uint64_t *)stack_top;

    frame[0] = 0;                                  /* r15 */
    frame[1] = arg;                                /* r14 -> passed to user RDI */
    frame[2] = (uint64_t)proc_info.user_stack_top; /* r13 -> user RSP */
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
    tcb_t *curr = thread_current();
    if (curr) {
        /* Close all open descriptors and release references upon process exit */
        fd_close_all(curr);

        curr->exit_code = exit_code;
        curr->has_exited = true;

        /* Record in bounded circular exit records under lock */
        uint64_t rflags = spin_lock_irqsave(&g_sched_lock);
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
        spin_unlock_irqrestore(&g_sched_lock, rflags);
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
