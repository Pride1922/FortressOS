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

/*
 * NOTE ON STACK GUARD SEMANTICS & LIMITATIONS:
 * 1. Linear Growth Protection:
 *    The 4 KiB unmapped guard page at the base of each slot catches contiguous
 *    downward stack growth. Any push or call into this page causes a Page Fault (#PF).
 * 2. Exception Delivery & Double Fault (#DF) Escalation:
 *    Because Vector 14 (#PF) delivers on the current stack (IST=0), attempting
 *    to push the #PF exception frame onto an already-exhausted stack causes a
 *    nested page fault. The CPU automatically escalates this to a Double Fault
 *    (#DF, Vector 8). Because Vector 8 is wired to IST1, the kernel safely lands
 *    on the dedicated 16 KiB emergency IST1 stack and dumps diagnostic panic info,
 *    preventing an unrecoverable Triple Fault (CPU reset).
 * 3. Frame Skip Limitation:
 *    A single 4 KiB guard does NOT catch arbitrary out-of-bounds indexing or
 *    stack frame allocations exceeding 4096 bytes (e.g., large alloca or array)
 *    that jump over the guard into unmapped space or lower slots. Compilers use
 *    stack probes (-fstack-clash-protection) to guarantee touches in every 4 KiB page.
 */

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
    uint64_t       preempt_count;    /* Number of timer-driven preemptions experienced */
    bool           is_idle;          /* True if dedicated idle thread */

    /* Process Address Space & Privilege Extensions */
    uintptr_t      cr3;              /* Physical CR3 (0 for kernel threads) */
    uint64_t      *pml4_virt;        /* Virtual address of PML4 (NULL for kernel threads) */
    bool           is_user;          /* True if user-space process */
    uint64_t       exit_code;        /* Exit code captured upon termination */
    bool           has_exited;       /* True if process has exited */

    /* Per-Process File Descriptor Table (Step 8B) */
    struct file   *fd_table[32];

    struct tcb    *next;             /* Intrusive run queue link */
    const void *wait_channel; /* Only on blocked list while sleeping. */
    int            cpu_affinity;     /* Target CPU affinity: -1 for any, or 0..MAX-1 */
    size_t         current_cpu;      /* CPU ID where thread is currently queued/running */
} tcb_t;

struct file;
int          fd_alloc(tcb_t *proc, struct file *file);
struct file *fd_get(tcb_t *proc, int fd);
int          fd_free(tcb_t *proc, int fd);
void         fd_close_all(tcb_t *proc);

/* Public Scheduler & Thread API */
void   sched_init(void);
void   sched_init_aps(size_t total_cpus);
_Noreturn void sched_ap_start(size_t cpu_id);
tcb_t *thread_create(const char *name, void (*entry)(void *), void *arg);
tcb_t *thread_create_on_cpu(size_t target_cpu, const char *name, void (*entry)(void *), void *arg);
tcb_t *thread_create_unbound_on_cpu(size_t target_cpu, const char *name, void (*entry)(void *), void *arg);
void   thread_yield(void);
/* Bootstrap CPU only. Predicate runs under sched lock with IRQs disabled;
 * it must neither block nor acquire locks. Publish events before waking. */
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg);
void sched_wake_all(const void *channel);
void   thread_exit(void);
void   sched_reap_dead(void);
tcb_t *thread_current(void);
size_t sched_ready_count(void);
size_t sched_cpu_ready_count(size_t cpu_id);
uint64_t sched_get_stolen_count(size_t cpu_id);
void sched_lock_pair(spinlock_t *a, spinlock_t *b);
void sched_unlock_pair(spinlock_t *a, spinlock_t *b);

/* Process Lifecycle Management */
#define MAX_ELF_FILE_SIZE (4ULL * 1024 * 1024)
/* Kernel path, syscall error codes. Reserves one of 64 child records until
 * wait or parent exit; never overwrites an uncollected child status.
 * System V AMD64 ABI: RSP points to argc, RDI = argc, RSI = argv.
 * Bootstrap CPU only; IRQ-excluded publication, no inherited file descriptors. */
int process_setup_user_stack(uintptr_t stack_phys, int argc, const char *const argv[],
                             uintptr_t *out_user_rsp, uintptr_t *out_user_argv);
int64_t process_spawn_from_vfs(const char *path, int argc, const char *const argv[], int64_t *out_pid);
bool process_wait_child(uint64_t pid, uint64_t *out_exit_code);
tcb_t *process_spawn(const char *name, const void *elf_data, size_t elf_size);
tcb_t *process_spawn_with_arg(const char *name, const void *elf_data, size_t elf_size, uint64_t arg);
void   process_exit(uint64_t exit_code);
bool   process_wait(uint64_t pid, uint64_t *out_exit_code);
bool   process_wait_extended(uint64_t pid, uint64_t *out_exit_code, uint64_t *out_preempt_count, uint64_t *out_total_ticks);
bool   process_is_alive(uint64_t pid);

/* Preemption Control & Timer Hook */
void   sched_enable_preemption(void);
void   sched_disable_preemption(void);
bool   sched_is_preemption_enabled(void);
void   sched_on_timer_tick(void);
uint64_t sched_get_active_stack_slots_mask(void);
uint64_t sched_get_timer_preempt_count(void);
uint64_t sched_get_runnable_switches_count(void);

/* Low-level Context Switch Assembly Primitives */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);
extern void user_process_trampoline(void);

#endif /* FORTRESS_THREAD_H */
