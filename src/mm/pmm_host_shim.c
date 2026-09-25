#ifdef TEST_SMP_MEMORY
#include "spinlock.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* Host‑only shim for the PMM spinlock when building the test harness with
 * ThreadSanitizer (TSan). The real kernel spinlock uses a custom busy‑wait
 * implementation that TSan cannot recognise, so we map the lock to a
 * pthread_mutex. This is compiled only when TEST_SMP_MEMORY is defined.
 */


/* Host stubs for kernel serial diagnostics. */
void serial_puts(const char *s) { (void)s; }
void serial_putc(char c) { (void)c; }
void serial_print_hex(uint64_t v) { (void)v; }
void serial_print_dec(uint64_t v) { (void)v; }


/* Fake physical RAM buffer, sized to match PMM_TEST_TOTAL_BYTES. */
#define PMM_TEST_TOTAL_BYTES (2ULL * 1024 * 1024 * 1024)   /* 2 GiB */
static uint8_t *g_fake_ram = NULL;
static size_t   g_fake_ram_size = 0;

void pmm_host_shim_init(void) {
    g_fake_ram_size = (size_t)PMM_TEST_TOTAL_BYTES;
    g_fake_ram = calloc(1, g_fake_ram_size);
    if (!g_fake_ram) {
        fprintf(stderr, "pmm_host_shim_init: calloc failed\n");
        exit(1);
    }
}

void *pmm_host_phys_to_virt(uintptr_t phys) {
    if (!g_fake_ram || phys >= g_fake_ram_size) return NULL;
    return g_fake_ram + phys;
}

void pmm_host_shim_free(void) {
    free(g_fake_ram);
    g_fake_ram = NULL;
    g_fake_ram_size = 0;
}

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    pthread_mutex_lock(&lock->mutex);
    lock->acquire_count++;
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    pthread_mutex_unlock(&lock->mutex);
}

#endif /* TEST_SMP_MEMORY */