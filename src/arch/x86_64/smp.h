#ifndef FORTRESS_SMP_H
#define FORTRESS_SMP_H

#include "types.h"
#include "acpi.h"

/* SMP_DESIGN.md, Piece 1 (AP discovery) only: find every CPU ACPI's MADT
 * says is enabled, start it via Limine's SMP boot protocol, cross-check
 * the two sources against each other (SM1), and wait for every AP to
 * report in (SM2). An AP that comes up here does nothing but report in
 * and park itself, halted with interrupts disabled -- it does not touch
 * scheduler, heap, or any other shared kernel state, because none of
 * that is CPU-safe yet (that starts at Piece 2, per-CPU storage).
 *
 * Returns the number of APs (not counting the BSP) that came online.
 * A single-CPU machine, or a MADT/Limine mismatch, returns 0 and the
 * kernel proceeds single-CPU exactly as before this piece existed.
 */
size_t smp_init(const acpi_madt_info_t *madt_info);

/* Total CPU count (BSP + online APs) as of the last smp_init() call.
 * 1 if smp_init() was never called, found no usable Limine SMP response,
 * or every AP failed to report in. */
size_t smp_get_cpu_count(void);

uint32_t smp_get_bsp_lapic_id(void);

#endif /* FORTRESS_SMP_H */
