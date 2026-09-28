#ifndef FORTRESS_TERMINAL_ABI_H
#define FORTRESS_TERMINAL_ABI_H
#include "types.h"
/* SYS_TERMCTL(op, info*, sizeof(info)). Per-process output endpoint. */
#define TERM_GET 0
#define TERM_SET 1
#define TERM_ISATTY 2
#define TERM_MIRROR 0
#define TERM_LOCAL 1
#define TERM_SERIAL 2
#define TERM_PLAIN 3
typedef struct {
    uint32_t version; /* must be 1 for SET */
    uint32_t mode;
    uint32_t cols; /* serial/plain configured width, 20..512; 0 selects 80 */
    uint32_t rows;
    uint64_t dropped;
    uint64_t generation; /* kernel output interrupted terminal display */
} terminal_info_t;
_Static_assert(sizeof(terminal_info_t) == 32, "terminal ABI size");
/* Shared input settings, separate from per-process output routing above.
 * SYS_TERMATTR(fd, TERM_GET/SET, attrs*, 32). GET writes; SET reads only.
 * SET requires version=1, supported flags, distinct nonzero 7-bit control
 * characters and zero reserved bytes. Diagnostic counters are read-only and
 * ignored on SET, so a GET snapshot can be modified and passed back to SET.
 * Control events flush queued ordinary input; output is never flushed. */
#define TERM_ISIG 1u
typedef struct {
    uint32_t version, input_flags;
    uint8_t vintr, vsusp, reserved[6];
    uint64_t signal_dropped, signal_coalesced;
} terminal_attrs_t;
_Static_assert(sizeof(terminal_attrs_t) == 32, "input terminal ABI size");
/* SYS_INPUT_READ(buf, cap, timeout_ms): raw non-echoed short read.
 * -1 timeout = indefinite; 0 = nonblocking; 1..1000 = bounded wait.
 * Returns 0 on timeout, -20 on lost input (queue flushed), or bytes.
 * BSP-only single interactive consumer. Legacy SYS_READ retains its ABI. */
#define INPUT_LOST (-20)
#endif
