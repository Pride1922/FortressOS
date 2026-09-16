#ifndef FORTRESS_INPUT_H
#define FORTRESS_INPUT_H
#include "types.h"
#include "acpi.h"
bool input_init(const acpi_madt_info_t *madt);
/* Raw ASCII stdin: blocks until at least one byte, returns available short read.
 * Caller must validate destination; single CPU, no concurrent process unmap. */
int64_t input_read(void *buffer, size_t count);
#endif
