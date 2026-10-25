#ifndef FORTRESS_CRASH_DUMP_H
#define FORTRESS_CRASH_DUMP_H

#include "types.h"
#include "idt.h"

#define CRASH_DUMP_CODE_BYTES    16
#define CRASH_DUMP_STACK_QWORDS  16

/* Decodes CPU exception vector to canonical POSIX signal number (SIGSEGV, SIGBUS, SIGILL, SIGFPE) */
unsigned crash_dump_vector_to_signal(uint64_t vector);

/* Returns a human-readable mnemonic for the exception vector */
const char *crash_dump_vector_name(uint64_t vector);

/* Returns canonical POSIX signal name string */
const char *crash_dump_signal_name(unsigned sig);

/* Decodes page fault error code bits into human-readable text */
void crash_dump_format_pf_error(uint64_t error_code, char *buf, size_t buf_len);

/* Formats decoded condition flags from RFLAGS into text */
void crash_dump_format_rflags(uint64_t rflags, char *buf, size_t buf_len);

/* Performs bounded user-space crash dump and terminates faulting process */
void crash_dump_user(interrupt_frame_t *frame);

#endif /* FORTRESS_CRASH_DUMP_H */
