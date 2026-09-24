#ifndef FORTRESS_SMP_H
#define FORTRESS_SMP_H

#include "types.h"
#include "acpi.h"

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

#endif /* FORTRESS_SMP_H */
