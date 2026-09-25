// SPDX-License-Identifier: MIT
// SMP memory stress test for PMM (host build).
// Compiled with -DTEST_SMP_MEMORY which activates the pthread mutex shim.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <time.h>
#include <string.h>

#include "pmm.h"
#include "spinlock.h"
/* Minimal VMM stubs required by PMM. */
bool vmm_boot_memory_ready(void) { return true; }
void *vmm_get_kernel_pml4(void) { return (void *)0x1; }
void *vmm_get_current_pml4(void) { return (void *)0x1; }

/* Maximum frames the PMM ever manages (32 GiB / 4 KiB). */
#ifndef PMM_MAX_FRAMES
#define PMM_MAX_FRAMES (8388608U)
#endif

/* The ceiling under test. Must match pmm.c's early-boot limit. */
#define PMM_TEST_CEILING (0x40000000ULL)   /* 1 GiB */

/* -------------------------------------------------------------------------
 * Host-test hooks provided by pmm.c (under #ifdef TEST_SMP_MEMORY):
 *
 *   void  pmm_test_init(uint64_t total_pages);
 *   void *pmm_host_phys_to_virt(uintptr_t phys);
 *
 * pmm_test_init() sets the internal PMM state directly (bitmap cleared,
 * counters at zero, ceiling at PMM_TEST_CEILING) without going through
 * pmm_init()'s memmap parsing.
 *
 * pmm_host_phys_to_virt() maps a "physical" address the PMM hands out
 * onto a real host buffer so the test can read/write it.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Shared test state
 * ------------------------------------------------------------------------- */

static uint8_t dup_bitmap[PMM_MAX_FRAMES / 8];  /* 1 MiB static */

static inline void dup_clear(size_t idx) {
    uint8_t mask = (uint8_t)(1u << (idx % 8));
    __atomic_fetch_and(&dup_bitmap[idx / 8], (uint8_t)~mask, __ATOMIC_SEQ_CST);
}
static inline bool dup_test_and_set(size_t idx) {
    uint8_t mask = (uint8_t)(1u << (idx % 8));
    return (__atomic_fetch_or(&dup_bitmap[idx / 8], mask, __ATOMIC_SEQ_CST) & mask) != 0;
}

/* Set by test_transition to gate the ceiling assertion. */
static bool g_high_memory_enabled = false;

/* -------------------------------------------------------------------------
 * Worker: allocate / verify / free, one frame at a time.
 * ------------------------------------------------------------------------- */

static void *worker(void *arg) {
    (void)arg;

    for (int i = 0; i < 10000; ++i) {
        uintptr_t p = pmm_alloc_page();
        if (!p) {
            fprintf(stderr, "worker: allocation failed at iteration %d\n", i);
            exit(1);
        }

        /* Ceiling enforcement: before unlock, no frame may sit above the cap. */
        if (!__atomic_load_n(&g_high_memory_enabled, __ATOMIC_ACQUIRE)) {
            if (p >= PMM_TEST_CEILING) {
                fprintf(stderr, "worker: got frame above ceiling before unlock: 0x%llx\n",
                        (unsigned long long)p);
                exit(1);
            }
        }

        size_t frame = p / PAGE_SIZE;
        if (frame >= PMM_MAX_FRAMES) {
            fprintf(stderr, "worker: frame index out of range: %zu\n", frame);
            exit(1);
        }

        /* Duplicate detection: atomically test and set the bit.
         * If the bit was already set, another worker is holding the same frame. */
        if (dup_test_and_set(frame)) {
            fprintf(stderr, "worker: duplicate allocation of frame %zu (phys 0x%llx)\n",
                    frame, (unsigned long long)p);
            exit(1);
        }

        /* Dereference the frame through the host shim. */
        void *v = pmm_host_phys_to_virt(p);
        if (!v) {
            fprintf(stderr, "worker: pmm_host_phys_to_virt(0x%llx) returned NULL\n",
                    (unsigned long long)p);
            exit(1);
        }
        memset(v, 0xAA, PAGE_SIZE);

        dup_clear(frame);
        pmm_free_page(p);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Test A: ceiling before and after unlock.
 * ------------------------------------------------------------------------- */

static void test_ceiling(void) {
    /* Warm up so a low frame definitely exists. */
    uintptr_t warm = pmm_alloc_page_above(0x100000);
    if (warm) pmm_free_page(warm);

    /* Under the ceiling: should succeed (1 GiB machine mock has free RAM
     * both below and, before unlock, only below). */
    uintptr_t a = pmm_alloc_page_above(0x20000000ULL);   /* 512 MiB */
    assert(a != 0);
    assert(a < PMM_TEST_CEILING);
    pmm_free_page(a);

    /* Above the ceiling: must fail. */
    uintptr_t b = pmm_alloc_page_above(PMM_TEST_CEILING);
    assert(b == 0);

    /* Unlock. */
    bool ok = pmm_unlock_high_memory();
    assert(ok);

    /* Both should succeed now. */
    uintptr_t c = pmm_alloc_page_above(0x20000000ULL);
    assert(c != 0);
    pmm_free_page(c);

    uintptr_t d = pmm_alloc_page_above(PMM_TEST_CEILING);
    assert(d != 0);
    assert(d >= PMM_TEST_CEILING);
    pmm_free_page(d);
}

/* -------------------------------------------------------------------------
 * Test I: transition atomicity.
 *
 * Workers run and allocate continuously. The main thread unlocks at some
 * point mid-flight. Workers assert "below ceiling while not unlocked".
 * ------------------------------------------------------------------------- */

static void test_transition(void) {
    enum { NUM_WORKERS = 8 };
    pthread_t th[NUM_WORKERS];

    g_high_memory_enabled = false;

    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (pthread_create(&th[i], NULL, worker, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }

    /* Let them spin on the low-ceiling side for a bit. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 };  /* 200 ms */
    nanosleep(&ts, NULL);

    /* Flip the flag before unlocking so workers stop asserting the ceiling
     * the moment the unlock takes effect. */
    __atomic_store_n(&g_high_memory_enabled, true, __ATOMIC_RELEASE);
    assert(pmm_unlock_high_memory());

    for (int i = 0; i < NUM_WORKERS; ++i) {
        pthread_join(th[i], NULL);
    }
}

/* -------------------------------------------------------------------------
 * Test J: latency under fragmentation.
 *
 * Allocate every other frame in a range, hold the odd ones, free the even
 * ones. Now the bitmap is fragmented; measure allocation latency.
 * ------------------------------------------------------------------------- */

static void test_latency(void) {
    enum { HOLD = 1024 };
    static uintptr_t held[HOLD];

    /* Hold a contiguous block of frames. */
    size_t got = 0;
    for (size_t i = 0; i < HOLD; ++i) {
        uintptr_t p = pmm_alloc_page();
        if (!p) break;
        held[got++] = p;
    }
    if (got < HOLD) {
        fprintf(stderr, "test_latency: only allocated %zu of %d frames\n", got, HOLD);
        /* Not fatal; just fewer frames to work with. */
    }

    /* Free the even-indexed ones, keeping the odd ones allocated. */
    for (size_t i = 0; i < got; i += 2) {
        pmm_free_page(held[i]);
        held[i] = 0;
    }

    /* Measure. The bitmap now has holes every other frame. */
    const int iterations = 10000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; ++i) {
        uintptr_t p = pmm_alloc_page();
        assert(p != 0);
        pmm_free_page(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long long nsec = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL
                   + (long long)(end.tv_nsec - start.tv_nsec);
    long long avg_ns = nsec / iterations;
    printf("Latency test: %d allocations in %lld ns (avg %lld ns)\n",
           iterations, nsec, avg_ns);

    /* Clean up the held frames. */
    for (size_t i = 1; i < got; i += 2) {
        if (held[i]) pmm_free_page(held[i]);
    }
}

/* -------------------------------------------------------------------------
 * Test: Spinlock telemetry race hardening.
 * 8 threads concurrently acquire/release a test lock 10,000 times each.
 * Asserts acquire_count is exactly 8 * 10,000 = 80,000.
 * ------------------------------------------------------------------------- */
static spinlock_t g_test_telemetry_lock = SPINLOCK_RANKED(1, "test_telemetry");

static void *telemetry_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 10000; ++i) {
        uint64_t flags = spin_lock_irqsave(&g_test_telemetry_lock);
        spin_unlock_irqrestore(&g_test_telemetry_lock, flags);
    }
    return NULL;
}

static void test_telemetry(void) {
    enum { NUM_WORKERS = 8 };
    pthread_t th[NUM_WORKERS];
    g_test_telemetry_lock.acquire_count = 0;
    g_test_telemetry_lock.contention_count = 0;

    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (pthread_create(&th[i], NULL, telemetry_worker, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < NUM_WORKERS; ++i) {
        pthread_join(th[i], NULL);
    }

    assert(g_test_telemetry_lock.acquire_count == (uint64_t)(NUM_WORKERS * 10000));
}

static void *multipage_worker(void *arg) {
    (void)arg;
    const size_t runs[] = {1, 2, 4, 8};
    for (int i = 0; i < 1000; ++i) {
        size_t count = runs[i % 4];
        uintptr_t p = pmm_alloc_pages(count);
        if (!p) {
            fprintf(stderr, "multipage_worker: alloc failed for %zu pages\n", count);
            exit(1);
        }
        assert(p % PAGE_SIZE == 0);
        size_t base_frame = p / PAGE_SIZE;
        for (size_t f = 0; f < count; ++f) {
            size_t frame = base_frame + f;
            assert(frame < PMM_MAX_FRAMES);
            if (dup_test_and_set(frame)) {
                fprintf(stderr, "multipage_worker: duplicate allocation of frame %zu\n", frame);
                exit(1);
            }
            void *v = pmm_host_phys_to_virt(p + f * PAGE_SIZE);
            assert(v != NULL);
            memset(v, 0x5A, PAGE_SIZE);
        }
        for (size_t f = 0; f < count; ++f) {
            dup_clear(base_frame + f);
        }
        pmm_free_pages(p, count);
    }
    return NULL;
}

static void test_multipage(void) {
    enum { NUM_WORKERS = 4 };
    pthread_t th[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (pthread_create(&th[i], NULL, multipage_worker, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < NUM_WORKERS; ++i) {
        pthread_join(th[i], NULL);
    }
}

static void test_oom(void) {
    static uint8_t oom_bitmap[64];
    pmm_test_init(64, oom_bitmap);
    assert(pmm_unlock_high_memory());

    uintptr_t pages[63];
    for (size_t i = 0; i < 63; ++i) {
        pages[i] = pmm_alloc_page();
        assert(pages[i] != 0);
        assert(pages[i] % PAGE_SIZE == 0);
    }

    assert(pmm_get_free_pages() == 0);
    assert(pmm_get_allocatable_pages() == 0);
    assert(pmm_alloc_page() == 0);
    assert(pmm_alloc_pages(2) == 0);

    for (size_t i = 0; i < 63; ++i) {
        pmm_free_page(pages[i]);
    }
    assert(pmm_get_free_pages() == 63);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
    printf("=== PMM SMP host test ===\n");

    pmm_host_shim_init();

    /* Static bitmap: 1 MiB covers 8M frames, but we only need
     * (num_pages + 7) / 8 = 128 KiB for 1 GiB / 4 KiB = 262144 pages. */
    static uint8_t test_bitmap[256 * 1024];
    size_t num_pages = (2ULL * 1024 * 1024 * 1024) / PAGE_SIZE;   /* 262144 */
    pmm_test_init(num_pages, test_bitmap);

    memset(dup_bitmap, 0, sizeof(dup_bitmap));
    g_high_memory_enabled = false;

    test_ceiling();
    printf("PASS: test_ceiling\n");

    test_transition();
    printf("PASS: test_transition\n");

    test_latency();
    printf("PASS: test_latency\n");

    test_telemetry();
    printf("PASS: test_telemetry\n");

    test_multipage();
    printf("PASS: test_multipage\n");

    test_oom();
    printf("PASS: test_oom\n");

    pmm_host_shim_free();
    printf("=== All PMM SMP tests passed ===\n");
    return 0;
}