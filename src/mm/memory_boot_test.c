#include "memory_boot_test.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "apic.h"
#include "serial.h"
#include "string.h"
#include "thread.h"
#include "vfs.h"
#include "usb_mount.h"
#include "syscall_abi.h"
#include "percpu.h"
#include "smp.h"

/* Pre-heap tests need static snapshot storage. It remains allocated, including
 * in the baseline. Only the explicit test mode performs these diagnostics. */
static uint8_t before[PMM_BITMAP_CAPACITY_BYTES];
static uint8_t after[PMM_BITMAP_CAPACITY_BYTES];
static void require(bool condition, const char *reason);
static void snapshot_begin(void);
static void snapshot_end(void);

void memory_storage_test_run(void) {
    static uint8_t data[4096], readback[4096];
    static const char path[] = "/mnt/memory-probe.bin";
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 17 + 43);
    file_t *file = vfs_open_kernel(path, VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC);
    require(file != NULL, "storage probe open");
    require(vfs_write(file, data, sizeof(data)) == sizeof(data), "storage warm write");
    require(vfs_truncate_kernel(file->node, 0) == 0 && vfs_close(file) == 0 && usb_mount_sync(),
            "storage warm cleanup");
    require(heap_verify_integrity() && pmm_audit(), "storage baseline integrity");
    heap_stats_t heap_before, heap_after;
    pmm_stats_t pmm_before, pmm_after;
    heap_get_stats(&heap_before);
    pmm_get_stats(&pmm_before);
    size_t table_before = vmm_get_allocated_table_frames();
    size_t deferred_before = vmm_get_deferred_count();
    uint64_t mappings_before = vmm_kernel_mapping_fingerprint();
    snapshot_begin();
    for (unsigned round = 0; round < 10; round++) {
        file = vfs_open_kernel(path, VFS_O_RDWR);
        require(file != NULL, "storage repeated open");
        require(vfs_write(file, data, sizeof(data)) == sizeof(data), "storage repeated write");
        file->offset = 0;
        require(vfs_read(file, readback, sizeof(readback)) == sizeof(readback) &&
                memcmp(data, readback, sizeof(data)) == 0, "storage exact readback");
        require(vfs_truncate_kernel(file->node, 0) == 0 && vfs_close(file) == 0 && usb_mount_sync(),
                "storage truncate/close/sync");
        require(heap_verify_integrity() && pmm_audit(), "storage repeated integrity");
    }
    heap_get_stats(&heap_after);
    pmm_get_stats(&pmm_after);
    snapshot_end();
    require(heap_before.used_bytes == heap_after.used_bytes &&
            heap_before.allocated_blocks == heap_after.allocated_blocks &&
            heap_before.total_bytes == heap_after.total_bytes &&
            heap_before.free_bytes == heap_after.free_bytes &&
            heap_before.free_blocks == heap_after.free_blocks &&
            heap_before.largest_free_payload == heap_after.largest_free_payload,
            "storage heap baseline mismatch");
    require(pmm_before.used_pages == pmm_after.used_pages &&
            pmm_before.free_pages == pmm_after.free_pages &&
            pmm_before.rejected_frees == pmm_after.rejected_frees &&
            pmm_before.allocation_failures == pmm_after.allocation_failures &&
            table_before == vmm_get_allocated_table_frames() &&
            deferred_before == vmm_get_deferred_count() &&
            mappings_before == vmm_kernel_mapping_fingerprint(), "storage frame/table baseline mismatch");
    serial_puts("[MEMORY STORAGE] PASS ten warmed write/read/truncate/close/sync cycles; exact bitmap/heap/table equality\n");
    serial_puts("[MEMORY STORAGE] heap_used="); serial_print_dec(heap_after.used_bytes);
    serial_puts(" committed="); serial_print_dec(heap_after.total_bytes);
    serial_puts(" largest_payload="); serial_print_dec(heap_after.largest_free_payload);
    serial_puts(" free_blocks="); serial_print_dec(heap_after.free_blocks);
    serial_puts(" pmm_free="); serial_print_dec(pmm_after.free_pages);
    serial_puts(" tables="); serial_print_dec(table_before); serial_puts("\n");
    require(vfs_unlink_kernel(path) == 0 && usb_mount_sync(), "storage unlink cleanup");
}

void memory_storage_churn_test_run(void) {
    static const char path[] = "/mnt/memory-churn.bin";
    heap_stats_t baseline, completed;
    file_t *warm=vfs_open_kernel(path,VFS_O_CREAT|VFS_O_RDWR);
    require(warm && vfs_close(warm)==0 && vfs_unlink_kernel(path)==0, "churn warm cleanup");
    heap_get_stats(&baseline);
    snapshot_begin();
    unsigned rounds = 0;
    int error = 0;
    for (; rounds < 1152; rounds++) {
        file_t *file = vfs_open_ext_kernel(path, VFS_O_CREAT | VFS_O_RDWR, &error);
        require(file != NULL && error==0, "churn create past old capacity");
        require(vfs_close(file) == 0 && vfs_unlink_kernel(path) == 0 && vfs_lookup_kernel(path) == NULL,
                "churn close/unlink/absence");
        if ((rounds + 1) % 128 == 0) {
            require(heap_verify_integrity() && pmm_audit(), "churn periodic integrity");
            serial_puts("[MEMORY CHURN] progress="); serial_print_dec(rounds + 1); serial_puts("\n");
        }
    }
    heap_get_stats(&completed);
    snapshot_end();
    require(completed.allocated_blocks == baseline.allocated_blocks &&
            completed.used_bytes == baseline.used_bytes && completed.total_bytes == baseline.total_bytes &&
            heap_verify_integrity() && pmm_audit() && usb_mount_sync(), "churn exact memory cleanup/sync");
    serial_puts("[MEMORY CHURN] PASS rounds="); serial_print_dec(rounds);
    serial_puts(" heap_used_delta=0 retained_blocks=0");
    serial_puts(" committed_before="); serial_print_dec(baseline.total_bytes);
    serial_puts(" committed_after="); serial_print_dec(completed.total_bytes);
    serial_puts("; exact bitmap, absent file, sync and memory audits\n");
    file_t *existing=vfs_open_kernel("/mnt/README.txt",VFS_O_RDONLY);
    require(existing != NULL, "churn existing file lookup");
    uint8_t byte;
    require(vfs_read(existing,&byte,1)==1 && vfs_close(existing)==0, "churn existing file read");
    serial_puts("[MEMORY CHURN] existing uncached file read PASS\n");
}

static bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t pressure_round(const char *label) {
    heap_stats_t heap, restored;
    pmm_stats_t baseline, exhausted, recovered;
    heap_get_stats(&heap);pmm_get_stats(&baseline);
    size_t tables=vmm_get_allocated_table_frames();
    snapshot_begin();
    uintptr_t head=0,cursor=PAGE_SIZE;
    size_t held=0;
    for (;;) {
        /* Monotonic selection avoids a first-fit rescan for every held frame;
         * this is an ownership/pressure test, not an allocator timing test. */
        uintptr_t page=pmm_alloc_page_above(cursor);
        if (!page) break;
        require(page>=cursor && held<baseline.total_pages, "pressure frame bound");
        uint64_t *mapped=vmm_phys_to_virt(page);
        mapped[0]=head;mapped[PAGE_SIZE/sizeof(*mapped)-1]=page^0x5372AC19ULL;
        head=page;cursor=page+PAGE_SIZE;held++;
    }
    require(held==baseline.free_pages && pmm_alloc_page()==0 && pmm_alloc_pages(2)==0,
            "pressure PMM exhaustion");
    pmm_get_stats(&exhausted);
    require(exhausted.free_pages==0 && heap.largest_free_payload>0, "pressure free domains");
    void *reuse=kmalloc(heap.largest_free_payload);
    require(reuse!=NULL, "pressure existing heap reuse");
    memset(reuse,0x6b,heap.largest_free_payload);
    require(kmalloc(16)==NULL, "pressure expansion should fail");
    require(((uint8_t *)reuse)[0]==0x6b &&
            ((uint8_t *)reuse)[heap.largest_free_payload-1]==0x6b, "pressure payload preservation");
    kfree(reuse);
    size_t released=0;
    while (head) {
        uint64_t *mapped=vmm_phys_to_virt(head);
        require(mapped[PAGE_SIZE/sizeof(*mapped)-1]==(head^0x5372AC19ULL) && released<held,
                "pressure held-frame ownership");
        uintptr_t next=mapped[0];pmm_free_page(head);head=next;released++;
    }
    require(released==held, "pressure frame release count");
    snapshot_end();heap_get_stats(&restored);pmm_get_stats(&recovered);
    require(restored.used_bytes==heap.used_bytes && restored.allocated_blocks==heap.allocated_blocks &&
            restored.total_bytes==heap.total_bytes && restored.free_bytes==heap.free_bytes &&
            restored.free_blocks==heap.free_blocks && restored.largest_free_payload==heap.largest_free_payload &&
            recovered.free_pages==baseline.free_pages && recovered.used_pages==baseline.used_pages &&
            recovered.rejected_frees==baseline.rejected_frees && tables==vmm_get_allocated_table_frames() &&
            heap_verify_integrity() && pmm_audit(), "pressure exact cleanup");
    serial_puts("[MEMORY PRESSURE] ");serial_puts(label);
    serial_puts(" held_pages=");serial_print_dec(held);
    serial_puts(" heap_free=");serial_print_dec(heap.free_bytes);
    serial_puts(" heap_committed=");serial_print_dec(heap.total_bytes);
    serial_puts(" largest_payload=");serial_print_dec(heap.largest_free_payload);
    serial_puts(" oom_delta=");serial_print_dec(recovered.allocation_failures-baseline.allocation_failures);
    serial_puts(" tables=");serial_print_dec(tables);
    serial_puts(" PASS exact bitmap/heap cleanup; PMM OOM, heap reuse, expansion OOM\n");
    return held;
}

void memory_pressure_test_run(void) {
    require(vmm_boot_memory_ready() && pmm_high_memory_enabled() &&
            pmm_get_total_pages()<=131072 && heap_get_used_bytes()==0,
            "pressure exclusive low-RAM boot admission");
    size_t initial=pressure_round("initial");
    heap_stats_t before_growth,retained;
    heap_get_stats(&before_growth);
    size_t table_before=vmm_get_allocated_table_frames();
    void *burst[8];
    for (unsigned i=0;i<8;i++) {
        burst[i]=kmalloc(512*1024);require(burst[i]!=NULL, "pressure heap burst");
        memset(burst[i],(int)i+1,512*1024);
    }
    for (unsigned i=0;i<8;i++) {
        require(((uint8_t *)burst[i])[0]==i+1 && ((uint8_t *)burst[i])[512*1024-1]==i+1,
                "pressure burst payload");kfree(burst[i]);
    }
    heap_get_stats(&retained);
    require(retained.used_bytes==0 && retained.allocated_blocks==0 && retained.free_blocks==1 &&
            retained.free_bytes>=4*1024*1024, "pressure retained free backing");
    size_t final=pressure_round("retained");
    size_t data_pages=(retained.total_bytes-before_growth.total_bytes)/PAGE_SIZE;
    size_t table_pages=vmm_get_allocated_table_frames()-table_before;
    require(initial==final+data_pages+table_pages, "pressure retained frame attribution");
    serial_puts("[MEMORY PRESSURE] PASS retained_data_pages=");serial_print_dec(data_pages);
    serial_puts(" retained_table_pages=");serial_print_dec(table_pages);
    serial_puts(" page_consumer_capacity_lost=");serial_print_dec(initial-final);
    serial_puts("; grow-only policy unchanged\n");
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

/* =========================================================================
 * SMP Piece 6D: Address Space Lifetime & Deferred Reaping Hardware Test
 * ========================================================================= */

bool memory_vmm_lifecycle_test_enabled(const boot_info_t *boot_info) {
    static const char token[] = "smp_memory_test=vmm_lifecycle";
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

static void require_6d(bool condition, const char *reason) {
    if (condition) return;
    serial_puts("[FAIL] SMP memory 6D: ");
    serial_puts(reason);
    serial_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void memory_vmm_lifecycle_test_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("SMP Piece 6D: Address-Space Lifetime & Deferred Reaping\n");
    serial_puts("========================================================\n");

    /* Ensure initial quiescence: reap any dead threads and drain deferred queue */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_heap_bytes = heap_get_total_bytes();
    size_t baseline_heap_used = heap_get_used_bytes();
    require_6d(pmm_snapshot(before, sizeof(before)), "baseline allocation set");
    uint64_t baseline_stack_slots = sched_get_active_stack_slots_mask();
    size_t initial_deferred = vmm_get_deferred_count();

    require_6d(initial_deferred == 0, "initial deferred destruction queue not empty");

    serial_puts("       [INFO] Baseline snapshot: allocated_tables=");
    serial_print_dec(baseline_allocated_tables);
    serial_puts(" free_pages=");
    serial_print_dec(baseline_free_pages);
    serial_puts("\n");

    /* Part 1: Explicit VMM_ERR_BUSY deferral, queueing, and drainage verification */
    serial_puts("[TEST 1] SMP memory 6D: Testing VMM_ERR_BUSY deferral and deferred list drainage...\n");
    uintptr_t u1 = vmm_create_user_pml4();
    require_6d(u1 != 0, "vmm_create_user_pml4 failed");
    vmm_space_t *s1 = vmm_space_lookup(u1);
    require_6d(s1 != NULL && s1->state == VMM_SPACE_LIVE, "user space not registered as LIVE");

    uintptr_t tf1 = pmm_alloc_page();
    uintptr_t tf2 = pmm_alloc_page();
    require_6d(tf1 != 0 && tf2 != 0, "failed to allocate test data pages");
    require_6d(vmm_map_page(s1->pml4_virt, 0x400000, tf1, PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK, "map tf1");
    require_6d(vmm_map_page(s1->pml4_virt, 0x800000, tf2, PTE_PRESENT | PTE_WRITABLE | PTE_USER) == VMM_OK, "map tf2");

    require_6d(vmm_space_get_op(s1->pml4_virt) == VMM_OK, "vmm_space_get_op failed");
    require_6d(s1->op_refs == 1, "op_refs mismatch");

    /* While op_ref held: destruction must return VMM_ERR_BUSY, transition to DYING, and enqueue */
    int busy_res = vmm_destroy_pml4(u1, true);
    require_6d(busy_res == VMM_ERR_BUSY, "vmm_destroy_pml4 did not return VMM_ERR_BUSY");
    require_6d(s1->state == VMM_SPACE_DYING, "busy space state not DYING");
    require_6d(s1->deferred_queued, "busy space not marked deferred_queued");
    require_6d(vmm_get_deferred_count() == 1, "deferred count not 1");

    /* Verify DYING space rejects new ops and sched refs */
    require_6d(vmm_space_get_op(s1->pml4_virt) == VMM_ERR_INVALID_ADDR, "DYING accepted op_ref");
    require_6d(vmm_space_add_sched_ref(u1) == VMM_ERR_INVALID_ADDR, "DYING accepted sched_ref");

    /* Release op_ref and drain deferred destruction queue */
    vmm_space_put_op(s1->pml4_virt);
    size_t drained = vmm_drain_deferred_destructions();
    require_6d(drained == 1, "deferred drainage count mismatch");
    require_6d(vmm_get_deferred_count() == 0, "deferred list not empty after drain");
    require_6d(vmm_space_lookup(u1) == NULL, "space still in registry after drain");
    require_6d(vmm_get_allocated_table_frames() == baseline_allocated_tables, "table frames leaked in Part 1");
    require_6d(pmm_get_free_pages() == baseline_free_pages, "data pages leaked in Part 1");

    serial_puts("       [PASS] SMP memory 6D: VMM_ERR_BUSY deferral, queueing, and drainage verified\n");

    /* Part 2: 100 spawn/exit cycles across cores */
    extern const uint8_t embedded_init_elf_start[];
    extern const uint8_t embedded_init_elf_end[];
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    serial_puts("[TEST 2] SMP memory 6D: Running 100 spawn/exit cycles across ");
    serial_print_dec(total_cpus);
    serial_puts(" CPU(s)...\n");

    for (int cycle = 1; cycle <= 100; cycle++) {
        size_t target_cpu = (cycle - 1) % total_cpus;
        tcb_t *proc = process_spawn_on_cpu(target_cpu, "vmm_worker",
                                           embedded_init_elf_start, init_elf_size, 9);
        if (!proc) {
            serial_puts("[FAIL] SMP memory 6D: process_spawn_on_cpu failed at cycle ");
            serial_print_dec(cycle);
            serial_puts("\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        uint64_t pid = proc->tid;

        uint64_t exit_code = 0;
        bool wait_ok = process_wait(pid, &exit_code);
        if (!wait_ok) {
            serial_puts("[FAIL] SMP memory 6D: process_wait failed for PID ");
            serial_print_dec(pid);
            serial_puts("\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        if (exit_code != 42) {
            serial_puts("[FAIL] SMP memory 6D: Unexpected exit code ");
            serial_print_dec(exit_code);
            serial_puts(" (expected 42)\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }

        sched_reap_dead();
        vmm_drain_deferred_destructions();

        if (cycle % 25 == 0) {
            serial_puts("       [INFO] Completed ");
            serial_print_dec(cycle);
            serial_puts("/100 cycles...\n");
        }
    }

    /* A remote idle thread may already have detached the last dead task and
     * still be freeing its stack outside the scheduler lock. Ten local yields
     * do not establish quiescence. Wait for those retained resources to return
     * before taking the exact accounting snapshots; never relax equality. */
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == baseline_stack_slots &&
            heap_get_used_bytes() == baseline_heap_used &&
            vmm_get_deferred_count() == 0) break;
        require_6d(apic_timer_get_ticks() < reap_deadline, "final reaper quiescence timeout");
        thread_yield();
    }

    size_t final_deferred = vmm_get_deferred_count();
    require_6d(final_deferred == 0, "deferred destructions remaining");
    serial_puts("       [PASS] SMP memory 6D: zero deferred destructions remaining (all drained)\n");

    size_t final_allocated_tables = vmm_get_allocated_table_frames();
    if (final_allocated_tables != baseline_allocated_tables) {
        serial_puts("[FAIL] SMP memory 6D: Table frame mismatch! Baseline: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Final: ");
        serial_print_dec(final_allocated_tables);
        serial_puts("\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("       [PASS] SMP memory 6D: exact table-frame counter equality (matches baseline)\n");

    size_t final_free_pages = pmm_get_free_pages();
    if (final_free_pages != baseline_free_pages) {
        serial_puts("[INFO] SMP memory 6D: Free pages mismatch! Baseline: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Final: ");
        serial_print_dec(final_free_pages);
        serial_puts("\n");
        serial_puts("       [INFO] Heap capacity baseline/final: ");
        serial_print_dec(baseline_heap_bytes);
        serial_puts(" / ");
        serial_print_dec(heap_get_total_bytes());
        serial_puts("; used baseline/final: ");
        serial_print_dec(baseline_heap_used);
        serial_puts(" / ");
        serial_print_dec(heap_get_used_bytes());
        serial_puts("\n");
        serial_puts("       [INFO] Stack slots baseline/final: ");
        serial_print_hex(baseline_stack_slots);
        serial_puts(" / ");
        serial_print_hex(sched_get_active_stack_slots_mask());
        serial_puts("\n");
        require_6d(pmm_snapshot(after, sizeof(after)), "final allocation set");
        unsigned shown = 0;
        for (size_t bit = 0; bit < sizeof(before) * 8 && shown < 16; bit++) {
            uint8_t mask = (uint8_t)(1U << (bit % 8));
            if ((before[bit / 8] ^ after[bit / 8]) & mask) {
                serial_puts("       [INFO] Changed frame: ");
                serial_print_hex(bit * PAGE_SIZE);
                serial_puts(after[bit / 8] & mask ? " allocated\n" : " freed\n");
                shown++;
            }
        }
        require_6d(false, "free pages mismatch after final reap");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("       [PASS] SMP memory 6D: exact physical frame equality (zero frame leaks)\n");
    require_6d(pmm_snapshot(after, sizeof(after)) &&
               memcmp(before, after, sizeof(before)) == 0,
               "physical allocation set mismatch");
    serial_puts("       [PASS] SMP memory 6D: exact physical allocation-set equality\n");

    uint64_t final_stack_slots = sched_get_active_stack_slots_mask();
    require_6d(final_stack_slots == baseline_stack_slots, "kernel stack slots leaked");
    serial_puts("       [PASS] SMP memory 6D: all worker stacks reaped cleanly\n");
    serial_puts("[ OK ] SMP Piece 6D (Address-Space Lifetime & Deferred Reaping) complete.\n\n");
}

/* =========================================================================
 * Representative Process Burst & Bounded PMM Pressure Investigation
 * ========================================================================= */

bool memory_burst_test_enabled(const boot_info_t *boot_info) {
    static const char token[] = "smp_memory_test=burst";
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

static void require_burst(bool condition, const char *reason) {
    if (condition) return;
    serial_puts("[FAIL] Memory burst: ");
    serial_puts(reason);
    serial_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void memory_burst_test_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("Memory Investigation: Representative Process Burst & Recovery\n");
    serial_puts("========================================================\n");

    require_burst(vmm_boot_memory_ready() && pmm_high_memory_enabled(),
                  "memory readiness invariant");

    /* Initial quiescence */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    extern const uint8_t embedded_init_elf_start[];
    extern const uint8_t embedded_init_elf_end[];
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    /* 1. Warmup: 4 process spawn/exit cycles to warm up stack slot mapping and TCB sizing */
    for (size_t w = 0; w < 4; w++) {
        size_t target_cpu = w % total_cpus;
        tcb_t *proc = process_spawn_on_cpu(target_cpu, "burst_warm",
                                           embedded_init_elf_start, init_elf_size, 9);
        require_burst(proc != NULL, "warmup process spawn failed");
        uint64_t exit_code = 0;
        require_burst(process_wait(proc->tid, &exit_code) && exit_code == 42,
                      "warmup process wait failed");
        sched_reap_dead();
        vmm_drain_deferred_destructions();
    }

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require_burst(apic_timer_get_ticks() < reap_deadline, "warmup quiescence timeout");
        thread_yield();
    }

    /* 2. Warmed Baseline Snapshot */
    heap_stats_t baseline_heap;
    pmm_stats_t baseline_pmm;
    heap_get_stats(&baseline_heap);
    pmm_get_stats(&baseline_pmm);
    size_t baseline_tables = vmm_get_allocated_table_frames();

    require_burst(vmm_get_deferred_count() == 0, "baseline deferred queue not empty");
    require_burst(pmm_snapshot(before, sizeof(before)), "baseline pmm snapshot");
    require_burst(heap_verify_integrity() && pmm_audit(), "baseline integrity audit");

    serial_puts("[MEMORY BURST] baseline heap_used=");
    serial_print_dec(baseline_heap.used_bytes);
    serial_puts(" heap_committed=");
    serial_print_dec(baseline_heap.total_bytes);
    serial_puts(" heap_free=");
    serial_print_dec(baseline_heap.free_bytes);
    serial_puts(" largest_payload=");
    serial_print_dec(baseline_heap.largest_free_payload);
    serial_puts(" pmm_free=");
    serial_print_dec(baseline_pmm.free_pages);
    serial_puts(" tables=");
    serial_print_dec(baseline_tables);
    serial_puts(" deferred=0\n");

    /* 3. Representative Process Burst: 32 process spawn/exit cycles across CPUs */
    serial_puts("[MEMORY BURST] executing 32 process spawn/exit cycles...\n");
    for (int cycle = 1; cycle <= 32; cycle++) {
        size_t target_cpu = (cycle - 1) % total_cpus;
        tcb_t *proc = process_spawn_on_cpu(target_cpu, "burst_worker",
                                           embedded_init_elf_start, init_elf_size, 9);
        require_burst(proc != NULL, "burst process spawn failed");
        uint64_t exit_code = 0;
        require_burst(process_wait(proc->tid, &exit_code) && exit_code == 42,
                      "burst process wait failed");

        /* Interleaved kernel heap allocation to exercise kernel metadata churn */
        void *kbuf = kmalloc(256 + (cycle % 8) * 64);
        if (kbuf) {
            memset(kbuf, 0x3c + (cycle & 0xf), 256 + (cycle % 8) * 64);
            kfree(kbuf);
        }

        sched_reap_dead();
        vmm_drain_deferred_destructions();
    }

    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            heap_get_used_bytes() == baseline_heap.used_bytes &&
            vmm_get_deferred_count() == 0) break;
        require_burst(apic_timer_get_ticks() < reap_deadline, "post-burst quiescence timeout");
        thread_yield();
    }

    /* 4. Post-Burst Snapshot & Metrics */
    heap_stats_t burst_heap;
    pmm_stats_t burst_pmm;
    heap_get_stats(&burst_heap);
    pmm_get_stats(&burst_pmm);
    size_t burst_tables = vmm_get_allocated_table_frames();

    require_burst(vmm_get_deferred_count() == 0, "post-burst deferred remaining");
    require_burst(sched_get_active_stack_slots_mask() == initial_stack_mask,
                  "post-burst stack slot leak");
    require_burst(burst_heap.used_bytes == baseline_heap.used_bytes,
                  "post-burst live heap used mismatch");
    require_burst(burst_heap.allocated_blocks == baseline_heap.allocated_blocks,
                  "post-burst allocated blocks mismatch");
    require_burst(heap_verify_integrity() && pmm_audit(), "post-burst integrity audit");

    size_t retained_heap_delta = burst_heap.total_bytes >= baseline_heap.total_bytes
        ? burst_heap.total_bytes - baseline_heap.total_bytes : 0;
    size_t table_delta = burst_tables >= baseline_tables ? burst_tables - baseline_tables : 0;

    serial_puts("[MEMORY BURST] post_burst heap_used=");
    serial_print_dec(burst_heap.used_bytes);
    serial_puts(" heap_committed=");
    serial_print_dec(burst_heap.total_bytes);
    serial_puts(" heap_free=");
    serial_print_dec(burst_heap.free_bytes);
    serial_puts(" largest_payload=");
    serial_print_dec(burst_heap.largest_free_payload);
    serial_puts(" pmm_free=");
    serial_print_dec(burst_pmm.free_pages);
    serial_puts(" tables=");
    serial_print_dec(burst_tables);
    serial_puts(" deferred=0 retained_heap_delta=");
    serial_print_dec(retained_heap_delta);
    serial_puts(" table_delta=");
    serial_print_dec(table_delta);
    serial_puts("\n");

    /* 5. Bounded PMM Pressure & Real Page Consumer Demand */
    serial_puts("[MEMORY BURST] testing bounded PMM pressure...\n");
    uintptr_t head = 0, cursor = PAGE_SIZE;
    size_t held = 0;

    /* Step 5A: Hold free frames leaving 64 free frames for real page consumer */
    while (pmm_get_free_pages() > 64) {
        uintptr_t page = pmm_alloc_page_above(cursor);
        if (!page) {
            cursor = PAGE_SIZE;
            page = pmm_alloc_page_above(cursor);
            if (!page) break;
        }
        uint64_t *mapped = vmm_phys_to_virt(page);
        mapped[0] = head;
        mapped[PAGE_SIZE / sizeof(*mapped) - 1] = page ^ 0x5372AC19ULL;
        head = page;
        cursor = page + PAGE_SIZE;
        held++;
    }
    require_burst(pmm_get_free_pages() <= 64, "failed to constrain free frames to 64");

    /* Real page consumer test: 64-frame headroom is sufficient for 1 process (~10-12 frames) */
    tcb_t *p64 = process_spawn_on_cpu(0, "burst_p64", embedded_init_elf_start, init_elf_size, 9);
    require_burst(p64 != NULL, "process spawn under 64-frame headroom failed");
    uint64_t code64 = 0;
    require_burst(process_wait(p64->tid, &code64) && code64 == 42,
                  "process execution under 64-frame headroom failed");
    sched_reap_dead();
    vmm_drain_deferred_destructions();

    /* Reusable heap backing test: allocation within existing free capacity must succeed without PMM */
    if (burst_heap.largest_free_payload >= 4096) {
        void *reuse_buf = kmalloc(4096);
        require_burst(reuse_buf != NULL, "reusable heap allocation failed under pressure");
        memset(reuse_buf, 0x4a, 4096);
        kfree(reuse_buf);
    }

    /* Step 5B: Constrain to tightest bound (2 free frames) */
    while (pmm_get_free_pages() > 2) {
        uintptr_t page = pmm_alloc_page_above(cursor);
        if (!page) {
            cursor = PAGE_SIZE;
            page = pmm_alloc_page_above(cursor);
            if (!page) break;
        }
        uint64_t *mapped = vmm_phys_to_virt(page);
        mapped[0] = head;
        mapped[PAGE_SIZE / sizeof(*mapped) - 1] = page ^ 0x5372AC19ULL;
        head = page;
        cursor = page + PAGE_SIZE;
        held++;
    }
    require_burst(pmm_get_free_pages() <= 2, "failed to constrain free frames to 2");

    /* Insufficient headroom test: Process spawn must cleanly fail with ENOMEM / NULL */
    tcb_t *p_oom = process_spawn_on_cpu(0, "burst_poom", embedded_init_elf_start, init_elf_size, 9);
    require_burst(p_oom == NULL, "process spawn should fail cleanly under 2-frame headroom");
    sched_reap_dead();
    vmm_drain_deferred_destructions();

    /* Heap expansion beyond existing capacity must fail cleanly */
    void *fill = kmalloc(burst_heap.largest_free_payload);
    if (fill) {
        void *exp = kmalloc(32 * PAGE_SIZE);
        require_burst(exp == NULL, "heap expansion should fail under 2-frame headroom");
        kfree(fill);
    }

    /* Release all held pressure frames */
    size_t released = 0;
    while (head) {
        uint64_t *mapped = vmm_phys_to_virt(head);
        require_burst(mapped[PAGE_SIZE / sizeof(*mapped) - 1] == (head ^ 0x5372AC19ULL),
                      "corrupted held frame signature");
        uintptr_t next = mapped[0];
        pmm_free_page(head);
        head = next;
        released++;
    }
    require_burst(released == held, "held/released frames count mismatch");

    serial_puts("[MEMORY BURST] pressure held_pages=");
    serial_print_dec(held);
    serial_puts(" headroom_64=PASS proc_spawn_64=PASS headroom_2=PASS proc_spawn_oom=PASS heap_reuse=PASS\n");

    /* 6. Post-Pressure Recovery */
    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            heap_get_used_bytes() == baseline_heap.used_bytes &&
            vmm_get_deferred_count() == 0) break;
        require_burst(apic_timer_get_ticks() < reap_deadline, "recovery quiescence timeout");
        thread_yield();
    }

    heap_stats_t rec_heap;
    pmm_stats_t rec_pmm;
    heap_get_stats(&rec_heap);
    pmm_get_stats(&rec_pmm);
    size_t rec_tables = vmm_get_allocated_table_frames();

    require_burst(vmm_get_deferred_count() == 0, "recovery deferred remaining");
    require_burst(sched_get_active_stack_slots_mask() == initial_stack_mask,
                  "recovery stack slot leak");
    require_burst(rec_heap.used_bytes == burst_heap.used_bytes,
                  "recovery heap used mismatch");
    require_burst(rec_heap.allocated_blocks == burst_heap.allocated_blocks,
                  "recovery heap blocks mismatch");
    require_burst(rec_heap.total_bytes == burst_heap.total_bytes,
                  "recovery heap total mismatch");
    require_burst(rec_heap.free_bytes == burst_heap.free_bytes,
                  "recovery heap free mismatch");
    require_burst(rec_tables == burst_tables, "recovery tables mismatch");
    require_burst(rec_pmm.free_pages == burst_pmm.free_pages,
                  "recovery pmm free pages mismatch");
    require_burst(heap_verify_integrity() && pmm_audit(), "recovery integrity audit");

    if (burst_pmm.free_pages == baseline_pmm.free_pages && burst_tables == baseline_tables) {
        require_burst(pmm_snapshot(after, sizeof(after)) &&
                      memcmp(before, after, sizeof(before)) == 0,
                      "recovery exact bitmap mismatch against baseline");
    }

    serial_puts("[MEMORY BURST] recovery exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS\n");
    serial_puts("[MEMORY BURST] PASS representative process bursts and recovery under bounded PMM pressure\n");
}

bool memory_cohort_test_enabled(const boot_info_t *boot_info) {
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    static const char token[] = "smp_memory_test=cohort";
    size_t len = 0;
    while (cmd[len] && len < sizeof(boot_info->cmdline)) len++;
    for (size_t i = 0; i + sizeof(token) - 1 <= len; i++) {
        if ((i == 0 || cmd[i - 1] == ' ') &&
            (i + sizeof(token) - 1 == len || cmd[i + sizeof(token) - 1] == ' ') &&
            memcmp(cmd + i, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

static void require_cohort(bool condition, const char *reason) {
    if (condition) return;
    serial_puts("[FAIL] Memory cohort: ");
    serial_puts(reason);
    serial_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static int count_bits64(uint64_t v) {
    int count = 0;
    while (v) {
        count += (int)(v & 1ULL);
        v >>= 1;
    }
    return count;
}

void memory_cohort_test_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("Memory Investigation: Concurrent Process Peaks & Fragmented PMM\n");
    serial_puts("========================================================\n");

    require_cohort(vmm_boot_memory_ready() && pmm_high_memory_enabled(),
                   "memory readiness invariant");

    /* Initial quiescence */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    extern const uint8_t embedded_init_elf_start[];
    extern const uint8_t embedded_init_elf_end[];
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    /* 1. Warmup: 2 process spawn/exit cycles to warm up stack slot mapping and TCB sizing */
    for (size_t w = 0; w < 2; w++) {
        size_t target_cpu = w % total_cpus;
        tcb_t *proc = process_spawn_on_cpu(target_cpu, "cohort_warm",
                                           embedded_init_elf_start, init_elf_size, 9);
        require_cohort(proc != NULL, "warmup process spawn failed");
        uint64_t exit_code = 0;
        require_cohort(process_wait(proc->tid, &exit_code) && exit_code == 42,
                       "warmup process wait failed");
        sched_reap_dead();
        vmm_drain_deferred_destructions();
    }

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require_cohort(apic_timer_get_ticks() < reap_deadline, "warmup quiescence timeout");
        thread_yield();
    }

    /* 2. Warmed Baseline Snapshot */
    heap_stats_t baseline_heap;
    pmm_stats_t baseline_pmm;
    heap_get_stats(&baseline_heap);
    pmm_get_stats(&baseline_pmm);
    size_t baseline_tables = vmm_get_allocated_table_frames();

    require_cohort(vmm_get_deferred_count() == 0, "baseline deferred queue not empty");
    require_cohort(pmm_snapshot(before, sizeof(before)), "baseline pmm snapshot");
    require_cohort(heap_verify_integrity() && pmm_audit(), "baseline integrity audit");

    serial_puts("[MEMORY COHORT] baseline heap_used=");
    serial_print_dec(baseline_heap.used_bytes);
    serial_puts(" heap_committed=");
    serial_print_dec(baseline_heap.total_bytes);
    serial_puts(" heap_free=");
    serial_print_dec(baseline_heap.free_bytes);
    serial_puts(" largest_payload=");
    serial_print_dec(baseline_heap.largest_free_payload);
    serial_puts(" pmm_free=");
    serial_print_dec(baseline_pmm.free_pages);
    serial_puts(" tables=");
    serial_print_dec(baseline_tables);
    serial_puts(" deferred=0\n");

    /* 3. Concurrent Cohort Peaks across Sizes 2, 4, 8 with 2 Passes Each */
    const size_t cohort_sizes[3] = {2, 4, 8};
    const size_t reap_order_2[2] = {1, 0};
    const size_t reap_order_4[4] = {3, 0, 2, 1};
    const size_t reap_order_8[8] = {7, 2, 5, 0, 6, 1, 4, 3};

    heap_stats_t post_cohort_heap = baseline_heap;
    pmm_stats_t post_cohort_pmm = baseline_pmm;
    size_t post_cohort_tables = baseline_tables;

    for (size_t c = 0; c < 3; c++) {
        size_t n = cohort_sizes[c];
        const size_t *reap_order = (n == 2) ? reap_order_2 : ((n == 4) ? reap_order_4 : reap_order_8);
        size_t pass0_committed = 0;

        for (int pass = 0; pass < 2; pass++) {
            tcb_t *procs[8];
            uint64_t tids[8];
            int slots[8];

            sched_disable_preemption();
            for (size_t i = 0; i < n; i++) {
                size_t target_cpu = (i) % total_cpus;
                procs[i] = process_spawn_on_cpu(target_cpu, "cohort_worker",
                                                embedded_init_elf_start, init_elf_size, 9);
                require_cohort(procs[i] != NULL, "cohort process spawn failed");
                tids[i] = procs[i]->tid;
                slots[i] = procs[i]->stack_slot;
                require_cohort(slots[i] >= 0 && slots[i] < 64, "invalid stack slot");
            }

            /* Verify simultaneous overlapping lifetimes */
            uint64_t active_mask = sched_get_active_stack_slots_mask();
            uint64_t cohort_mask = active_mask ^ initial_stack_mask;
            require_cohort(count_bits64(cohort_mask) == (int)n,
                           "cohort overlapping lifetime count mismatch");
            for (size_t i = 0; i < n; i++) {
                require_cohort((cohort_mask & (1ULL << slots[i])) != 0,
                               "cohort slot bit missing in active mask");
            }

            /* Record peak metrics */
            heap_stats_t peak_heap;
            heap_get_stats(&peak_heap);
            sched_enable_preemption();

            /* Mixed-order wait */
            for (size_t step = 0; step < n; step++) {
                size_t idx = reap_order[step];
                uint64_t exit_code = 0;
                require_cohort(process_wait(tids[idx], &exit_code) && exit_code == 42,
                               "cohort process wait failed");
            }

            /* Quiescence after full cohort reap */
            reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
            for (;;) {
                sched_reap_dead();
                vmm_drain_deferred_destructions();
                if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
                    heap_get_used_bytes() == baseline_heap.used_bytes &&
                    vmm_get_deferred_count() == 0) break;
                require_cohort(apic_timer_get_ticks() < reap_deadline,
                               "cohort post-reap quiescence timeout");
                thread_yield();
            }

            heap_stats_t post_heap;
            pmm_stats_t post_pmm;
            heap_get_stats(&post_heap);
            pmm_get_stats(&post_pmm);
            size_t post_tables = vmm_get_allocated_table_frames();

            require_cohort(vmm_get_deferred_count() == 0, "post-cohort deferred remaining");
            require_cohort(sched_get_active_stack_slots_mask() == initial_stack_mask,
                           "post-cohort stack slot leak");
            require_cohort(post_heap.used_bytes == baseline_heap.used_bytes,
                           "post-cohort live heap used mismatch");
            require_cohort(post_heap.allocated_blocks == baseline_heap.allocated_blocks,
                           "post-cohort allocated blocks mismatch");
            require_cohort(heap_verify_integrity() && pmm_audit(),
                           "post-cohort integrity audit");

            size_t heap_delta = post_heap.total_bytes >= baseline_heap.total_bytes
                ? post_heap.total_bytes - baseline_heap.total_bytes : 0;
            size_t table_delta = post_tables >= baseline_tables ? post_tables - baseline_tables : 0;

            if (pass == 0) {
                pass0_committed = post_heap.total_bytes;
                serial_puts("[MEMORY COHORT] cohort size=");
                serial_print_dec(n);
                serial_puts(" pass=0 peak_heap=");
                serial_print_dec(peak_heap.used_bytes);
                serial_puts(" post_heap=");
                serial_print_dec(post_heap.used_bytes);
                serial_puts(" heap_committed=");
                serial_print_dec(post_heap.total_bytes);
                serial_puts(" heap_delta=");
                serial_print_dec(heap_delta);
                serial_puts(" table_delta=");
                serial_print_dec(table_delta);
                serial_puts(" overlap=PASS mixed_reap=PASS\n");
            } else {
                /* Pass 1: Proves heap expansion was one-time peak capacity adjustment, NOT ongoing retention */
                require_cohort(post_heap.total_bytes == pass0_committed,
                               "heap capacity grew further on repeated cohort pass");
                serial_puts("[MEMORY COHORT] cohort size=");
                serial_print_dec(n);
                serial_puts(" pass=1 peak_heap=");
                serial_print_dec(peak_heap.used_bytes);
                serial_puts(" post_heap=");
                serial_print_dec(post_heap.used_bytes);
                serial_puts(" heap_committed=");
                serial_print_dec(post_heap.total_bytes);
                serial_puts(" heap_delta=");
                serial_print_dec(heap_delta);
                serial_puts(" table_delta=");
                serial_print_dec(table_delta);
                serial_puts(" repeat_stable=PASS\n");
            }

            post_cohort_heap = post_heap;
            post_cohort_pmm = post_pmm;
            post_cohort_tables = post_tables;
        }
    }

    /* 4. Controlled Scattered Free Frames & Fragmented PMM Headroom */
    serial_puts("[MEMORY COHORT] testing fragmented PMM headroom...\n");
    uintptr_t head = 0, cursor = PAGE_SIZE;
    size_t held = 0;

    /* Constrain free frames down to 128 using intrusive linked list */
    while (pmm_get_free_pages() > 128) {
        uintptr_t page = pmm_alloc_page_above(cursor);
        if (!page) {
            cursor = PAGE_SIZE;
            page = pmm_alloc_page_above(cursor);
            if (!page) break;
        }
        uint64_t *mapped = vmm_phys_to_virt(page);
        mapped[0] = head;
        mapped[PAGE_SIZE / sizeof(*mapped) - 1] = page ^ 0x5372AC19ULL;
        head = page;
        cursor = page + PAGE_SIZE;
        held++;
    }

    /* Allocate remaining free frames into frag_pages until completely exhausted */
    uintptr_t frag_pages[128];
    size_t frag_count = 0;
    while (frag_count < 128 && pmm_get_free_pages() > 0) {
        uintptr_t p = pmm_alloc_page_above(cursor);
        if (!p) {
            cursor = PAGE_SIZE;
            p = pmm_alloc_page_above(cursor);
            if (!p) break;
        }
        frag_pages[frag_count++] = p;
        cursor = p + PAGE_SIZE;
    }
    require_cohort(frag_count >= 64, "insufficient frames for fragmentation array");

    /* Free alternating even frames: frag_pages[0], frag_pages[2], ...
     * Every freed frame is strictly flanked by held frames:
     * frag_pages[1], frag_pages[3], ... leaving ZERO contiguous runs >= 2. */
    size_t scattered_count = 0;
    for (size_t i = 0; i < frag_count; i += 2) {
        pmm_free_page(frag_pages[i]);
        scattered_count++;
    }

    /* Verify contiguous allocation rejection under 100% fragmented headroom */
    require_cohort(pmm_alloc_pages(2) == 0,
                   "pmm_alloc_pages(2) must fail when all free frames are isolated");
    require_cohort(pmm_alloc_pages(4) == 0,
                   "pmm_alloc_pages(4) must fail when all free frames are isolated");

    /* Verify single page allocation succeeds */
    uintptr_t single = pmm_alloc_page();
    require_cohort(single != 0, "pmm_alloc_page must succeed under scattered frames");
    pmm_free_page(single);

    /* Verify real Ring 3 process spawn succeeds under scattered frames */
    tcb_t *p_frag = process_spawn_on_cpu(0, "cohort_frag",
                                         embedded_init_elf_start, init_elf_size, 9);
    require_cohort(p_frag != NULL,
                   "real process spawn failed under fragmented PMM headroom");
    uint64_t code_frag = 0;
    require_cohort(process_wait(p_frag->tid, &code_frag) && code_frag == 42,
                   "process execution failed under fragmented PMM headroom");
    sched_reap_dead();
    vmm_drain_deferred_destructions();

    /* Step 4B: Tight budget exhaustion (constrain to 2 free pages) */
    uintptr_t tight_head = 0;
    size_t tight_held = 0;
    while (pmm_get_free_pages() > 2) {
        uintptr_t p = pmm_alloc_page();
        if (!p) break;
        uint64_t *m = vmm_phys_to_virt(p);
        m[0] = tight_head;
        m[PAGE_SIZE / sizeof(*m) - 1] = p ^ 0x5372AC19ULL;
        tight_head = p;
        tight_held++;
    }
    require_cohort(pmm_get_free_pages() <= 2, "failed to constrain free frames to 2");

    /* Process spawn must fail cleanly under 2-frame headroom */
    tcb_t *p_oom = process_spawn_on_cpu(0, "cohort_poom",
                                        embedded_init_elf_start, init_elf_size, 9);
    require_cohort(p_oom == NULL,
                   "process spawn should fail cleanly under 2-frame headroom");
    sched_reap_dead();
    vmm_drain_deferred_destructions();

    /* Heap expansion beyond existing capacity must fail cleanly */
    void *fill = kmalloc(post_cohort_heap.largest_free_payload);
    if (fill) {
        void *exp = kmalloc(32 * PAGE_SIZE);
        require_cohort(exp == NULL, "heap expansion should fail under 2-frame headroom");
        kfree(fill);
    }

    /* Release tight held frames */
    while (tight_head) {
        uint64_t *m = vmm_phys_to_virt(tight_head);
        require_cohort(m[PAGE_SIZE / sizeof(*m) - 1] == (tight_head ^ 0x5372AC19ULL),
                       "corrupted tight held frame signature");
        uintptr_t nxt = m[0];
        pmm_free_page(tight_head);
        tight_head = nxt;
    }

    /* Release odd frag_pages */
    for (size_t i = 1; i < frag_count; i += 2) {
        pmm_free_page(frag_pages[i]);
    }

    /* Release head linked-list frames */
    size_t released = 0;
    while (head) {
        uint64_t *mapped = vmm_phys_to_virt(head);
        require_cohort(mapped[PAGE_SIZE / sizeof(*mapped) - 1] == (head ^ 0x5372AC19ULL),
                       "corrupted held frame signature");
        uintptr_t next = mapped[0];
        pmm_free_page(head);
        head = next;
        released++;
    }
    require_cohort(released == held, "held/released frames count mismatch");

    /* Verify contiguous allocation restoration */
    uintptr_t contig4 = pmm_alloc_pages(4);
    require_cohort(contig4 != 0, "contiguous 4-page allocation failed after full release");
    pmm_free_pages(contig4, 4);

    /* Successful post-pressure process creation */
    tcb_t *p_post = process_spawn_on_cpu(0, "cohort_post",
                                         embedded_init_elf_start, init_elf_size, 9);
    require_cohort(p_post != NULL, "post-pressure process spawn failed");
    uint64_t code_post = 0;
    require_cohort(process_wait(p_post->tid, &code_post) && code_post == 42,
                   "post-pressure process execution failed");
    sched_reap_dead();
    vmm_drain_deferred_destructions();

    serial_puts("[MEMORY COHORT] fragmented_pmm held_pages=");
    serial_print_dec(held);
    serial_puts(" scattered_pages=");
    serial_print_dec(scattered_count);
    serial_puts(" contig2_fail=PASS contig4_fail=PASS single_page=PASS proc_spawn=PASS oom_spawn_fail=PASS contig_restore=PASS post_spawn=PASS\n");

    /* 5. Post-Pressure Recovery */
    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            heap_get_used_bytes() == baseline_heap.used_bytes &&
            vmm_get_deferred_count() == 0) break;
        require_cohort(apic_timer_get_ticks() < reap_deadline, "recovery quiescence timeout");
        thread_yield();
    }

    heap_stats_t rec_heap;
    pmm_stats_t rec_pmm;
    heap_get_stats(&rec_heap);
    pmm_get_stats(&rec_pmm);
    size_t rec_tables = vmm_get_allocated_table_frames();

    require_cohort(vmm_get_deferred_count() == 0, "recovery deferred remaining");
    require_cohort(sched_get_active_stack_slots_mask() == initial_stack_mask,
                   "recovery stack slot leak");
    require_cohort(rec_heap.used_bytes == post_cohort_heap.used_bytes,
                   "recovery heap used mismatch");
    require_cohort(rec_heap.allocated_blocks == post_cohort_heap.allocated_blocks,
                   "recovery heap blocks mismatch");
    require_cohort(rec_heap.total_bytes == post_cohort_heap.total_bytes,
                   "recovery heap total mismatch");
    require_cohort(rec_heap.free_bytes == post_cohort_heap.free_bytes,
                   "recovery heap free mismatch");
    require_cohort(rec_tables == post_cohort_tables, "recovery tables mismatch");
    require_cohort(rec_pmm.free_pages == post_cohort_pmm.free_pages,
                   "recovery pmm free pages mismatch");
    require_cohort(heap_verify_integrity() && pmm_audit(), "recovery integrity audit");

    if (post_cohort_pmm.free_pages == baseline_pmm.free_pages && post_cohort_tables == baseline_tables) {
        require_cohort(pmm_snapshot(after, sizeof(after)) &&
                       memcmp(before, after, sizeof(before)) == 0,
                       "recovery exact bitmap mismatch against baseline");
    }

    serial_puts("[MEMORY COHORT] recovery exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS\n");
    serial_puts("[MEMORY COHORT] PASS concurrent process cohorts and fragmented PMM headroom\n");
}

bool memory_rollback_test_enabled(const boot_info_t *boot_info) {
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    static const char token[] = "smp_memory_test=rollback";
    size_t len = 0;
    while (cmd[len] && len < sizeof(boot_info->cmdline)) len++;
    for (size_t i = 0; i + sizeof(token) - 1 <= len; i++) {
        if ((i == 0 || cmd[i - 1] == ' ') &&
            (i + sizeof(token) - 1 == len || cmd[i + sizeof(token) - 1] == ' ') &&
            memcmp(cmd + i, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

static void require_rollback(bool condition, const char *reason) {
    if (condition) return;
    serial_puts("[FAIL] Memory rollback: ");
    serial_puts(reason);
    serial_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void memory_rollback_test_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("Memory Investigation: Process Launch Rollback Verification\n");
    serial_puts("========================================================\n");

    require_rollback(vmm_boot_memory_ready() && pmm_high_memory_enabled(),
                     "memory readiness invariant");

    /* Initial quiescence */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    extern const uint8_t embedded_init_elf_start[];
    extern const uint8_t embedded_init_elf_end[];
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    /* 1. Warmup: 2 process spawn/exit cycles */
    for (size_t w = 0; w < 2; w++) {
        size_t target_cpu = w % total_cpus;
        tcb_t *proc = process_spawn_on_cpu(target_cpu, "rollback_warm",
                                           embedded_init_elf_start, init_elf_size, 9);
        require_rollback(proc != NULL, "warmup process spawn failed");
        uint64_t exit_code = 0;
        require_rollback(process_wait(proc->tid, &exit_code) && exit_code == 42,
                         "warmup process wait failed");
        sched_reap_dead();
        vmm_drain_deferred_destructions();
    }

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require_rollback(apic_timer_get_ticks() < reap_deadline, "warmup quiescence timeout");
        thread_yield();
    }

    /* 2. Warmed Baseline Snapshot */
    heap_stats_t baseline_heap;
    pmm_stats_t baseline_pmm;
    heap_get_stats(&baseline_heap);
    pmm_get_stats(&baseline_pmm);
    size_t baseline_tables = vmm_get_allocated_table_frames();

    require_rollback(vmm_get_deferred_count() == 0, "baseline deferred queue not empty");
    require_rollback(pmm_snapshot(before, sizeof(before)), "baseline pmm snapshot");
    require_rollback(heap_verify_integrity() && pmm_audit(), "baseline integrity audit");

    serial_puts("[MEMORY ROLLBACK] baseline heap_used=");
    serial_print_dec(baseline_heap.used_bytes);
    serial_puts(" heap_committed=");
    serial_print_dec(baseline_heap.total_bytes);
    serial_puts(" heap_free=");
    serial_print_dec(baseline_heap.free_bytes);
    serial_puts(" pmm_free=");
    serial_print_dec(baseline_pmm.free_pages);
    serial_puts(" tables=");
    serial_print_dec(baseline_tables);
    serial_puts(" deferred=0\n");

    /* 3. Deterministic Fault Injection across 16 Cuts */
    typedef struct {
        spawn_fault_type_t fault;
        size_t trigger;
        const char *name;
        bool is_action_test;
    } rollback_cut_test_t;

    static const rollback_cut_test_t cuts[] = {
        { SPAWN_FAULT_VMM_USER_PML4,   0, "VMM_USER_PML4",          false },
        { SPAWN_FAULT_ELF_SEGMENT_PMM, 0, "ELF_SEGMENT_PMM_PAGE0",  false },
        { SPAWN_FAULT_ELF_SEGMENT_PMM, 1, "ELF_SEGMENT_PMM_PAGE1",  false },
        { SPAWN_FAULT_ELF_SEGMENT_MAP, 0, "ELF_SEGMENT_MAP_PAGE0",  false },
        { SPAWN_FAULT_ELF_SEGMENT_MAP, 1, "ELF_SEGMENT_MAP_PAGE1",  false },
        { SPAWN_FAULT_SIGRESTORER_PMM, 0, "SIGRESTORER_PMM",        false },
        { SPAWN_FAULT_SIGRESTORER_MAP, 0, "SIGRESTORER_MAP",        false },
        { SPAWN_FAULT_USER_STACK_PMM,  0, "USER_STACK_PMM",         false },
        { SPAWN_FAULT_USER_STACK_MAP,  0, "USER_STACK_MAP",         false },
        { SPAWN_FAULT_KSTACK_PMM,      2, "KSTACK_PMM_PARTIAL",     false },
        { SPAWN_FAULT_KSTACK_MAP,      0, "KSTACK_MAP_BATCH",       false },
        { SPAWN_FAULT_KSTACK_ALLOC,    0, "KSTACK_SLOT_EXHAUST",    false },
        { SPAWN_FAULT_TCB_KMALLOC,     0, "TCB_KMALLOC",            false },
        { SPAWN_FAULT_FD_INIT,         0, "FD_INIT",                false },
        { SPAWN_FAULT_NONE,            0, "SPAWN_ACTIONS_PARTIAL",  true  },
        { SPAWN_FAULT_SCHED_REF,       0, "SCHED_REF",              false },
    };
    size_t num_cuts = sizeof(cuts) / sizeof(cuts[0]);

    for (size_t cut = 0; cut < num_cuts; cut++) {
        tcb_t *proc = NULL;
        if (cuts[cut].is_action_test) {
            spawn_kaction_t acts[2];
            memset(acts, 0, sizeof(acts));
            acts[0].type = SPAWN_FD_ACTION_OPEN;
            acts[0].dst_fd = 3;
            acts[0].path = "/etc/motd";
            acts[0].flags = 0;
            acts[1].type = SPAWN_FD_ACTION_OPEN;
            acts[1].dst_fd = 4;
            acts[1].path = "/etc/nonexistent_test_marker";
            acts[1].flags = 0;
            proc = process_spawn_with_actions(0, "fault_worker",
                                              embedded_init_elf_start, init_elf_size, 2, acts);
        } else {
            spawn_set_fault_injection(cuts[cut].fault, cuts[cut].trigger);
            proc = process_spawn_on_cpu(0, "fault_worker",
                                        embedded_init_elf_start, init_elf_size, 9);
            require_rollback(spawn_get_fault_hits() == 1,
                             "fault injection point was not hit exactly once");
            spawn_clear_fault_injection();
        }
        require_rollback(proc == NULL, "spawn succeeded despite fault condition");

        uint64_t aborted_pid = spawn_get_last_aborted_pid();
        require_rollback(aborted_pid > 0, "missing aborted pid tracking");
        require_rollback(!process_is_alive(aborted_pid), "aborted process remains alive in table");
        uint64_t dummy_exit = 0;
        require_rollback(!process_wait(aborted_pid, &dummy_exit), "aborted process unexpectedly waitable");

        /* Ensure reap & drain */
        sched_reap_dead();
        vmm_drain_deferred_destructions();

        /* Verify no partially runnable process or leaked state */
        require_rollback(sched_get_active_stack_slots_mask() == initial_stack_mask,
                         "leaked stack slot during rollback");
        require_rollback(vmm_get_deferred_count() == 0,
                         "deferred destruction remaining after rollback");

        heap_stats_t cur_heap;
        pmm_stats_t cur_pmm;
        heap_get_stats(&cur_heap);
        pmm_get_stats(&cur_pmm);
        size_t cur_tables = vmm_get_allocated_table_frames();

        require_rollback(cur_heap.used_bytes == baseline_heap.used_bytes,
                         "leaked heap used bytes during rollback");
        require_rollback(cur_heap.allocated_blocks == baseline_heap.allocated_blocks,
                         "leaked heap allocated blocks during rollback");
        require_rollback(cur_tables == baseline_tables,
                         "leaked page table frames during rollback");
        require_rollback(cur_pmm.free_pages == baseline_pmm.free_pages,
                         "leaked PMM frames during rollback");
        require_rollback(heap_verify_integrity() && pmm_audit(),
                         "integrity audit failed after rollback");

        serial_puts("[MEMORY ROLLBACK] cut ");
        serial_print_dec(cut + 1);
        serial_puts(" (");
        serial_puts(cuts[cut].name);
        serial_puts(") hits=1 pid_aborted=PASS rollback=PASS\n");
    }

    /* 4. Subsequent Normal Process Execution and Full Recovery */
    tcb_t *norm = process_spawn_on_cpu(0, "norm_worker",
                                       embedded_init_elf_start, init_elf_size, 9);
    require_rollback(norm != NULL, "normal process spawn failed after rollback tests");
    uint64_t norm_exit = 0;
    require_rollback(process_wait(norm->tid, &norm_exit) && norm_exit == 42,
                     "normal process execution failed after rollback tests");

    /* Quiescence */
    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            heap_get_used_bytes() == baseline_heap.used_bytes &&
            vmm_get_deferred_count() == 0) break;
        require_rollback(apic_timer_get_ticks() < reap_deadline, "post-rollback quiescence timeout");
        thread_yield();
    }

    heap_stats_t final_heap;
    pmm_stats_t final_pmm;
    heap_get_stats(&final_heap);
    pmm_get_stats(&final_pmm);
    size_t final_tables = vmm_get_allocated_table_frames();

    require_rollback(final_heap.used_bytes == baseline_heap.used_bytes,
                     "final heap used bytes mismatch");
    require_rollback(final_heap.allocated_blocks == baseline_heap.allocated_blocks,
                     "final heap blocks mismatch");
    require_rollback(final_tables == baseline_tables,
                     "final table frames mismatch");
    require_rollback(final_pmm.free_pages == baseline_pmm.free_pages,
                     "final pmm free pages mismatch");
    require_rollback(pmm_snapshot(after, sizeof(after)) &&
                     memcmp(before, after, sizeof(before)) == 0,
                     "final PMM bitmap mismatch against baseline");
    require_rollback(heap_verify_integrity() && pmm_audit(),
                     "final integrity audit failed");

    serial_puts("[MEMORY ROLLBACK] recovery exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS exact_bitmap=PASS\n");
    serial_puts("[MEMORY ROLLBACK] PASS process launch allocation failure rollback\n");
}

/* =========================================================================
 * SMP Memory Investigation: Kernel-Stack Allocation Telemetry & Profiling
 * ========================================================================= */

typedef struct {
    uint64_t ops;
    uint64_t total_alloc_tsc;
    uint64_t total_free_tsc;
    uint64_t slot_alloc_wait_tsc;
    uint64_t slot_alloc_hold_tsc;
    uint64_t alloc_prep_tsc;
    uint64_t pmm_alloc_tsc;
    uint64_t map_prep_tsc;
    uint64_t map_vmm_lock_wait_tsc;
    uint64_t map_vmm_lock_hold_tsc;
    uint64_t map_vmm_pt_work_tsc;
    uint64_t map_vmm_pre_lock_tsc;
    uint64_t map_vmm_post_lock_prep_tsc;
    uint64_t map_vmm_put_op_wait_tsc;
    uint64_t map_vmm_put_op_hold_tsc;
    uint64_t map_tlb_dispatch_tsc;
    uint64_t map_tlb_ack_poll_tsc;
    uint64_t map_tlb_service_tsc;
    uint64_t alloc_tail_tsc;
    uint64_t unmap_vmm_lock_wait_tsc;
    uint64_t unmap_vmm_lock_hold_tsc;
    uint64_t unmap_vmm_pt_work_tsc;
    uint64_t unmap_vmm_pre_lock_tsc;
    uint64_t unmap_vmm_post_lock_prep_tsc;
    uint64_t unmap_vmm_put_op_wait_tsc;
    uint64_t unmap_vmm_put_op_hold_tsc;
    uint64_t unmap_tlb_dispatch_tsc;
    uint64_t unmap_tlb_ack_poll_tsc;
    uint64_t unmap_tlb_service_tsc;
    uint64_t free_mid_tsc;
    uint64_t pmm_free_tsc;
    uint64_t free_tail_tsc;
    uint64_t slot_free_wait_tsc;
    uint64_t slot_free_hold_tsc;
    uint64_t alloc_inner_tsc;
    uint64_t free_inner_tsc;
    uint64_t remote_ipi_service_tsc;
    uint64_t remote_ipi_service_count;
    uint64_t cpu_id;
} kstack_cpu_telemetry_t;

static kstack_cpu_telemetry_t g_kstack_cpu_telemetry[MAX_DETECTED_CPUS];
static volatile uint32_t g_kprof_ready = 0;
static volatile uint32_t g_kprof_start = 0;
static volatile uint32_t g_kprof_done = 0;
static volatile uint32_t g_kprof_workers_active = 0;
static volatile uint32_t g_kprof_workers_terminated = 0;

#define KPROF_WORKERS_PER_CPU 2
#define KPROF_ITERATIONS 50

static inline uint64_t read_tsc_ordered(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void memory_kstack_profile_worker(void *arg) {
    size_t assigned_cpu = (size_t)(uintptr_t)arg;
    size_t actual_cpu = cpu_current()->id;
    require(actual_cpu == assigned_cpu, "worker cpu stability invariant");

    /* Signal arrival at start gate */
    __atomic_fetch_add(&g_kprof_ready, 1, __ATOMIC_RELEASE);

    /* Wait for start gate */
    while (__atomic_load_n(&g_kprof_start, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    kstack_cpu_telemetry_t *t = &g_kstack_cpu_telemetry[actual_cpu];
    t->cpu_id = actual_cpu;

    for (int iter = 0; iter < KPROF_ITERATIONS; iter++) {
        uintptr_t guard = 0, base = 0;
        size_t size = 0;
        kstack_subinterval_t alloc_sub = {0};
        kstack_subinterval_t free_sub = {0};

        uint64_t t_alloc_start = read_tsc_ordered();
        int slot = kstack_alloc_tracked(&guard, &base, &size, &alloc_sub);
        uint64_t t_alloc_end = read_tsc_ordered();

        require(slot >= 0 && slot < 64, "kstack_alloc failed during profiling");
        require(guard == KERNEL_STACKS_BASE + (uintptr_t)slot * STACK_SLOT_SIZE, "guard address");
        require(base == guard + STACK_GUARD_SIZE, "base address");
        require(size == STACK_USABLE_SIZE, "stack usable size");

        /* Touch top and bottom usable stack pages */
        volatile uint64_t *stack_top = (volatile uint64_t *)(base + size - 16);
        volatile uint64_t *stack_bot = (volatile uint64_t *)base;
        *stack_top = 0xAA55AA5500000000ULL | (uint64_t)slot;
        *stack_bot = 0x55AA55AA00000000ULL | (uint64_t)slot;
        require(*stack_top == (0xAA55AA5500000000ULL | (uint64_t)slot), "stack write verify top");
        require(*stack_bot == (0x55AA55AA00000000ULL | (uint64_t)slot), "stack write verify bot");

        uint64_t t_free_start = read_tsc_ordered();
        kstack_free_tracked(slot, base, &free_sub);
        uint64_t t_free_end = read_tsc_ordered();

        t->ops++;
        if (t_alloc_end >= t_alloc_start) {
            t->total_alloc_tsc += (t_alloc_end - t_alloc_start);
        }
        if (t_free_end >= t_free_start) {
            t->total_free_tsc += (t_free_end - t_free_start);
        }
        t->slot_alloc_wait_tsc += alloc_sub.slot_wait_tsc;
        t->slot_alloc_hold_tsc += alloc_sub.slot_hold_tsc;
        t->alloc_prep_tsc      += alloc_sub.alloc_prep_tsc;
        t->pmm_alloc_tsc       += alloc_sub.pmm_tsc;
        t->map_prep_tsc        += alloc_sub.map_prep_tsc;
        t->map_vmm_lock_wait_tsc   += alloc_sub.vmm_lock_wait_tsc;
        t->map_vmm_lock_hold_tsc   += alloc_sub.vmm_lock_hold_tsc;
        t->map_vmm_pt_work_tsc     += alloc_sub.vmm_pt_work_tsc;
        t->map_vmm_pre_lock_tsc    += alloc_sub.vmm_pre_lock_tsc;
        t->map_vmm_post_lock_prep_tsc += alloc_sub.vmm_post_lock_prep_tsc;
        t->map_vmm_put_op_wait_tsc += alloc_sub.vmm_put_op_wait_tsc;
        t->map_vmm_put_op_hold_tsc += alloc_sub.vmm_put_op_hold_tsc;
        t->map_tlb_dispatch_tsc    += alloc_sub.vmm_tlb_dispatch_tsc;
        t->map_tlb_ack_poll_tsc    += alloc_sub.vmm_tlb_ack_poll_tsc;
        t->map_tlb_service_tsc     += alloc_sub.vmm_tlb_service_tsc;
        t->alloc_tail_tsc          += alloc_sub.alloc_tail_tsc;

        t->unmap_vmm_lock_wait_tsc += free_sub.vmm_lock_wait_tsc;
        t->unmap_vmm_lock_hold_tsc += free_sub.vmm_lock_hold_tsc;
        t->unmap_vmm_pt_work_tsc   += free_sub.vmm_pt_work_tsc;
        t->unmap_vmm_pre_lock_tsc  += free_sub.vmm_pre_lock_tsc;
        t->unmap_vmm_post_lock_prep_tsc += free_sub.vmm_post_lock_prep_tsc;
        t->unmap_vmm_put_op_wait_tsc += free_sub.vmm_put_op_wait_tsc;
        t->unmap_vmm_put_op_hold_tsc += free_sub.vmm_put_op_hold_tsc;
        t->unmap_tlb_dispatch_tsc  += free_sub.vmm_tlb_dispatch_tsc;
        t->unmap_tlb_ack_poll_tsc  += free_sub.vmm_tlb_ack_poll_tsc;
        t->unmap_tlb_service_tsc   += free_sub.vmm_tlb_service_tsc;
        t->free_mid_tsc        += free_sub.free_mid_tsc;
        t->pmm_free_tsc        += free_sub.pmm_tsc;
        t->free_tail_tsc       += free_sub.free_tail_tsc;
        t->slot_free_wait_tsc  += free_sub.slot_wait_tsc;
        t->slot_free_hold_tsc  += free_sub.slot_hold_tsc;
        t->alloc_inner_tsc     += alloc_sub.inner_tsc;
        t->free_inner_tsc      += free_sub.inner_tsc;
    }

    __atomic_fetch_sub(&g_kprof_workers_active, 1, __ATOMIC_RELEASE);

    /* Park at done gate */
    while (__atomic_load_n(&g_kprof_done, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    __atomic_fetch_add(&g_kprof_workers_terminated, 1, __ATOMIC_RELEASE);
    thread_exit();
}

bool memory_kstack_profile_enabled(const boot_info_t *boot_info) {
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    static const char token[] = "smp_memory_test=kstack_profile";
    size_t len = 0;
    while (cmd[len] && len < sizeof(boot_info->cmdline)) len++;
    for (size_t i = 0; i + sizeof(token) - 1 <= len; i++) {
        if ((i == 0 || cmd[i - 1] == ' ') &&
            (i + sizeof(token) - 1 == len || cmd[i + sizeof(token) - 1] == ' ') &&
            memcmp(cmd + i, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

void memory_kstack_profile_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("Memory Investigation: Kernel Stack Telemetry Profiling\n");
    serial_puts("========================================================\n");
    serial_puts("[KPROF] CPUs online: ");
    serial_print_dec(total_cpus);
    serial_puts("\n");

    require(vmm_boot_memory_ready() && pmm_high_memory_enabled(), "memory readiness invariant");

    /* Initial quiescence */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require(apic_timer_get_ticks() < reap_deadline, "initial quiescence timeout");
        thread_yield();
    }

    /* Baseline resource snapshots */
    heap_stats_t baseline_heap;
    pmm_stats_t baseline_pmm;
    heap_get_stats(&baseline_heap);
    pmm_get_stats(&baseline_pmm);
    size_t baseline_tables = vmm_get_allocated_table_frames();
    require(pmm_snapshot(before, sizeof(before)), "baseline pmm snapshot");
    require(heap_verify_integrity() && pmm_audit(), "baseline integrity audit");

    memset(g_kstack_cpu_telemetry, 0, sizeof(g_kstack_cpu_telemetry));
    g_kprof_ready = 0;
    g_kprof_start = 0;
    g_kprof_done = 0;
    size_t worker_count = total_cpus * KPROF_WORKERS_PER_CPU;
    g_kprof_workers_active = worker_count;
    g_kprof_workers_terminated = 0;

    serial_puts("[KPROF] Spawning ");
    serial_print_dec(worker_count);
    serial_puts(" pinned workers (");
    serial_print_dec(KPROF_WORKERS_PER_CPU);
    serial_puts(" per CPU, ");
    serial_print_dec(KPROF_ITERATIONS);
    serial_puts(" iterations each)...\n");

    for (size_t i = 0; i < worker_count; i++) {
        size_t target_cpu = i % total_cpus;
        tcb_t *w = thread_create_on_cpu(target_cpu, "kprof_w", memory_kstack_profile_worker, (void *)(uintptr_t)target_cpu);
        require(w != NULL, "failed to spawn profiling worker");
    }

    /* Wait for all workers to arrive at the start gate before taking the baseline snapshot */
    uint64_t ready_timeout = 50000000ULL;
    while (__atomic_load_n(&g_kprof_ready, __ATOMIC_ACQUIRE) < worker_count) {
        __asm__ volatile("pause");
        thread_yield();
        if (--ready_timeout == 0) {
            serial_puts("[FAIL] KPROF: Timed out waiting for workers to reach start gate!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    smp_tlb_stats_t tlb_before, tlb_after;
    smp_tlb_get_stats(&tlb_before);

    uint64_t pmm_acq_before = 0, pmm_cont_before = 0;
    uint64_t pmm_acq_after = 0, pmm_cont_after = 0;
    pmm_get_lock_stats(&pmm_acq_before, &pmm_cont_before);

    uint64_t ipi_cnt_before[MAX_DETECTED_CPUS];
    uint64_t ipi_tsc_before[MAX_DETECTED_CPUS];
    for (size_t c = 0; c < total_cpus; c++) {
        ipi_cnt_before[c] = g_ipi_tlb_count[c];
        ipi_tsc_before[c] = g_ipi_tlb_service_tsc[c];
    }

    /* Release start gate */
    uint64_t overall_start_tsc = read_tsc_ordered();
    __atomic_store_n(&g_kprof_start, 1, __ATOMIC_RELEASE);

    /* Wait for workers to finish iterations */
    uint64_t wait_timeout = 200000000ULL;
    while (__atomic_load_n(&g_kprof_workers_active, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
        thread_yield();
        if (--wait_timeout == 0) {
            serial_puts("[FAIL] KPROF: Timed out waiting for workers to complete iterations!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }
    uint64_t overall_end_tsc = read_tsc_ordered();

    smp_tlb_get_stats(&tlb_after);
    pmm_get_lock_stats(&pmm_acq_after, &pmm_cont_after);

    /* Release done gate to let workers terminate */
    __atomic_store_n(&g_kprof_done, 1, __ATOMIC_RELEASE);

    /* Wait for workers to terminate and reap */
    uint64_t term_timeout = 50000000ULL;
    while (__atomic_load_n(&g_kprof_workers_terminated, __ATOMIC_ACQUIRE) < worker_count) {
        __asm__ volatile("pause");
        thread_yield();
        if (--term_timeout == 0) {
            serial_puts("[FAIL] KPROF: Timed out waiting for workers to terminate!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require(apic_timer_get_ticks() < reap_deadline, "final reaping timeout");
        thread_yield();
    }

    /* Verification of exact post-profile resource restoration */
    heap_stats_t final_heap;
    pmm_stats_t final_pmm;
    heap_get_stats(&final_heap);
    pmm_get_stats(&final_pmm);
    size_t final_tables = vmm_get_allocated_table_frames();

    require(final_heap.used_bytes == baseline_heap.used_bytes, "final heap used mismatch");
    require(final_heap.allocated_blocks == baseline_heap.allocated_blocks, "final heap blocks mismatch");
    require(final_tables == baseline_tables, "final table frames mismatch");
    require(final_pmm.free_pages == baseline_pmm.free_pages, "final pmm free mismatch");
    require(pmm_snapshot(after, sizeof(after)) &&
            memcmp(before, after, sizeof(before)) == 0,
            "final pmm bitmap mismatch");
    require(heap_verify_integrity() && pmm_audit(), "final integrity audit");

    /* Report per-CPU telemetry */
    uint64_t total_ops = 0;
    uint64_t total_alloc_tsc = 0;
    uint64_t total_free_tsc = 0;
    uint64_t sum_slot_alloc_wait = 0;
    uint64_t sum_slot_alloc_hold = 0;
    uint64_t sum_alloc_prep = 0;
    uint64_t sum_pmm_alloc = 0;
    uint64_t sum_map_prep = 0;
    uint64_t sum_vmm_map_wait = 0;
    uint64_t sum_vmm_map_hold = 0;
    uint64_t sum_vmm_map_pt = 0;
    uint64_t sum_vmm_map_pre_lock = 0;
    uint64_t sum_vmm_map_post_prep = 0;
    uint64_t sum_vmm_map_put_op_wait = 0;
    uint64_t sum_vmm_map_put_op_hold = 0;
    uint64_t sum_map_dispatch = 0;
    uint64_t sum_map_ack_poll = 0;
    uint64_t sum_map_service = 0;
    uint64_t sum_alloc_tail = 0;
    uint64_t sum_unmap_wait = 0;
    uint64_t sum_unmap_hold = 0;
    uint64_t sum_unmap_pt = 0;
    uint64_t sum_unmap_pre_lock = 0;
    uint64_t sum_unmap_post_prep = 0;
    uint64_t sum_unmap_put_op_wait = 0;
    uint64_t sum_unmap_put_op_hold = 0;
    uint64_t sum_unmap_dispatch = 0;
    uint64_t sum_unmap_ack_poll = 0;
    uint64_t sum_unmap_service = 0;
    uint64_t sum_free_mid = 0;
    uint64_t sum_pmm_free = 0;
    uint64_t sum_free_tail = 0;
    uint64_t sum_slot_free_wait = 0;
    uint64_t sum_slot_free_hold = 0;
    uint64_t sum_remote_ipi_tsc = 0;
    uint64_t sum_remote_ipi_count = 0;

    for (size_t c = 0; c < total_cpus; c++) {
        kstack_cpu_telemetry_t *t = &g_kstack_cpu_telemetry[c];
        uint64_t ipi_cnt_after = g_ipi_tlb_count[c];
        uint64_t ipi_tsc_after = g_ipi_tlb_service_tsc[c];
        t->remote_ipi_service_count = (ipi_cnt_after >= ipi_cnt_before[c]) ? (ipi_cnt_after - ipi_cnt_before[c]) : 0;
        t->remote_ipi_service_tsc = (ipi_tsc_after >= ipi_tsc_before[c]) ? (ipi_tsc_after - ipi_tsc_before[c]) : 0;

        total_ops += t->ops;
        total_alloc_tsc += t->total_alloc_tsc;
        total_free_tsc += t->total_free_tsc;
        sum_slot_alloc_wait += t->slot_alloc_wait_tsc;
        sum_slot_alloc_hold += t->slot_alloc_hold_tsc;
        sum_alloc_prep      += t->alloc_prep_tsc;
        sum_pmm_alloc       += t->pmm_alloc_tsc;
        sum_map_prep        += t->map_prep_tsc;
        sum_vmm_map_wait    += t->map_vmm_lock_wait_tsc;
        sum_vmm_map_hold    += t->map_vmm_lock_hold_tsc;
        sum_vmm_map_pt      += t->map_vmm_pt_work_tsc;
        sum_vmm_map_pre_lock += t->map_vmm_pre_lock_tsc;
        sum_vmm_map_post_prep += t->map_vmm_post_lock_prep_tsc;
        sum_vmm_map_put_op_wait += t->map_vmm_put_op_wait_tsc;
        sum_vmm_map_put_op_hold += t->map_vmm_put_op_hold_tsc;
        sum_map_dispatch    += t->map_tlb_dispatch_tsc;
        sum_map_ack_poll    += t->map_tlb_ack_poll_tsc;
        sum_map_service     += t->map_tlb_service_tsc;
        sum_alloc_tail      += t->alloc_tail_tsc;
        sum_unmap_wait      += t->unmap_vmm_lock_wait_tsc;
        sum_unmap_hold      += t->unmap_vmm_lock_hold_tsc;
        sum_unmap_pt        += t->unmap_vmm_pt_work_tsc;
        sum_unmap_pre_lock  += t->unmap_vmm_pre_lock_tsc;
        sum_unmap_post_prep += t->unmap_vmm_post_lock_prep_tsc;
        sum_unmap_put_op_wait += t->unmap_vmm_put_op_wait_tsc;
        sum_unmap_put_op_hold += t->unmap_vmm_put_op_hold_tsc;
        sum_unmap_dispatch  += t->unmap_tlb_dispatch_tsc;
        sum_unmap_ack_poll  += t->unmap_tlb_ack_poll_tsc;
        sum_unmap_service   += t->unmap_tlb_service_tsc;
        sum_free_mid        += t->free_mid_tsc;
        sum_pmm_free        += t->pmm_free_tsc;
        sum_free_tail       += t->free_tail_tsc;
        sum_slot_free_wait  += t->slot_free_wait_tsc;
        sum_slot_free_hold  += t->slot_free_hold_tsc;
        sum_remote_ipi_tsc  += t->remote_ipi_service_tsc;
        sum_remote_ipi_count += t->remote_ipi_service_count;

        serial_puts("[KPROF_CPU] cpu=");
        serial_print_dec(c);
        serial_puts(" ops=");
        serial_print_dec(t->ops);
        serial_puts(" alloc_tsc_ticks=");
        serial_print_dec(t->total_alloc_tsc);
        serial_puts(" alloc_inner_tsc=");
        serial_print_dec(t->alloc_inner_tsc);
        serial_puts(" free_tsc_ticks=");
        serial_print_dec(t->total_free_tsc);
        serial_puts(" free_inner_tsc=");
        serial_print_dec(t->free_inner_tsc);
        if (t->ops > 0) {
            serial_puts(" avg_alloc_ticks=");
            serial_print_dec(t->total_alloc_tsc / t->ops);
            serial_puts(" avg_free_ticks=");
            serial_print_dec(t->total_free_tsc / t->ops);
        }
        serial_puts("\n");

        serial_puts("[KPROF_SUBINTERVAL] cpu=");
        serial_print_dec(c);
        serial_puts(" slot_alloc_wait=");
        serial_print_dec(t->slot_alloc_wait_tsc);
        serial_puts(" slot_alloc_hold=");
        serial_print_dec(t->slot_alloc_hold_tsc);
        serial_puts(" alloc_prep=");
        serial_print_dec(t->alloc_prep_tsc);
        serial_puts(" pmm_alloc=");
        serial_print_dec(t->pmm_alloc_tsc);
        serial_puts(" map_prep=");
        serial_print_dec(t->map_prep_tsc);
        serial_puts(" vmm_map_wait=");
        serial_print_dec(t->map_vmm_lock_wait_tsc);
        serial_puts(" vmm_map_hold=");
        serial_print_dec(t->map_vmm_lock_hold_tsc);
        serial_puts(" vmm_map_pt=");
        serial_print_dec(t->map_vmm_pt_work_tsc);
        serial_puts(" vmm_map_pre_lock=");
        serial_print_dec(t->map_vmm_pre_lock_tsc);
        serial_puts(" vmm_map_post_prep=");
        serial_print_dec(t->map_vmm_post_lock_prep_tsc);
        serial_puts(" vmm_map_put_op_wait=");
        serial_print_dec(t->map_vmm_put_op_wait_tsc);
        serial_puts(" vmm_map_put_op_hold=");
        serial_print_dec(t->map_vmm_put_op_hold_tsc);
        serial_puts(" map_dispatch=");
        serial_print_dec(t->map_tlb_dispatch_tsc);
        serial_puts(" map_ack_poll=");
        serial_print_dec(t->map_tlb_ack_poll_tsc);
        serial_puts(" map_service=");
        serial_print_dec(t->map_tlb_service_tsc);
        serial_puts(" alloc_tail=");
        serial_print_dec(t->alloc_tail_tsc);
        serial_puts(" unmap_wait=");
        serial_print_dec(t->unmap_vmm_lock_wait_tsc);
        serial_puts(" unmap_hold=");
        serial_print_dec(t->unmap_vmm_lock_hold_tsc);
        serial_puts(" unmap_pt=");
        serial_print_dec(t->unmap_vmm_pt_work_tsc);
        serial_puts(" unmap_pre_lock=");
        serial_print_dec(t->unmap_vmm_pre_lock_tsc);
        serial_puts(" unmap_post_prep=");
        serial_print_dec(t->unmap_vmm_post_lock_prep_tsc);
        serial_puts(" unmap_put_op_wait=");
        serial_print_dec(t->unmap_vmm_put_op_wait_tsc);
        serial_puts(" unmap_put_op_hold=");
        serial_print_dec(t->unmap_vmm_put_op_hold_tsc);
        serial_puts(" unmap_dispatch=");
        serial_print_dec(t->unmap_tlb_dispatch_tsc);
        serial_puts(" unmap_ack_poll=");
        serial_print_dec(t->unmap_tlb_ack_poll_tsc);
        serial_puts(" unmap_service=");
        serial_print_dec(t->unmap_tlb_service_tsc);
        serial_puts(" free_mid=");
        serial_print_dec(t->free_mid_tsc);
        serial_puts(" pmm_free=");
        serial_print_dec(t->pmm_free_tsc);
        serial_puts(" free_tail=");
        serial_print_dec(t->free_tail_tsc);
        serial_puts(" slot_free_wait=");
        serial_print_dec(t->slot_free_wait_tsc);
        serial_puts(" slot_free_hold=");
        serial_print_dec(t->slot_free_hold_tsc);
        serial_puts("\n");

        serial_puts("[KPROF_IPI_OVERLAY] cpu=");
        serial_print_dec(c);
        serial_puts(" remote_ipi_count=");
        serial_print_dec(t->remote_ipi_service_count);
        serial_puts(" remote_ipi_tsc=");
        serial_print_dec(t->remote_ipi_service_tsc);
        serial_puts("\n");
    }

    uint64_t overall_elapsed_tsc = overall_end_tsc >= overall_start_tsc ? (overall_end_tsc - overall_start_tsc) : 0;
    uint64_t delta_pmm_acq = pmm_acq_after >= pmm_acq_before ? (pmm_acq_after - pmm_acq_before) : 0;
    uint64_t delta_pmm_cont = pmm_cont_after >= pmm_cont_before ? (pmm_cont_after - pmm_cont_before) : 0;
    uint64_t delta_tlb_calls = tlb_after.calls >= tlb_before.calls ? (tlb_after.calls - tlb_before.calls) : 0;
    uint64_t delta_tlb_wait_cycles = tlb_after.wait_cycles >= tlb_before.wait_cycles ? (tlb_after.wait_cycles - tlb_before.wait_cycles) : 0;

    serial_puts("[KPROF_SUMMARY] cpus=");
    serial_print_dec(total_cpus);
    serial_puts(" total_ops=");
    serial_print_dec(total_ops);
    serial_puts(" overall_elapsed_tsc=");
    serial_print_dec(overall_elapsed_tsc);
    serial_puts(" pmm_acq=");
    serial_print_dec(delta_pmm_acq);
    serial_puts(" pmm_cont=");
    serial_print_dec(delta_pmm_cont);
    serial_puts(" tlb_calls=");
    serial_print_dec(delta_tlb_calls);
    serial_puts(" tlb_wait_cycles=");
    serial_print_dec(delta_tlb_wait_cycles);
    serial_puts("\n");

    serial_puts("[KPROF_SUBINTERVAL_SUM] slot_alloc_wait=");
    serial_print_dec(sum_slot_alloc_wait);
    serial_puts(" slot_alloc_hold=");
    serial_print_dec(sum_slot_alloc_hold);
    serial_puts(" alloc_prep=");
    serial_print_dec(sum_alloc_prep);
    serial_puts(" pmm_alloc=");
    serial_print_dec(sum_pmm_alloc);
    serial_puts(" map_prep=");
    serial_print_dec(sum_map_prep);
    serial_puts(" vmm_map_wait=");
    serial_print_dec(sum_vmm_map_wait);
    serial_puts(" vmm_map_hold=");
    serial_print_dec(sum_vmm_map_hold);
    serial_puts(" vmm_map_pt=");
    serial_print_dec(sum_vmm_map_pt);
    serial_puts(" vmm_map_pre_lock=");
    serial_print_dec(sum_vmm_map_pre_lock);
    serial_puts(" vmm_map_post_prep=");
    serial_print_dec(sum_vmm_map_post_prep);
    serial_puts(" vmm_map_put_op_wait=");
    serial_print_dec(sum_vmm_map_put_op_wait);
    serial_puts(" vmm_map_put_op_hold=");
    serial_print_dec(sum_vmm_map_put_op_hold);
    serial_puts(" map_dispatch=");
    serial_print_dec(sum_map_dispatch);
    serial_puts(" map_ack_poll=");
    serial_print_dec(sum_map_ack_poll);
    serial_puts(" map_service=");
    serial_print_dec(sum_map_service);
    serial_puts(" alloc_tail=");
    serial_print_dec(sum_alloc_tail);
    serial_puts(" unmap_wait=");
    serial_print_dec(sum_unmap_wait);
    serial_puts(" unmap_hold=");
    serial_print_dec(sum_unmap_hold);
    serial_puts(" unmap_pt=");
    serial_print_dec(sum_unmap_pt);
    serial_puts(" unmap_pre_lock=");
    serial_print_dec(sum_unmap_pre_lock);
    serial_puts(" unmap_post_prep=");
    serial_print_dec(sum_unmap_post_prep);
    serial_puts(" unmap_put_op_wait=");
    serial_print_dec(sum_unmap_put_op_wait);
    serial_puts(" unmap_put_op_hold=");
    serial_print_dec(sum_unmap_put_op_hold);
    serial_puts(" unmap_dispatch=");
    serial_print_dec(sum_unmap_dispatch);
    serial_puts(" unmap_ack_poll=");
    serial_print_dec(sum_unmap_ack_poll);
    serial_puts(" unmap_service=");
    serial_print_dec(sum_unmap_service);
    serial_puts(" free_mid=");
    serial_print_dec(sum_free_mid);
    serial_puts(" pmm_free=");
    serial_print_dec(sum_pmm_free);
    serial_puts(" free_tail=");
    serial_print_dec(sum_free_tail);
    serial_puts(" slot_free_wait=");
    serial_print_dec(sum_slot_free_wait);
    serial_puts(" slot_free_hold=");
    serial_print_dec(sum_slot_free_hold);
    serial_puts(" remote_ipi_count=");
    serial_print_dec(sum_remote_ipi_count);
    serial_puts(" remote_ipi_tsc=");
    serial_print_dec(sum_remote_ipi_tsc);
    serial_puts("\n");

    serial_puts("[KPROF] exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS exact_bitmap=PASS\n");
    serial_puts("[KPROF] PASS kernel stack telemetry profiling complete\n");
}

bool memory_kstack_control_enabled(const boot_info_t *boot_info) {
    if (!boot_info) return false;
    const char *cmd = boot_info->cmdline;
    static const char token[] = "smp_memory_test=kstack_control";
    size_t len = 0;
    while (cmd[len] && len < sizeof(boot_info->cmdline)) len++;
    for (size_t i = 0; i + sizeof(token) - 1 <= len; i++) {
        if ((i == 0 || cmd[i - 1] == ' ') &&
            (i + sizeof(token) - 1 == len || cmd[i + sizeof(token) - 1] == ' ') &&
            memcmp(cmd + i, token, sizeof(token) - 1) == 0) return true;
    }
    return false;
}

static volatile uint32_t g_kctrl_ready = 0;
static volatile uint32_t g_kctrl_start = 0;
static volatile uint32_t g_kctrl_done = 0;
static volatile uint32_t g_kctrl_workers_active = 0;
static volatile uint32_t g_kctrl_workers_terminated = 0;

static void memory_kstack_control_worker(void *arg) {
    size_t assigned_cpu = (size_t)(uintptr_t)arg;
    size_t actual_cpu = cpu_current()->id;
    require(actual_cpu == assigned_cpu, "control worker cpu stability invariant");

    __atomic_fetch_add(&g_kctrl_ready, 1, __ATOMIC_RELEASE);

    while (__atomic_load_n(&g_kctrl_start, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    for (int iter = 0; iter < KPROF_ITERATIONS; iter++) {
        uintptr_t guard = 0, base = 0;
        size_t size = 0;
        /* Call UNTRACKED kstack_alloc (metrics = NULL) */
        int slot = kstack_alloc_tracked(&guard, &base, &size, NULL);
        require(slot >= 0 && slot < 64, "kstack_alloc failed during control");

        /* Touch top and bottom usable stack pages identically */
        volatile uint64_t *stack_top = (volatile uint64_t *)(base + size - 16);
        volatile uint64_t *stack_bot = (volatile uint64_t *)base;
        *stack_top = 0xAA55AA5500000000ULL | (uint64_t)slot;
        *stack_bot = 0x55AA55AA00000000ULL | (uint64_t)slot;
        require(*stack_top == (0xAA55AA5500000000ULL | (uint64_t)slot), "stack write verify top");
        require(*stack_bot == (0x55AA55AA00000000ULL | (uint64_t)slot), "stack write verify bot");

        /* Call UNTRACKED kstack_free (metrics = NULL) */
        kstack_free_tracked(slot, base, NULL);
    }

    __atomic_fetch_sub(&g_kctrl_workers_active, 1, __ATOMIC_RELEASE);

    while (__atomic_load_n(&g_kctrl_done, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
        thread_yield();
    }

    __atomic_fetch_add(&g_kctrl_workers_terminated, 1, __ATOMIC_RELEASE);
    thread_exit();
}

void memory_kstack_control_run(size_t total_cpus) {
    if (total_cpus == 0) total_cpus = 1;

    serial_puts("\n========================================================\n");
    serial_puts("Memory Investigation: Kernel Stack Untracked Gated Control\n");
    serial_puts("========================================================\n");
    serial_puts("[KPROF_CONTROL] CPUs online: ");
    serial_print_dec(total_cpus);
    serial_puts("\n");

    require(vmm_boot_memory_ready() && pmm_high_memory_enabled(), "memory readiness invariant");

    /* Initial quiescence */
    for (int d = 0; d < 5; d++) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        thread_yield();
    }

    uint64_t initial_stack_mask = sched_get_active_stack_slots_mask();
    uint64_t reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require(apic_timer_get_ticks() < reap_deadline, "initial quiescence timeout");
        thread_yield();
    }

    heap_stats_t baseline_heap;
    pmm_stats_t baseline_pmm;
    heap_get_stats(&baseline_heap);
    pmm_get_stats(&baseline_pmm);
    size_t baseline_tables = vmm_get_allocated_table_frames();
    require(pmm_snapshot(before, sizeof(before)), "baseline pmm snapshot");
    require(heap_verify_integrity() && pmm_audit(), "baseline integrity audit");

    g_kctrl_ready = 0;
    g_kctrl_start = 0;
    g_kctrl_done = 0;
    size_t worker_count = total_cpus * KPROF_WORKERS_PER_CPU;
    g_kctrl_workers_active = worker_count;
    g_kctrl_workers_terminated = 0;

    serial_puts("[KPROF_CONTROL] Spawning ");
    serial_print_dec(worker_count);
    serial_puts(" pinned control workers (");
    serial_print_dec(KPROF_WORKERS_PER_CPU);
    serial_puts(" per CPU, ");
    serial_print_dec(KPROF_ITERATIONS);
    serial_puts(" iterations each)...\n");

    for (size_t i = 0; i < worker_count; i++) {
        size_t target_cpu = i % total_cpus;
        tcb_t *w = thread_create_on_cpu(target_cpu, "kctrl_w", memory_kstack_control_worker, (void *)(uintptr_t)target_cpu);
        require(w != NULL, "failed to spawn control worker");
    }

    uint64_t ready_timeout = 50000000ULL;
    while (__atomic_load_n(&g_kctrl_ready, __ATOMIC_ACQUIRE) < worker_count) {
        __asm__ volatile("pause");
        thread_yield();
        if (--ready_timeout == 0) {
            serial_puts("[FAIL] KPROF_CONTROL: Timed out waiting for workers to reach start gate!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    smp_tlb_stats_t tlb_before, tlb_after;
    smp_tlb_get_stats(&tlb_before);

    uint64_t pmm_acq_before = 0, pmm_cont_before = 0;
    uint64_t pmm_acq_after = 0, pmm_cont_after = 0;
    pmm_get_lock_stats(&pmm_acq_before, &pmm_cont_before);

    /* Release start gate and record overall gated interval */
    uint64_t overall_start_tsc = read_tsc_ordered();
    __atomic_store_n(&g_kctrl_start, 1, __ATOMIC_RELEASE);

    uint64_t wait_timeout = 200000000ULL;
    while (__atomic_load_n(&g_kctrl_workers_active, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
        thread_yield();
        if (--wait_timeout == 0) {
            serial_puts("[FAIL] KPROF_CONTROL: Timed out waiting for workers to complete iterations!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }
    uint64_t overall_end_tsc = read_tsc_ordered();

    smp_tlb_get_stats(&tlb_after);
    pmm_get_lock_stats(&pmm_acq_after, &pmm_cont_after);

    __atomic_store_n(&g_kctrl_done, 1, __ATOMIC_RELEASE);

    uint64_t term_timeout = 50000000ULL;
    while (__atomic_load_n(&g_kctrl_workers_terminated, __ATOMIC_ACQUIRE) < worker_count) {
        __asm__ volatile("pause");
        thread_yield();
        if (--term_timeout == 0) {
            serial_puts("[FAIL] KPROF_CONTROL: Timed out waiting for workers to terminate!\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    reap_deadline = apic_timer_get_ticks() + 5 * apic_timer_get_frequency();
    for (;;) {
        sched_reap_dead();
        vmm_drain_deferred_destructions();
        if (sched_get_active_stack_slots_mask() == initial_stack_mask &&
            vmm_get_deferred_count() == 0) break;
        require(apic_timer_get_ticks() < reap_deadline, "final reaping timeout");
        thread_yield();
    }

    heap_stats_t final_heap;
    pmm_stats_t final_pmm;
    heap_get_stats(&final_heap);
    pmm_get_stats(&final_pmm);
    size_t final_tables = vmm_get_allocated_table_frames();

    require(final_heap.used_bytes == baseline_heap.used_bytes, "control final heap used mismatch");
    require(final_heap.allocated_blocks == baseline_heap.allocated_blocks, "control final heap blocks mismatch");
    require(final_tables == baseline_tables, "control final table frames mismatch");
    require(final_pmm.free_pages == baseline_pmm.free_pages, "control final pmm free mismatch");
    require(pmm_snapshot(after, sizeof(after)) &&
            memcmp(before, after, sizeof(before)) == 0,
            "control final pmm bitmap mismatch");
    require(heap_verify_integrity() && pmm_audit(), "control final integrity audit");

    uint64_t overall_elapsed_tsc = overall_end_tsc >= overall_start_tsc ? (overall_end_tsc - overall_start_tsc) : 0;
    uint64_t delta_pmm_acq = pmm_acq_after >= pmm_acq_before ? (pmm_acq_after - pmm_acq_before) : 0;
    uint64_t delta_pmm_cont = pmm_cont_after >= pmm_cont_before ? (pmm_cont_after - pmm_cont_before) : 0;
    uint64_t delta_tlb_calls = tlb_after.calls >= tlb_before.calls ? (tlb_after.calls - tlb_before.calls) : 0;
    uint64_t delta_tlb_wait_cycles = tlb_after.wait_cycles >= tlb_before.wait_cycles ? (tlb_after.wait_cycles - tlb_before.wait_cycles) : 0;
    uint64_t total_ops = worker_count * KPROF_ITERATIONS;

    serial_puts("[KPROF_CONTROL_SUMMARY] cpus=");
    serial_print_dec(total_cpus);
    serial_puts(" total_ops=");
    serial_print_dec(total_ops);
    serial_puts(" overall_elapsed_tsc=");
    serial_print_dec(overall_elapsed_tsc);
    serial_puts(" pmm_acq=");
    serial_print_dec(delta_pmm_acq);
    serial_puts(" pmm_cont=");
    serial_print_dec(delta_pmm_cont);
    serial_puts(" tlb_calls=");
    serial_print_dec(delta_tlb_calls);
    serial_puts(" tlb_wait_cycles=");
    serial_print_dec(delta_tlb_wait_cycles);
    serial_puts("\n");

    serial_puts("[KPROF_CONTROL] exact_heap=PASS exact_tables=PASS zero_deferred=PASS pmm_recovered=PASS exact_bitmap=PASS\n");
    serial_puts("[KPROF_CONTROL] PASS kernel stack untracked gated control complete\n");
}




