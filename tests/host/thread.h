#ifndef HOST_THREAD_H
#define HOST_THREAD_H

#include "types.h"
#include "spinlock.h"

typedef struct tcb {
    uint64_t  tid;
    uintptr_t cr3;
    size_t current_cpu;
    struct vmm_space *vmm_space;
    uint32_t terminal_mode;
    int stack_slot;
} tcb_t;
#ifdef TEST_VMM_HOST
extern _Thread_local tcb_t *g_vmm_host_current;
static inline tcb_t *thread_current(void) { return g_vmm_host_current; }
#else
static inline tcb_t *thread_current(void) { return NULL; }
#endif

static inline tcb_t *thread_create_on_cpu(size_t c, const char *n, void (*entry)(void *), void *a) {
    (void)c; (void)n; (void)entry; (void)a;
    return NULL;
}
static inline void thread_yield(void) {}
static inline void thread_exit(void) {}
static inline void sched_reap_dead(void) {}
static inline void sched_disable_preemption(void) {}
static inline void sched_enable_preemption(void) {}
static inline uint64_t sched_get_active_stack_slots_mask(void) { return 0; }

#define KERNEL_STACKS_BASE 0xFFFFFFFFA0000000ULL
#define STACK_GUARD_SIZE   4096ULL
#define STACK_USABLE_SIZE  16384ULL
#define STACK_SLOT_SIZE    (STACK_GUARD_SIZE + STACK_USABLE_SIZE)

typedef struct {
    uint64_t slot_wait_tsc;
    uint64_t slot_hold_tsc;
    uint64_t alloc_prep_tsc;
    uint64_t pmm_tsc;
    uint64_t map_prep_tsc;
    uint64_t vmm_lock_wait_tsc;
    uint64_t vmm_lock_hold_tsc;
    uint64_t vmm_pt_work_tsc;
    uint64_t vmm_pre_lock_tsc;
    uint64_t vmm_post_lock_prep_tsc;
    uint64_t vmm_put_op_wait_tsc;
    uint64_t vmm_put_op_hold_tsc;
    uint64_t vmm_tlb_dispatch_tsc;
    uint64_t vmm_tlb_ack_poll_tsc;
    uint64_t vmm_tlb_service_tsc;
    uint64_t alloc_tail_tsc;
    uint64_t free_mid_tsc;
    uint64_t free_tail_tsc;
    uint64_t inner_tsc;
} kstack_subinterval_t;

static inline int kstack_alloc_tracked(uintptr_t *out_guard, uintptr_t *out_base, size_t *out_size, kstack_subinterval_t *metrics) {
    (void)out_guard; (void)out_base; (void)out_size; (void)metrics; return -1;
}
static inline void kstack_free_tracked(int slot, uintptr_t base_addr, kstack_subinterval_t *metrics) {
    (void)slot; (void)base_addr; (void)metrics;
}

/* Declarations for shared memory-test source; boot-only fixtures discard
 * the unused lifecycle sections rather than emulating process execution. */
typedef struct spawn_kaction {
    uint32_t type;
    int32_t  dst_fd;
    int32_t  src_fd;
    uint32_t flags;
    uint32_t mode;
    const char *path;
} spawn_kaction_t;

tcb_t *process_spawn_on_cpu(size_t cpu, const char *name, const void *elf,
                            size_t size, uint64_t arg);
tcb_t *process_spawn_with_actions(size_t cpu, const char *name, const void *elf,
                                  size_t size, int action_count, const spawn_kaction_t *actions);
bool process_wait(uint64_t pid, uint64_t *out_exit_code);
bool process_is_alive(uint64_t pid);

typedef enum {
    SPAWN_FAULT_NONE = 0,
    SPAWN_FAULT_VMM_USER_PML4,
    SPAWN_FAULT_ELF_SEGMENT_PMM,
    SPAWN_FAULT_ELF_SEGMENT_MAP,
    SPAWN_FAULT_SIGRESTORER_PMM,
    SPAWN_FAULT_SIGRESTORER_MAP,
    SPAWN_FAULT_USER_STACK_PMM,
    SPAWN_FAULT_USER_STACK_MAP,
    SPAWN_FAULT_KSTACK_PMM,
    SPAWN_FAULT_KSTACK_MAP,
    SPAWN_FAULT_KSTACK_ALLOC,
    SPAWN_FAULT_TCB_KMALLOC,
    SPAWN_FAULT_FD_INIT,
    SPAWN_FAULT_SCHED_REF,
} spawn_fault_type_t;

static inline void spawn_set_fault_injection(spawn_fault_type_t t, size_t c) { (void)t; (void)c; }
static inline void spawn_clear_fault_injection(void) {}
static inline spawn_fault_type_t spawn_get_fault_type(void) { return SPAWN_FAULT_NONE; }
static inline size_t spawn_get_fault_trigger(void) { return 0; }
static inline size_t spawn_get_fault_hits(void) { return 0; }
static inline void spawn_record_fault_hit(void) {}
static inline uint64_t spawn_get_last_aborted_pid(void) { return 0; }

#endif
