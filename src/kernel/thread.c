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
#include "apic.h"
#include "smp.h"

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

/* SMP Piece 4: Per-CPU scheduler state */
/* SMP Piece 4: Per-CPU scheduler state */
static struct scheduler_cpu {
    exit_record_t exit_records[MAX_EXIT_RECORDS];
    size_t        exit_records_head;
    child_record_t child_records[MAX_EXIT_RECORDS];
    uint64_t      sched_runnable_switches;
    tcb_t         main_thread;
    tcb_t        *idle_thread;
    tcb_t        *runqueue_head;
    tcb_t        *runqueue_tail;
    tcb_t        *blocked_threads;
    tcb_t        *dead_threads;
    tcb_t        *zombie_thread;
    uint64_t      next_tid;
    spinlock_t    sched_lock;
    volatile bool preemption_enabled;
    uint64_t      stack_slots_bitmap;
    uint64_t      stolen_tasks_count;
    /* CPU-private transient handoff across switch_context with interrupts disabled.
     * Written by this CPU before switch_context, consumed by this CPU in sched_post_switch.
     * Never accessed by remote CPUs. */
    uintptr_t     prev_cr3;
    bool          prev_terminated;
    bool          prev_cr3_changed;
} scheduler_cpus[MAX_DETECTED_CPUS] = { [0] = { .next_tid = 1, .sched_lock = SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, "sched-0") } };

static const char *g_sched_lock_names[MAX_DETECTED_CPUS] = {
    "sched-0", "sched-1", "sched-2", "sched-3", "sched-4", "sched-5", "sched-6", "sched-7",
    "sched-8", "sched-9", "sched-10", "sched-11", "sched-12", "sched-13", "sched-14", "sched-15",
    "sched-16", "sched-17", "sched-18", "sched-19", "sched-20", "sched-21", "sched-22", "sched-23",
    "sched-24", "sched-25", "sched-26", "sched-27", "sched-28", "sched-29", "sched-30", "sched-31",
    "sched-32", "sched-33", "sched-34", "sched-35", "sched-36", "sched-37", "sched-38", "sched-39",
    "sched-40", "sched-41", "sched-42", "sched-43", "sched-44", "sched-45", "sched-46", "sched-47",
    "sched-48", "sched-49", "sched-50", "sched-51", "sched-52", "sched-53", "sched-54", "sched-55",
    "sched-56", "sched-57", "sched-58", "sched-59", "sched-60", "sched-61", "sched-62", "sched-63"
};

static uint64_t g_global_next_tid = 1;
static size_t g_total_sched_cpus = 1;
static spinlock_t g_kstack_lock = SPINLOCK_RANKED(1, "kstack");

/* Read-only debug metadata for host tests; these are addresses, not mirrors. */
const uintptr_t scheduler_debug_bsp[] = {
    (uintptr_t)&scheduler_cpus[0].blocked_threads,
    (uintptr_t)&cpu_locals[0].current_thread,
    (uintptr_t)&scheduler_cpus[0].stack_slots_bitmap
};
#define g_exit_records (scheduler_cpus[cpu_current()->id].exit_records)
#define g_exit_records_head (scheduler_cpus[cpu_current()->id].exit_records_head)
#define g_child_records (scheduler_cpus[cpu_current()->id].child_records)
#define g_sched_timer_preemptions (cpu_current()->preempt_count)
#define g_sched_runnable_switches (scheduler_cpus[cpu_current()->id].sched_runnable_switches)
#define g_main_thread (scheduler_cpus[cpu_current()->id].main_thread)
#define g_idle_thread (scheduler_cpus[cpu_current()->id].idle_thread)
#define g_runqueue_head (scheduler_cpus[cpu_current()->id].runqueue_head)
#define g_runqueue_tail (scheduler_cpus[cpu_current()->id].runqueue_tail)
#define g_blocked_threads (scheduler_cpus[cpu_current()->id].blocked_threads)
#define g_dead_threads (scheduler_cpus[cpu_current()->id].dead_threads)
#define g_next_tid (scheduler_cpus[cpu_current()->id].next_tid)
#define g_sched_lock (scheduler_cpus[cpu_current()->id].sched_lock)
#define g_preemption_enabled (scheduler_cpus[cpu_current()->id].preemption_enabled)
#define g_stack_slots_bitmap (scheduler_cpus[0].stack_slots_bitmap)
#define g_current_thread (cpu_current()->current_thread)

void sched_lock_pair(spinlock_t *a, spinlock_t *b) {
    if (a == b) {
        spin_lock_noirq(a);
        return;
    }
    spinlock_t *first = ((uintptr_t)a < (uintptr_t)b) ? a : b;
    spinlock_t *second = ((uintptr_t)a < (uintptr_t)b) ? b : a;
    spin_lock_noirq(first);
    spin_lock_noirq(second);
}

void sched_unlock_pair(spinlock_t *a, spinlock_t *b) {
    if (a == b) {
        spin_unlock_noirq(a);
        return;
    }
    spinlock_t *first = ((uintptr_t)a < (uintptr_t)b) ? a : b;
    spinlock_t *second = ((uintptr_t)a < (uintptr_t)b) ? b : a;
    spin_unlock_noirq(second);
    spin_unlock_noirq(first);
}

static int kstack_alloc(uintptr_t *out_guard, uintptr_t *out_base, size_t *out_size) {
    uint64_t rflags = spin_lock_irqsave(&g_kstack_lock);
    int slot = -1;
    for (int i = 0; i < MAX_KERNEL_THREADS; i++) {
        if (!(g_stack_slots_bitmap & (1ULL << i))) {
            g_stack_slots_bitmap |= (1ULL << i);
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_kstack_lock, rflags);

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
            rflags = spin_lock_irqsave(&g_kstack_lock);
            g_stack_slots_bitmap &= ~(1ULL << slot);
            spin_unlock_irqrestore(&g_kstack_lock, rflags);
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
            rflags = spin_lock_irqsave(&g_kstack_lock);
            g_stack_slots_bitmap &= ~(1ULL << slot);
            spin_unlock_irqrestore(&g_kstack_lock, rflags);
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

    uint64_t rflags = spin_lock_irqsave(&g_kstack_lock);
    g_stack_slots_bitmap &= ~(1ULL << slot);
    spin_unlock_irqrestore(&g_kstack_lock, rflags);
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
    if (cpu_current()->id != 0) return;

    size_t limit = g_total_sched_cpus ? g_total_sched_cpus : 1;
    for (size_t c = 0; c < limit; c++) {
        uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[c].sched_lock);
        tcb_t *dead = scheduler_cpus[c].dead_threads;
        scheduler_cpus[c].dead_threads = NULL;
        spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);

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

    vmm_drain_deferred_destructions();
}

void sched_post_switch(void) {
    size_t cid = cpu_current()->id;
    if (cid < MAX_DETECTED_CPUS) {
        uintptr_t prev_cr3 = scheduler_cpus[cid].prev_cr3;
        bool prev_term = scheduler_cpus[cid].prev_terminated;
        bool cr3_changed = scheduler_cpus[cid].prev_cr3_changed;
        scheduler_cpus[cid].prev_cr3 = 0;
        scheduler_cpus[cid].prev_terminated = false;
        scheduler_cpus[cid].prev_cr3_changed = false;

        if (prev_cr3 != 0) {
            vmm_space_leave(prev_cr3, prev_term, cr3_changed);
        }

        if (scheduler_cpus[cid].zombie_thread) {
            tcb_t *z = scheduler_cpus[cid].zombie_thread;
            scheduler_cpus[cid].zombie_thread = NULL;
            uint64_t zflags = spin_lock_irqsave(&scheduler_cpus[cid].sched_lock);
            z->next = scheduler_cpus[cid].dead_threads;
            scheduler_cpus[cid].dead_threads = z;
            spin_unlock_irqrestore(&scheduler_cpus[cid].sched_lock, zflags);
        }
    }
}

static void idle_thread_entry(void *arg) {
    (void)arg;
    for (;;) {
        sched_reap_dead();
        thread_yield();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

static bool sched_steal_work(size_t thief_cpu) {
    if (g_total_sched_cpus <= 1) return false;

    for (size_t i = 0; i < g_total_sched_cpus; i++) {
        size_t victim_cpu = (thief_cpu + 1 + i) % g_total_sched_cpus;
        if (victim_cpu == thief_cpu) continue;

        if (!__atomic_load_n(&scheduler_cpus[victim_cpu].runqueue_head, __ATOMIC_RELAXED)) {
            continue;
        }

        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");

        sched_lock_pair(&scheduler_cpus[thief_cpu].sched_lock,
                        &scheduler_cpus[victim_cpu].sched_lock);

        tcb_t *prev = NULL;
        tcb_t *curr = scheduler_cpus[victim_cpu].runqueue_head;
        tcb_t *candidate = NULL;
        tcb_t *cand_prev = NULL;

        while (curr) {
            if (!curr->is_idle &&
                (curr->cpu_affinity == -1 || curr->cpu_affinity == (int)thief_cpu)) {
                candidate = curr;
                cand_prev = prev;
            }
            prev = curr;
            curr = curr->next;
        }

        if (candidate) {
            if (cand_prev) {
                cand_prev->next = candidate->next;
            } else {
                scheduler_cpus[victim_cpu].runqueue_head = candidate->next;
            }
            if (scheduler_cpus[victim_cpu].runqueue_tail == candidate) {
                scheduler_cpus[victim_cpu].runqueue_tail = cand_prev;
            }
            candidate->next = NULL;
            candidate->current_cpu = thief_cpu;

            if (!scheduler_cpus[thief_cpu].runqueue_head) {
                scheduler_cpus[thief_cpu].runqueue_head = candidate;
                scheduler_cpus[thief_cpu].runqueue_tail = candidate;
            } else {
                scheduler_cpus[thief_cpu].runqueue_tail->next = candidate;
                scheduler_cpus[thief_cpu].runqueue_tail = candidate;
            }
            scheduler_cpus[thief_cpu].stolen_tasks_count++;

            sched_unlock_pair(&scheduler_cpus[thief_cpu].sched_lock,
                              &scheduler_cpus[victim_cpu].sched_lock);
            __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
            return true;
        }

        sched_unlock_pair(&scheduler_cpus[thief_cpu].sched_lock,
                          &scheduler_cpus[victim_cpu].sched_lock);
        __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
    }

    return false;
}

void sched_init(void) {
    for (size_t i = 0; i < MAX_DETECTED_CPUS; i++) {
        scheduler_cpus[i].sched_lock = (spinlock_t)SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, g_sched_lock_names[i]);
    }

    uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[0].sched_lock);

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
    g_main_thread.cpu_affinity = 0;
    g_main_thread.current_cpu = 0;

    memset(g_exit_records, 0, sizeof(g_exit_records));

    g_current_thread = &g_main_thread;
    g_runqueue_head = NULL;
    g_runqueue_tail = NULL;
    g_dead_threads = NULL;
    g_blocked_threads = NULL;
    g_next_tid = 1;
    g_preemption_enabled = false;
    g_stack_slots_bitmap = 0;

    spin_unlock_irqrestore(&scheduler_cpus[0].sched_lock, rflags);

    /* Create dedicated low-power idle thread */
    g_idle_thread = thread_create("idle", idle_thread_entry, NULL);
    if (g_idle_thread) {
        rflags = spin_lock_irqsave(&scheduler_cpus[0].sched_lock);
        g_idle_thread->is_idle = true;
        g_idle_thread->cpu_affinity = 0;
        g_idle_thread->current_cpu = 0;
        /* Remove idle thread from normal runqueue so it only runs when queue is empty */
        if (g_runqueue_head == g_idle_thread) {
            g_runqueue_head = g_idle_thread->next;
            if (!g_runqueue_head) g_runqueue_tail = NULL;
            g_idle_thread->next = NULL;
        }
        spin_unlock_irqrestore(&scheduler_cpus[0].sched_lock, rflags);
    }

    serial_puts("[ OK ] Preemptive thread scheduler initialized (page-backed stack guard armed, main adopted, idle thread ready)\n");
}

static tcb_t *thread_create_internal(size_t target_cpu, int affinity, const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return NULL;
    if (target_cpu >= MAX_DETECTED_CPUS) target_cpu = 0;

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

    t->tid = __atomic_fetch_add(&g_global_next_tid, 1, __ATOMIC_RELAXED);
    t->cpu_affinity = affinity;
    t->current_cpu = target_cpu;

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

    uint8_t *stack_top = (uint8_t *)(stack_base + stack_size);
    stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);
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

    uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[target_cpu].sched_lock);
    if (!scheduler_cpus[target_cpu].runqueue_head) {
        scheduler_cpus[target_cpu].runqueue_head = t;
        scheduler_cpus[target_cpu].runqueue_tail = t;
    } else {
        scheduler_cpus[target_cpu].runqueue_tail->next = t;
        scheduler_cpus[target_cpu].runqueue_tail = t;
    }
    spin_unlock_irqrestore(&scheduler_cpus[target_cpu].sched_lock, rflags);

    if (target_cpu != cpu_current()->id) {
        smp_send_resched(target_cpu);
    }
    return t;
}

tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg) {
    return thread_create_internal(cpu_current()->id, -1, name, entry, arg);
}

tcb_t *thread_create_on_cpu(size_t target_cpu, const char *name, void (*entry)(void *), void *arg) {
    return thread_create_internal(target_cpu, (int)target_cpu, name, entry, arg);
}

tcb_t *thread_create_unbound_on_cpu(size_t target_cpu, const char *name, void (*entry)(void *), void *arg) {
    return thread_create_internal(target_cpu, -1, name, entry, arg);
}

void thread_yield(void) {
    sched_reap_dead();

    uint64_t rflags = spin_lock_irqsave(&g_sched_lock);

    tcb_t *old = g_current_thread;
    tcb_t *next = runqueue_pop_next_locked();

    if (!next && g_total_sched_cpus > 1) {
        spin_unlock_irqrestore(&g_sched_lock, rflags);
        if (sched_steal_work(cpu_current()->id)) {
            rflags = spin_lock_irqsave(&g_sched_lock);
            next = runqueue_pop_next_locked();
        } else {
            rflags = spin_lock_irqsave(&g_sched_lock);
        }
    }

    if (!next) {
        /* No other runnable threads: keep running current thread without context switch */
        spin_unlock_irqrestore(&g_sched_lock, rflags);
        return;
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
    uintptr_t old_cr3 = (old && old->cr3) ? old->cr3 : vmm_get_kernel_pml4();
    uintptr_t target_cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
    bool cr3_changed = (vmm_get_current_pml4() != target_cr3);
    if (cr3_changed) {
        vmm_space_enter(target_cr3);
        vmm_switch_pml4(target_cr3);
    }

    size_t cid = cpu_current()->id;
    if (cid < MAX_DETECTED_CPUS) {
        scheduler_cpus[cid].prev_cr3 = old_cr3;
        scheduler_cpus[cid].prev_terminated = false;
        scheduler_cpus[cid].prev_cr3_changed = cr3_changed;
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

    sched_post_switch();

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
        uintptr_t old_cr3 = (old && old->cr3) ? old->cr3 : vmm_get_kernel_pml4();
        uintptr_t cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
        bool cr3_changed = (vmm_get_current_pml4() != cr3);
        if (cr3_changed) {
            vmm_space_enter(cr3);
            vmm_switch_pml4(cr3);
        }

        size_t cid = cpu_current()->id;
        if (cid < MAX_DETECTED_CPUS) {
            scheduler_cpus[cid].prev_cr3 = old_cr3;
            scheduler_cpus[cid].prev_terminated = false;
            scheduler_cpus[cid].prev_cr3_changed = cr3_changed;
        }
        spin_unlock_noirq(&g_sched_lock);
        spin_debug_assert_unheld();
        uint64_t suspended_irq_depth = cpu_current()->irq_depth;
        cpu_current()->irq_depth = 0;
        switch_context(&old->rsp, next->rsp);
        cpu_current()->irq_depth = suspended_irq_depth;

        sched_post_switch();
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

    if (curr->is_user && curr->cr3 != 0) {
        vmm_space_retire(curr->cr3);
    }

    /* Stash as zombie_thread on this CPU. It will be moved to dead_threads
     * only AFTER the context switch to the next thread has completed. */
    size_t cid = cpu_current()->id;
    if (cid < MAX_DETECTED_CPUS) {
        scheduler_cpus[cid].zombie_thread = curr;
    }

    tcb_t *next = runqueue_pop_next_locked();
    if (!next) {
        if (g_idle_thread && curr != g_idle_thread) {
            next = g_idle_thread;
        } else {
            serial_raw_puts("[FATAL] All threads terminated; no runnable threads remaining!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    next->state = THREAD_RUNNING;
    next->ticks_remaining = DEFAULT_QUANTUM_TICKS;
    g_current_thread = next;

    /* Update TSS.RSP0 to target thread's kernel stack */
    gdt_set_tss_rsp0(next->kstack_base + next->kstack_size);

    /* Switch CR3 to target thread's address space */
    uintptr_t old_cr3 = (curr && curr->cr3) ? curr->cr3 : vmm_get_kernel_pml4();
    uintptr_t target_cr3 = next->cr3 ? next->cr3 : vmm_get_kernel_pml4();
    bool cr3_changed = (vmm_get_current_pml4() != target_cr3);
    if (cr3_changed) {
        vmm_space_enter(target_cr3);
        vmm_switch_pml4(target_cr3);
    }

    if (cid < MAX_DETECTED_CPUS) {
        scheduler_cpus[cid].prev_cr3 = old_cr3;
        scheduler_cpus[cid].prev_terminated = true;
        scheduler_cpus[cid].prev_cr3_changed = cr3_changed;
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
    uint64_t rflags = spin_lock_irqsave(&g_kstack_lock);
    uint64_t mask = g_stack_slots_bitmap;
    spin_unlock_irqrestore(&g_kstack_lock, rflags);
    return mask;
}

size_t sched_cpu_ready_count(size_t cpu_id) {
    if (cpu_id >= MAX_DETECTED_CPUS) return 0;
    uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[cpu_id].sched_lock);
    size_t count = 0;
    tcb_t *curr = scheduler_cpus[cpu_id].runqueue_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    spin_unlock_irqrestore(&scheduler_cpus[cpu_id].sched_lock, rflags);
    return count;
}

uint64_t sched_get_stolen_count(size_t cpu_id) {
    if (cpu_id >= MAX_DETECTED_CPUS) return 0;
    return __atomic_load_n(&scheduler_cpus[cpu_id].stolen_tasks_count, __ATOMIC_RELAXED);
}

void sched_init_aps(size_t total_cpus) {
    if (total_cpus > MAX_DETECTED_CPUS) total_cpus = MAX_DETECTED_CPUS;
    g_total_sched_cpus = total_cpus;

    for (size_t i = 1; i < total_cpus; i++) {
        scheduler_cpus[i].sched_lock = (spinlock_t)SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, g_sched_lock_names[i]);
        scheduler_cpus[i].preemption_enabled = false;
        scheduler_cpus[i].runqueue_head = NULL;
        scheduler_cpus[i].runqueue_tail = NULL;
        scheduler_cpus[i].blocked_threads = NULL;
        scheduler_cpus[i].dead_threads = NULL;
        scheduler_cpus[i].stolen_tasks_count = 0;

        /* Pre-allocate idle thread for AP on CPU 0 */
        tcb_t *idle = (tcb_t *)kmalloc(sizeof(tcb_t));
        if (!idle) {
            serial_puts("[FATAL] sched_init_aps: failed to kmalloc idle tcb\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        memset(idle, 0, sizeof(tcb_t));

        uintptr_t guard_virt = 0, stack_base = 0;
        size_t stack_size = 0;
        int slot = kstack_alloc(&guard_virt, &stack_base, &stack_size);
        if (slot < 0) {
            serial_puts("[FATAL] sched_init_aps: failed to allocate AP idle stack\n");
            for (;;) __asm__ volatile("cli; hlt");
        }

        idle->tid = __atomic_fetch_add(&g_global_next_tid, 1, __ATOMIC_RELAXED);
        memcpy(idle->name, "idle", 5);
        idle->state = THREAD_RUNNING;
        idle->stack_slot = slot;
        idle->kstack_guard = guard_virt;
        idle->kstack_base = stack_base;
        idle->kstack_size = stack_size;
        idle->ticks_remaining = DEFAULT_QUANTUM_TICKS;
        idle->is_idle = true;
        idle->cr3 = vmm_get_kernel_pml4();
        idle->pml4_virt = vmm_get_kernel_pml4_virt();
        idle->cpu_affinity = (int)i;
        idle->current_cpu = i;

        /* Setup stack frame for idle entry */
        uint8_t *stack_top = (uint8_t *)(stack_base + stack_size);
        stack_top = (uint8_t *)((uintptr_t)stack_top & ~0xFULL);
        stack_top -= sizeof(uint64_t) * 8;
        uint64_t *frame = (uint64_t *)stack_top;
        frame[0] = 0;                           /* r15 */
        frame[1] = 0;                           /* r14 */
        frame[2] = 0;                           /* r13 -> arg */
        frame[3] = (uint64_t)idle_thread_entry; /* r12 -> entry */
        frame[4] = 0;                           /* rbp */
        frame[5] = 0;                           /* rbx */
        frame[6] = 0x202;                       /* RFLAGS with IF=1 */
        frame[7] = (uint64_t)thread_trampoline; /* rip */
        idle->rsp = (uint64_t)stack_top;

        scheduler_cpus[i].idle_thread = idle;
    }
}

_Noreturn void sched_ap_start(size_t cpu_id) {
    if (cpu_id >= MAX_DETECTED_CPUS || !scheduler_cpus[cpu_id].idle_thread) {
        serial_puts("[FATAL] sched_ap_start: invalid AP CPU ID or uninitialized idle thread\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    tcb_t *idle = scheduler_cpus[cpu_id].idle_thread;
    cpu_locals[cpu_id].current_thread = idle;

    /* Set up AP TSS.RSP0 */
    gdt_set_tss_rsp0(idle->kstack_base + idle->kstack_size);

    /* Switch to kernel PML4 */
    vmm_switch_pml4(vmm_get_kernel_pml4());

    /* Start local LAPIC timer using calibrated count */
    lapic_timer_start_ap();

    /* Enable preemption on this AP */
    scheduler_cpus[cpu_id].preemption_enabled = true;

    /* Switch to the idle thread stack and start executing idle_thread_entry with interrupts enabled */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%rbp\n\t"
        "pop %%rbx\n\t"
        "popfq\n\t"
        "ret\n\t"
        :
        : "r"(idle->rsp)
        : "memory"
    );

    __builtin_unreachable();
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

    /* If currently in idle thread, yield to see if work arrived or can be stolen */
    if (g_current_thread->is_idle) {
        thread_yield();
        return;
    }

    /* Timeslice accounting for normal threads */
    if (--g_current_thread->ticks_remaining <= 0) {
        g_current_thread->ticks_remaining = DEFAULT_QUANTUM_TICKS;

        /* Preempt! */
        g_current_thread->preempt_count++;
        g_sched_timer_preemptions++;
        thread_yield();
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

static tcb_t *process_spawn_internal(size_t target_cpu, int affinity,
                                     const char *name, const void *elf_data, size_t elf_size,
                                     int argc, const char *const argv[],
                                     uint64_t scalar_arg, int64_t *error) {
    *error = SYSCALL_ENOMEM;
    if (!elf_data || elf_size == 0) return NULL;
    if (target_cpu >= MAX_DETECTED_CPUS) target_cpu = cpu_current()->id;

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
    p->current_cpu = target_cpu;
    p->cpu_affinity = affinity;

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

    int sref_err = vmm_space_add_sched_ref(proc_info.pml4_phys);
    if (sref_err != VMM_OK) {
        *error = SYSCALL_ENOMEM;
        kfree(p);
        kstack_free(slot, stack_base);
        vmm_destroy_pml4(proc_info.pml4_phys, true);
        return NULL;
    }

    rflags = spin_lock_irqsave(&scheduler_cpus[target_cpu].sched_lock);
    if (!scheduler_cpus[target_cpu].runqueue_head) {
        scheduler_cpus[target_cpu].runqueue_head = p;
        scheduler_cpus[target_cpu].runqueue_tail = p;
    } else {
        scheduler_cpus[target_cpu].runqueue_tail->next = p;
        scheduler_cpus[target_cpu].runqueue_tail = p;
    }
    spin_unlock_irqrestore(&scheduler_cpus[target_cpu].sched_lock, rflags);

    if (target_cpu != cpu_current()->id) {
        smp_send_resched(target_cpu);
    }

    return p;
}

tcb_t *process_spawn_with_arg(const char *name, const void *elf_data, size_t elf_size, uint64_t arg) {
    int64_t error;
    return process_spawn_internal(cpu_current()->id, (int)cpu_current()->id, name, elf_data, elf_size, 0, NULL, arg, &error);
}

tcb_t *process_spawn_on_cpu(size_t target_cpu, const char *name, const void *elf_data, size_t elf_size, uint64_t arg) {
    int64_t error;
    return process_spawn_internal(target_cpu, (int)target_cpu, name, elf_data, elf_size, 0, NULL, arg, &error);
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
    tcb_t *child = process_spawn_internal(cpu_current()->id, (int)cpu_current()->id, path, image, size, argc, argv, 0, &result);
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
    size_t limit = g_total_sched_cpus ? g_total_sched_cpus : 1;
    for (size_t c = 0; c < limit; c++) {
        uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[c].sched_lock);
        if (cpu_locals[c].current_thread &&
            cpu_locals[c].current_thread->tid == pid &&
            cpu_locals[c].current_thread->state != THREAD_TERMINATED) {
            spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
            return true;
        }
        for (tcb_t *t = scheduler_cpus[c].runqueue_head; t; t = t->next) {
            if (t->tid == pid && t->state != THREAD_TERMINATED) {
                spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
                return true;
            }
        }
        for (tcb_t *t = scheduler_cpus[c].blocked_threads; t; t = t->next) {
            if (t->tid == pid) {
                spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
                return true;
            }
        }
        spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
    }
    return false;
}

bool process_wait_extended(uint64_t pid, uint64_t *out_exit_code, uint64_t *out_preempt_count, uint64_t *out_total_ticks) {
    size_t limit = g_total_sched_cpus ? g_total_sched_cpus : 1;
    for (;;) {
        /* 1. Check if captured in exit records across all CPUs */
        for (size_t c = 0; c < limit; c++) {
            uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[c].sched_lock);
            for (int i = 0; i < MAX_EXIT_RECORDS; i++) {
                if (scheduler_cpus[c].exit_records[i].valid && scheduler_cpus[c].exit_records[i].pid == pid) {
                    if (out_exit_code) *out_exit_code = scheduler_cpus[c].exit_records[i].exit_code;
                    if (out_preempt_count) *out_preempt_count = scheduler_cpus[c].exit_records[i].preempt_count;
                    if (out_total_ticks) *out_total_ticks = scheduler_cpus[c].exit_records[i].total_ticks;
                    scheduler_cpus[c].exit_records[i].valid = false;
                    spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
                    return true;
                }
            }
            spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
        }

        /* 2. Check if process is still alive across all CPUs */
        bool alive = false;
        for (size_t c = 0; c < limit; c++) {
            uint64_t rflags = spin_lock_irqsave(&scheduler_cpus[c].sched_lock);
            if (cpu_locals[c].current_thread &&
                cpu_locals[c].current_thread->tid == pid &&
                cpu_locals[c].current_thread->state != THREAD_TERMINATED) {
                alive = true;
            } else if (scheduler_cpus[c].zombie_thread &&
                       scheduler_cpus[c].zombie_thread->tid == pid) {
                alive = true;
            } else {
                for (tcb_t *t = scheduler_cpus[c].runqueue_head; t; t = t->next) {
                    if (t->tid == pid && t->state != THREAD_TERMINATED) {
                        alive = true;
                        break;
                    }
                }
                if (!alive) {
                    for (tcb_t *t = scheduler_cpus[c].blocked_threads; t; t = t->next) {
                        if (t->tid == pid) {
                            alive = true;
                            break;
                        }
                    }
                }
                if (!alive) {
                    for (tcb_t *t = scheduler_cpus[c].dead_threads; t; t = t->next) {
                        if (t->tid == pid) {
                            alive = true;
                            break;
                        }
                    }
                }
            }
            spin_unlock_irqrestore(&scheduler_cpus[c].sched_lock, rflags);
            if (alive) break;
        }

        if (!alive) {
            /* Process not found in alive queues or exit records */
            return false;
        }

        /* Yield CPU to allow the process or reaper to make progress */
        sched_reap_dead();
        thread_yield();
    }
}

bool process_wait(uint64_t pid, uint64_t *out_exit_code) {
    return process_wait_extended(pid, out_exit_code, NULL, NULL);
}
