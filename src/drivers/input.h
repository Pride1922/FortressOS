#ifndef FORTRESS_INPUT_H
#define FORTRESS_INPUT_H
#include "types.h"
#include "acpi.h"
bool input_init(const acpi_madt_info_t *madt);
/* Raw terminal-byte stdin: blocks until at least one byte, returns available short read.
 * Caller must validate destination; BSP-affine consumer, no concurrent process unmap. */
int64_t input_read(void *buffer, size_t count);
int64_t input_read_timeout(void *buffer, size_t count, int64_t timeout_ms);
void input_timer_tick(uint32_t hz);
uint64_t input_dropped(void);
#endif
