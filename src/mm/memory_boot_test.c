#include "memory_boot_test.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "thread.h"

/* Pre-heap tests need static snapshot storage. It remains allocated, including
 * in the baseline. Only the explicit test mode performs these diagnostics. */
static uint8_t before[PMM_BITMAP_CAPACITY_BYTES];
static uint8_t after[PMM_BITMAP_CAPACITY_BYTES];

static bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool memory_boot_test_enabled(const boot_info_t *boot_info) {
    static const char token[] = "smp_memory_test=boot";
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    size_t capacity = sizeof(boot_info->cmdline);
    for (size_t i = 0; i < capacity && cmd[i];) {
        if (space(cmd[i])) { i++; continue; }
        size_t start = i;
        while (i < capacity && cmd[i] && !space(cmd[i])) i++;
        /* A token cut off at the buffer boundary is not an opt-in. */
        if (i < capacity && i - start == sizeof(token) - 1 &&
            memcmp(cmd + start, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

static void require(bool condition, const char *reason) {
    if (condition) return;
    serial_puts("[FAIL] SMP memory 6A: ");
    serial_puts(reason);
    serial_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static void require_low(uintptr_t p, size_t count) {
    require(p != 0 && p % PAGE_SIZE == 0 && p < PMM_BOOT_ALLOC_LIMIT &&
            count <= (PMM_BOOT_ALLOC_LIMIT - p) / PAGE_SIZE,
            "allocation escaped boot ceiling or failed");
}

static void snapshot_begin(void) {
    require(pmm_snapshot(before, sizeof(before)), "baseline snapshot");
}

static void snapshot_end(void) {
    require(pmm_snapshot(after, sizeof(after)) &&
            memcmp(before, after, sizeof(before)) == 0,
            "allocation set changed after probe cleanup");
}

void memory_boot_test_before_vmm(void) {
    require(!vmm_boot_memory_ready() && !pmm_high_memory_enabled(),
            "premature memory readiness");
    require(!pmm_unlock_high_memory(), "premature unlock accepted");
    snapshot_begin();
    size_t eligible = pmm_get_allocatable_pages();
    require(eligible <= pmm_get_free_pages(), "eligible/free accounting");
    uintptr_t pages[64];
    for (size_t i = 0; i < 64; i++) {
        pages[i] = pmm_alloc_page();
        require_low(pages[i], 1);
        for (size_t j = 0; j < i; j++)
            require(pages[i] != pages[j], "duplicate live page");
    }
    require(pmm_get_allocatable_pages() + 64 == eligible, "low-page accounting");
    for (size_t i = 0; i < 64; i++) pmm_free_page(pages[i]);
    const size_t runs[] = {1, 2, 7, 16};
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        uintptr_t p = pmm_alloc_pages(runs[i]);
        require_low(p, runs[i]);
        pmm_free_pages(p, runs[i]);
    }
    uintptr_t p = pmm_alloc_page_above(0x100001);
    require_low(p, 1);
    require(p >= 0x101000, "minimum address rounded down");
    pmm_free_page(p);
    require(pmm_alloc_page_above(PMM_BOOT_ALLOC_LIMIT) == 0 &&
            pmm_alloc_page_above(PMM_BOOT_ALLOC_LIMIT + 1) == 0 &&
            pmm_alloc_page_above(UINT64_MAX) == 0 &&
            pmm_alloc_pages(PMM_BOOT_ALLOC_LIMIT / PAGE_SIZE + 1) == 0,
            "high-only/oversized allocation accepted before unlock");
    snapshot_end();
    require(pmm_get_allocatable_pages() == eligible, "low-page cleanup");
    serial_puts("[PASS] SMP memory 6A: boot ceiling, early unlock rejection, exact cleanup\n");
}

void memory_boot_test_after_vmm(void) {
    require(vmm_boot_memory_ready() && pmm_high_memory_enabled() &&
            vmm_get_current_pml4() == vmm_get_kernel_pml4(), "unlock/CR3 ordering");
    require(pmm_get_allocatable_pages() == pmm_get_free_pages(), "full allocation eligibility");
    snapshot_begin();
    const uintptr_t thresholds[] = {
        PMM_BOOT_ALLOC_LIMIT, 0x80000000ULL, 0x100000000ULL,
        0x400000000ULL, 0x780000000ULL
    };
    for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
        uintptr_t min = thresholds[i];
        if (min >= pmm_get_total_memory()) {
            serial_puts("[SKIP] SMP memory 6A: threshold outside managed RAM ");
            serial_print_hex(min);
            serial_puts("\n");
            continue;
        }
        uintptr_t p = pmm_alloc_page_above(min);
        require(p != 0 && p >= min && p % PAGE_SIZE == 0, "high-page allocation");
        volatile uint64_t *v = vmm_phys_to_virt(p);
        require(vmm_get_physical_address(vmm_get_kernel_pml4_virt(), (uintptr_t)v) == p,
                "high-page HHDM translation");
        for (unsigned pass = 0; pass < 2; pass++) {
            for (size_t word = 0; word < PAGE_SIZE / sizeof(*v); word++) {
                uint64_t pattern = (p + word * sizeof(*v)) ^ 0xC35A96E187B40D2FULL;
                v[word] = pass ? ~pattern : pattern;
            }
            for (size_t word = 0; word < PAGE_SIZE / sizeof(*v); word++) {
                uint64_t pattern = (p + word * sizeof(*v)) ^ 0xC35A96E187B40D2FULL;
                require(v[word] == (pass ? ~pattern : pattern), "full-page high-memory readback");
            }
        }
        serial_puts("[PASS] SMP memory 6A: HHDM full-page readback min=");
        serial_print_hex(min);
        serial_puts(" phys=");
        serial_print_hex(p);
        serial_puts("\n");
        pmm_free_page(p);
    }
    snapshot_end();
    serial_puts("[PASS] SMP memory 6A: kernel CR3, high-memory unlock, exact cleanup\n");
}

/* =========================================================================
 * SMP Piece 6B: Freestanding Multi-Core Memory Stress Test
 * ========================================================================= */

bool memory_stress_test_enabled(const boot_info_t *boot_info) {
    static const char token[] = "smp_memory_test=stress";
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    size_t capacity = sizeof(boot_info->cmdline);
    for (size_t i = 0; i < capacity && cmd[i];) {
        if (space(cmd[i])) { i++; continue; }
        size_t start = i;
        while (i < capacity && cmd[i] && !space(cmd[i])) i++;
        if (i < capacity && i - start == sizeof(token) - 1 &&
            memcmp(cmd + start, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

#define STRESS_WORKER_COUNT 32
#define STRESS_ITERATIONS   10000
#define PMM_MAX_FRAMES (PMM_BITMAP_CAPACITY_BYTES * 8ULL)

static uint8_t g_frame_owner_table[PMM_MAX_FRAMES / 8];
_Static_assert(sizeof(g_frame_owner_table) == PMM_MAX_FRAMES / 8,
               "g_frame_owner_table size mismatch");

static inline bool claim_frame(size_t frame_idx) {
    uint8_t mask = (uint8_t)(1u << (frame_idx % 8));
    return (__atomic_fetch_or(&g_frame_owner_table[frame_idx / 8], mask, __ATOMIC_SEQ_CST) & mask) == 0;
}

static inline void release_frame(size_t frame_idx) {
    uint8_t mask = (uint8_t)(1u << (frame_idx % 8));
    __atomic_fetch_and(&g_frame_owner_table[frame_idx / 8], (uint8_t)~mask, __ATOMIC_SEQ_CST);
}

static volatile uint32_t g_stress_start = 0;
static volatile uint32_t g_stress_done = 0;
static volatile uint32_t g_stress_workers_active = 0;
static volatile uint32_t g_stress_workers_terminated = 0;
static volatile uint32_t g_stress_duplicate_errors = 0;
static volatile uint32_t g_stress_alloc_failures = 0;
static volatile uint32_t g_stress_verify_errors = 0;

static void memory_stress_worker(void *arg) {
    (void)arg;

    /* Wait for coordinator release barrier */
    while (__atomic_load_n(&g_stress_start, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        uintptr_t p = pmm_alloc_page();
        if (!p) {
            __atomic_fetch_add(&g_stress_alloc_failures, 1, __ATOMIC_RELAXED);
            break;
        }
        if (p % PAGE_SIZE != 0) {
            __atomic_fetch_add(&g_stress_verify_errors, 1, __ATOMIC_RELAXED);
            pmm_free_page(p);
            break;
        }
        size_t frame = p / PAGE_SIZE;
        if (frame >= PMM_MAX_FRAMES) {
            __atomic_fetch_add(&g_stress_verify_errors, 1, __ATOMIC_RELAXED);
            pmm_free_page(p);
            break;
        }
        if (!claim_frame(frame)) {
            __atomic_fetch_add(&g_stress_duplicate_errors, 1, __ATOMIC_RELAXED);
            pmm_free_page(p);
            break;
        }

        /* Write pattern and read back across page */
        volatile uint64_t *v = (volatile uint64_t *)vmm_phys_to_virt(p);
        uint64_t pattern = p ^ 0xC35A96E187B40D2FULL;
        v[0] = pattern;
        v[256] = pattern + 1;
        v[511] = ~pattern;
        if (v[0] != pattern || v[256] != (pattern + 1) || v[511] != ~pattern) {
            __atomic_fetch_add(&g_stress_verify_errors, 1, __ATOMIC_RELAXED);
        }

        release_frame(frame);
        pmm_free_page(p);
    }

    /* Signal completion of iterations */
    __atomic_fetch_sub(&g_stress_workers_active, 1, __ATOMIC_RELEASE);

    /* Wait for coordinator post-quiescence snapshot before exiting */
    while (__atomic_load_n(&g_stress_done, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    __atomic_fetch_add(&g_stress_workers_terminated, 1, __ATOMIC_RELEASE);
    thread_exit();
}

void memory_stress_test_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("SMP Piece 6B: PMM Concurrent Multi-Core Stress Test\n");
    serial_puts("========================================================\n");
    serial_puts("[TEST] SMP memory 6B: Spawning 32 workers across ");
    serial_print_dec(total_cpus);
    serial_puts(" CPU(s) (10,000 iterations each)...\n");

    /* Drain any prior terminated threads and ensure reap */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        thread_yield();
    }

    memset(g_frame_owner_table, 0, sizeof(g_frame_owner_table));
    g_stress_start = 0;
    g_stress_done = 0;
    g_stress_workers_active = STRESS_WORKER_COUNT;
    g_stress_workers_terminated = 0;
    g_stress_duplicate_errors = 0;
    g_stress_alloc_failures = 0;
    g_stress_verify_errors = 0;

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();

    /* 1. Spawn 32 pinned workers across available CPUs */
    for (size_t i = 0; i < STRESS_WORKER_COUNT; i++) {
        size_t target_cpu = i % total_cpus;
        tcb_t *w = thread_create_on_cpu(target_cpu, "pmm_stress", memory_stress_worker, (void *)(uintptr_t)i);
        if (!w) {
            serial_puts("[FAIL] SMP memory 6B: Failed to create worker thread!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    /* 2. Take baseline snapshot while all 32 workers are parked at start barrier */
    pmm_stats_t stats_before, stats_after;
    uint64_t acq_before = 0, cont_before = 0;
    uint64_t acq_after = 0, cont_after = 0;

    require(pmm_snapshot(before, sizeof(before)), "baseline snapshot before stress run");
    pmm_get_stats(&stats_before);
    pmm_get_lock_stats(&acq_before, &cont_before);

    serial_puts("       [INFO] Baseline snapshot taken: used=");
    serial_print_dec(stats_before.used_pages);
    serial_puts(" free=");
    serial_print_dec(stats_before.free_pages);
    serial_puts("\n");

    /* 3. Release start barrier */
    __atomic_store_n(&g_stress_start, 1, __ATOMIC_RELEASE);
    serial_puts("       [INFO] Workers released to start barrier...\n");

    /* 4. Wait for all workers to finish their 10,000 iterations */
    uint64_t wait_timeout = 200000000ULL;
    while (__atomic_load_n(&g_stress_workers_active, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
        thread_yield();
        if (--wait_timeout == 0) {
            serial_puts("[FAIL] SMP memory 6B: Timed out waiting for workers to complete iterations!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    /* 5. Take post-stress snapshot while workers are still parked at done barrier */
    require(pmm_snapshot(after, sizeof(after)), "post-stress snapshot");
    pmm_get_stats(&stats_after);
    pmm_get_lock_stats(&acq_after, &cont_after);

    serial_puts("       [INFO] All 32 workers completed 320,000 alloc/free iterations\n");

    /* 6. Load-bearing assertions */
    uint32_t dup_err = __atomic_load_n(&g_stress_duplicate_errors, __ATOMIC_ACQUIRE);
    if (dup_err != 0) {
        serial_puts("[FAIL] SMP memory 6B: Detected duplicate frame allocations: ");
        serial_print_dec(dup_err);
        serial_puts("\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("       [PASS] SMP memory 6B: zero duplicate frame claims across 320,000 cycles\n");

    uint32_t alloc_err = __atomic_load_n(&g_stress_alloc_failures, __ATOMIC_ACQUIRE);
    uint32_t verify_err = __atomic_load_n(&g_stress_verify_errors, __ATOMIC_ACQUIRE);
    require(alloc_err == 0, "unexpected allocation failures during stress test");
    require(verify_err == 0, "pattern verification or alignment errors during stress test");
    serial_puts("       [PASS] SMP memory 6B: zero verification errors, zero allocation failures\n");

    /* 7. Exact post-quiescence equality against baseline */
    require(memcmp(before, after, sizeof(before)) == 0,
            "exact bitmap post-quiescence equality mismatch");
    require(stats_before.used_pages == stats_after.used_pages,
            "used_pages mismatch against baseline");
    require(stats_before.free_pages == stats_after.free_pages,
            "free_pages mismatch against baseline");
    require(stats_before.rejected_frees == stats_after.rejected_frees,
            "rejected_frees changed during stress test");
    require(stats_before.allocation_failures == stats_after.allocation_failures,
            "allocation_failures changed during stress test");
    serial_puts("       [PASS] SMP memory 6B: exact post-quiescence equality (bitmap & stats match baseline)\n");

    /* 8. Telemetry checks */
    uint64_t delta_acq = acq_after - acq_before;
    uint64_t delta_cont = cont_after - cont_before;
    serial_puts("       [INFO] PMM lock acquires: ");
    serial_print_dec(delta_acq);
    serial_puts(" contentions: ");
    serial_print_dec(delta_cont);
    serial_puts("\n");

    require(delta_acq >= (uint64_t)STRESS_WORKER_COUNT * STRESS_ITERATIONS * 2,
            "telemetry acquire count did not reflect worker allocations");
    if (total_cpus > 1) {
        require(delta_cont > 0, "expected lock contention across multiple CPUs");
    }
    serial_puts("       [PASS] SMP memory 6B: lock telemetry verified (delta acquires: 640000+, contentions verified)\n");

    /* 9. Release workers to exit and reap all threads */
    __atomic_store_n(&g_stress_done, 1, __ATOMIC_RELEASE);

    uint64_t reap_timeout = 200000000ULL;
    while (__atomic_load_n(&g_stress_workers_terminated, __ATOMIC_ACQUIRE) < STRESS_WORKER_COUNT) {
        __asm__ volatile("pause");
        sched_reap_dead();
        thread_yield();
        if (--reap_timeout == 0) {
            serial_puts("[FAIL] SMP memory 6B: Timed out waiting for workers to terminate!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    for (int d = 0; d < 10; d++) {
        sched_reap_dead();
        thread_yield();
    }

    require(sched_get_active_stack_slots_mask() == initial_stack_mask,
            "stack slot leak: active slots mask did not return to initial state");
    serial_puts("       [PASS] SMP memory 6B: all worker stacks reaped cleanly\n");
    serial_puts("[ OK ] SMP Piece 6B (PMM Concurrent Multi-Core Safety) complete.\n\n");
}

