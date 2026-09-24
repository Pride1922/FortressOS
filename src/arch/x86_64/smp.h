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
 * No AP scheduler, allocation, ordinary IRQ, or subsystem-lock use yet.
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

#endif /* FORTRESS_SMP_H */
