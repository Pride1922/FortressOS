#ifndef FORTRESS_PERCPU_H
#define FORTRESS_PERCPU_H
#include "types.h"
#include "acpi.h"
struct tcb;
/* First three offsets are the assembly syscall ABI. Kernel GS always points
 * here; user GS starts at zero. FSGSBASE is not enabled. */
typedef struct cpu_local {
    struct cpu_local *self;
    uint64_t syscall_rsp;
    uint64_t rsp0;
    struct tcb *current_thread;
    uint32_t id, lapic_id;
    uint64_t preempt_count; /* Timer preemptions on this CPU. */
    uint64_t irq_depth;     /* Active interrupt nesting, suspended across switch. */
    volatile uint32_t online;
    volatile uint32_t probe;
    uintptr_t probe_rsp;
    uintptr_t recovery_rsp, recovery_rip;
    uint64_t nmi_count;
    uintptr_t nmi_rsp;
    bool nmi_uart_available;
    uint64_t fault_vector, fault_error;
    uintptr_t fault_rip;
    struct spinlock *held_locks[16];
    uint32_t lock_depth;
    uint32_t lock_panic;
} cpu_local_t;
#define MAX_HELD_LOCKS 16
_Static_assert(__builtin_offsetof(cpu_local_t, syscall_rsp) == 8, "GS scratch ABI");
_Static_assert(__builtin_offsetof(cpu_local_t, irq_depth) == 48, "GS IRQ depth ABI");
_Static_assert(__builtin_offsetof(cpu_local_t, rsp0) == 16, "GS RSP0 ABI");
extern cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
extern volatile bool g_cpu_installed[MAX_DETECTED_CPUS];
static inline cpu_local_t *cpu_current(void) {
    if (!g_cpu_installed[0]) {
        return &cpu_locals[0];
    }
    cpu_local_t *cpu;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cpu) : : "memory");
    return cpu;
}
void cpu_install(size_t id);
#endif
