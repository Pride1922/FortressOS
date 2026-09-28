#ifndef FORTRESS_INPUT_H
#define FORTRESS_INPUT_H
#include "types.h"
#include "acpi.h"
#include "terminal.h"
/* Thread context, no locks held. Terminal state is BSP IRQ-excluded; all these
 * APIs reject other CPUs. Kernel bootstrap runs before the shell is scheduled. */
int input_terminal_bootstrap(uint64_t pid);
int64_t input_tcgetpgrp(uint64_t fd);
int64_t input_tcsetpgrp(uint64_t fd, uint64_t pgid);
int input_termattr(uint64_t fd, uint64_t op, terminal_attrs_t *attrs);
int input_control_check(void);
bool input_init(const acpi_madt_info_t *madt);
/* Raw terminal-byte stdin: blocks until at least one byte, returns available short read.
 * Caller must validate destination; BSP-affine consumer, no concurrent process unmap.
 * Errors use SYSCALL_* values, including EINTR before consuming any bytes. */
int64_t input_read(void *buffer, size_t count);
int64_t input_read_timeout(void *buffer, size_t count, int64_t timeout_ms);
void input_timer_tick(uint32_t hz);
uint64_t input_dropped(void);
#endif
