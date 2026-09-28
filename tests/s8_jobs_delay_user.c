/* Test-only delayed exit for the real-shell idle runner. No terminal reads:
 * SYS_INPUT_READ cannot serve as a background sleep (SIGTTIN/EIO).
 * QEMU's TSC provides a finite delay of 12 billion ticks, not a portable
 * wall-clock sleep. The runner bounds elapsed time and rejects early exits.
 * Timer preemption remains enabled while this BSP-pinned helper runs. */
#include "types.h"

static uint64_t ticks(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void shell_main(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint64_t start = ticks();
    while (ticks() - start < 12000000000ULL) __asm__ volatile("pause");
    /* shell_start exits with status zero. */
}
