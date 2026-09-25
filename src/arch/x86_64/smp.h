#ifndef FORTRESS_SMP_H
#define FORTRESS_SMP_H

#include "types.h"
#include "acpi.h"
#include "spinlock.h"

/* SMP_DESIGN.md Pieces 1-2: cross-check MADT and Limine before release.
 * BSP calls once, with IF clear, after PMM/VMM/high-memory unlock and all
 * boot metadata parsing. Copies immutable MADT metadata, prepares all guards,
 * then starts APs serially. APs install private GS/GDT/TSS/stacks and the
 * shared kernel CR3/IDT, configure firmware NMI routes, test ISTs, and park.
 * Piece 6A checks boot readiness before release and again on each AP after
 * installing kernel CR3. AP scheduling starts separately (Piece 4).
 * Timeout stops further releases; static resources are retained even if a
 * CPU reports late. Returns confirmed AP count (BSP excluded); topology
 * rejection returns zero. Repeated calls return the first result.
 */
size_t smp_init(const acpi_madt_info_t *madt_info);

/* Total CPU count (BSP + online APs) as of the last smp_init() call.
 * 1 if smp_init() was never called, found no usable Limine SMP response,
 * or every AP failed to report in. */
size_t smp_get_cpu_count(void);

uint32_t smp_get_bsp_lapic_id(void);

/* SMP Piece 3: Lock Discipline Verification Modes */
#define SMP_TEST_MODE_NONE        0
#define SMP_TEST_MODE_CONTENTION  1
#define SMP_TEST_MODE_INVERSION   2
#define SMP_TEST_MODE_ASSERT_HELD 3

typedef struct {
    volatile uint32_t test_mode;
    volatile uint32_t ap_ready;
    volatile uint32_t bsp_start;
    volatile uint32_t ap_done;
    uint64_t counter; /* Accessed exclusively under test_lock */
    spinlock_t lock;
} smp_lock_test_mailbox_t;

extern smp_lock_test_mailbox_t g_smp_lock_test;

void smp_set_test_mode(uint32_t mode);
bool smp_run_lock_tests(void);

/* SMP Piece 4: Scheduler Bringup */
void smp_start_schedulers(void);

/* SMP Piece 5: Cross-Core Coordination & IPIs */
void smp_ipi_init(void);
void smp_tlb_shootdown(uintptr_t virt_addr, uintptr_t cr3);
void smp_send_resched(size_t cpu_id);
void smp_send_panic(void);

/* SMP Piece 6C: Contention-Safe TLB Shootdown (SM14, SM15)
 * Called from three contexts:
 *   1. IPI handler (smp_ipi_tlb_handler, IF=0, in ISR).
 *   2. Spinlock wait loop (spin_lock_irqsave/spin_lock_noirq, IF=0, thread context).
 *   3. Initiator wait loop (smp_tlb_shootdown, IF=0, thread context).
 * Invariant: No locks, no sleep, no schedule, no enable IF, no heap allocation.
 */
void smp_tlb_service_local(void);

extern volatile uint64_t g_ipi_tlb_count[MAX_DETECTED_CPUS];
extern volatile uint64_t g_ipi_resched_count[MAX_DETECTED_CPUS];
extern volatile uint64_t g_tlb_poll_serviced_count[MAX_DETECTED_CPUS];

#endif /* FORTRESS_SMP_H */

