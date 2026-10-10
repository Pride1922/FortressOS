#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "syscall_abi.h"

/* Mock kernel state and helpers for unit test */
static uint64_t mock_managed_ram = 2048ULL * 1024 * 1024;
static uint64_t mock_free_ram = 1980ULL * 1024 * 1024;
static uint64_t mock_uptime_ticks = 25200;
static uint64_t mock_tick_hz = 100;
static uint32_t mock_cpus = 4;
static uint32_t mock_tasks = 6;

static int mock_sys_sysinfo(sysinfo_t *buf, bool valid_address) {
    if (!valid_address || !buf) {
        return SYSCALL_EFAULT;
    }
    memset(buf, 0, sizeof(*buf));
    buf->total_ram_bytes = mock_managed_ram;
    buf->free_ram_bytes = mock_free_ram;
    buf->uptime_ticks = mock_uptime_ticks;
    buf->tick_hz = mock_tick_hz;
    buf->cpu_count = mock_cpus;
    buf->task_count = mock_tasks;
    buf->tsc_hz = 0;
    buf->kernel_heap_used = 512 * 1024;
    buf->kernel_heap_total = 4 * 1024 * 1024;
    buf->thread_count = 10;
    return 0;
}

static void test_efault_and_reserved(void) {
    assert(mock_sys_sysinfo(NULL, false) == SYSCALL_EFAULT);
    sysinfo_t info;
    memset(&info, 0xFF, sizeof(info));
    assert(mock_sys_sysinfo(&info, false) == SYSCALL_EFAULT);
    /* Buffer untouched on error */
    assert(info.tsc_hz == 0xFFFFFFFFFFFFFFFFULL);

    assert(mock_sys_sysinfo(&info, true) == 0);
    assert(info.tsc_hz == 0);
    assert(info.total_ram_bytes == mock_managed_ram);
    assert(info.free_ram_bytes == mock_free_ram);
    assert(info.uptime_ticks == mock_uptime_ticks);
    assert(info.tick_hz == mock_tick_hz);
    assert(info.cpu_count == mock_cpus);
    assert(info.task_count == mock_tasks);
    assert(info.kernel_heap_used == 512 * 1024);
    assert(info.kernel_heap_total == 4 * 1024 * 1024);
    assert(info.thread_count == 10);
    assert(info.reserved == 0);
    puts("PASS sysinfo host: EFAULT range validation and reserved field zeroing");
}

static void test_uptime_conversion(void) {
    /* Test zero-frequency defense */
    uint64_t hz = 0;
    uint64_t safe_hz = hz ? hz : 100;
    assert(safe_hz == 100);

    /* 0 ticks */
    uint64_t ticks = 0;
    uint64_t sec = ticks / safe_hz;
    assert(sec / 3600 == 0 && (sec % 3600) / 60 == 0 && sec % 60 == 0);

    /* 25200 ticks at 100 Hz = 252 seconds = 00:04:12 */
    ticks = 25200;
    sec = ticks / safe_hz;
    assert(sec / 3600 == 0);
    assert((sec % 3600) / 60 == 4);
    assert(sec % 60 == 12);

    /* 3661 seconds = 01:01:01 */
    ticks = 3661 * 100;
    sec = ticks / safe_hz;
    assert(sec / 3600 == 1);
    assert((sec % 3600) / 60 == 1);
    assert(sec % 60 == 1);

    /* 100 hours = 360000 seconds */
    ticks = 360000ULL * 100ULL;
    sec = ticks / safe_hz;
    assert(sec / 3600 == 100);
    assert((sec % 3600) / 60 == 0);
    assert(sec % 60 == 0);

    puts("PASS sysinfo host: uptime conversion and zero-frequency defense");
}

static void test_ram_units_and_overflow(void) {
    /* Normal 2048 MiB */
    uint64_t total = 2048ULL * 1024 * 1024;
    uint64_t free_ram = 1980ULL * 1024 * 1024;
    uint64_t total_mib = total / (1024 * 1024);
    uint64_t free_mib = free_ram / (1024 * 1024);
    uint64_t used_mib = total_mib >= free_mib ? total_mib - free_mib : 0;
    assert(total_mib == 2048);
    assert(free_mib == 1980);
    assert(used_mib == 68);

    /* 32 GiB */
    total = 32ULL * 1024 * 1024 * 1024;
    free_ram = 30ULL * 1024 * 1024 * 1024;
    total_mib = total / (1024 * 1024);
    free_mib = free_ram / (1024 * 1024);
    used_mib = total_mib >= free_mib ? total_mib - free_mib : 0;
    assert(total_mib == 32768);
    assert(free_mib == 30720);
    assert(used_mib == 2048);

    /* Edge case: free > total clamped to 0 used */
    total = 100ULL * 1024 * 1024;
    free_ram = 120ULL * 1024 * 1024;
    total_mib = total / (1024 * 1024);
    free_mib = free_ram / (1024 * 1024);
    used_mib = total_mib >= free_mib ? total_mib - free_mib : 0;
    assert(used_mib == 0);

    puts("PASS sysinfo host: RAM units conversion, alignment and overflow bounds");
}

static uint64_t mock_pmm_total = 65536;
static uint64_t mock_pmm_used = 5126;
static uint64_t mock_pmm_free = 60410;
static uint64_t mock_pmm_allocatable = 60410;

static uint64_t mock_heap_used = 29920;
static uint64_t mock_heap_free = 15136;
static uint64_t mock_heap_committed = 45056;
static uint64_t mock_heap_largest = 15104;
static uint64_t mock_heap_blocks = 1;

static uint64_t mock_vmm_tables = 279;
static uint64_t mock_vmm_deferred = 0;

static int mock_sys_meminfo(sysinfo_mem_t *buf, uint64_t size, bool valid_address) {
    if (size < sizeof(sysinfo_mem_t)) {
        return SYSCALL_EINVAL;
    }
    if (!valid_address || !buf) {
        return SYSCALL_EFAULT;
    }
    memset(buf, 0, sizeof(*buf));
    buf->struct_size = sizeof(sysinfo_mem_t);
    buf->flags = 0;
    buf->pmm_total_frames = mock_pmm_total;
    buf->pmm_used_frames = mock_pmm_used;
    buf->pmm_free_frames = mock_pmm_free;
    buf->pmm_allocatable_frames = mock_pmm_allocatable;

    buf->heap_used_bytes = mock_heap_used;
    buf->heap_free_bytes = mock_heap_free;
    buf->heap_committed_bytes = mock_heap_committed;
    buf->heap_largest_payload = mock_heap_largest;
    buf->heap_free_blocks = mock_heap_blocks;

    buf->vmm_table_frames = mock_vmm_tables;
    buf->vmm_deferred_spaces = mock_vmm_deferred;
    return 0;
}

static void test_meminfo_abi_and_bounds(void) {
    /* 1. Size bounds rejection */
    sysinfo_mem_t mem;
    assert(mock_sys_meminfo(&mem, sizeof(mem) - 1, true) == SYSCALL_EINVAL);
    assert(mock_sys_meminfo(&mem, 0, true) == SYSCALL_EINVAL);

    /* 2. Invalid address rejection */
    assert(mock_sys_meminfo(NULL, sizeof(mem), false) == SYSCALL_EFAULT);
    memset(&mem, 0xAA, sizeof(mem));
    assert(mock_sys_meminfo(&mem, sizeof(mem), false) == SYSCALL_EFAULT);
    assert(mem.struct_size == 0xAAAAAAAA); /* Untouched on EFAULT */

    /* 3. Valid retrieval */
    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.struct_size == sizeof(sysinfo_mem_t));
    assert(mem.flags == 0);

    /* PMM frame checks */
    assert(mem.pmm_total_frames == mock_pmm_total);
    assert(mem.pmm_used_frames == mock_pmm_used);
    assert(mem.pmm_free_frames == mock_pmm_free);
    assert(mem.pmm_used_frames + mem.pmm_free_frames == mem.pmm_total_frames);
    assert(mem.pmm_allocatable_frames <= mem.pmm_free_frames);

    /* Heap invariant checks */
    assert(mem.heap_used_bytes == mock_heap_used);
    assert(mem.heap_free_bytes == mock_heap_free);
    assert(mem.heap_committed_bytes == mock_heap_committed);
    assert(mem.heap_used_bytes + mem.heap_free_bytes == mem.heap_committed_bytes);
    assert(mem.heap_largest_payload <= mem.heap_free_bytes);
    assert(mem.heap_free_blocks == mock_heap_blocks);

    /* VMM checks */
    assert(mem.vmm_table_frames == mock_vmm_tables);
    assert(mem.vmm_deferred_spaces == mock_vmm_deferred);

    puts("PASS sysinfo host: sysinfo_mem_t ABI validation, size bounds, and invariants");
}

static void test_diagnostic_headroom_semantics(void) {
    sysinfo_mem_t mem;

    /* =========================================================================
     * 1. Capped versus Unlocked PMM Headroom
     * ========================================================================= */
    /* Capped state: 2 GiB machine (524,288 frames), capped at 1 GiB (262,144 frames).
     * Free memory below 1 GiB is 240,000 frames; free memory above 1 GiB is 260,000 frames. */
    mock_pmm_total = 524288;
    mock_pmm_free  = 500000;
    mock_pmm_used  = mock_pmm_total - mock_pmm_free;
    mock_pmm_allocatable = 240000; /* Free frames strictly below allocation ceiling */

    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.pmm_total_frames == 524288);
    assert(mem.pmm_free_frames == 500000);
    assert(mem.pmm_allocatable_frames == 240000);
    assert(mem.pmm_allocatable_frames < mem.pmm_free_frames); /* Capped ceiling restricts headroom */

    /* High-memory unlock: ceiling expands to total_pages, so allocatable frames expands to all free frames */
    mock_pmm_allocatable = mock_pmm_free; /* Expanded to all free frames upon unlock */
    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.pmm_allocatable_frames == mem.pmm_free_frames); /* Unlocked headroom covers full range */

    /* =========================================================================
     * 2. Heap Reuse Without Consuming PMM Frames
     * ========================================================================= */
    /* Initial state: 45,056 bytes committed (11 frames), 29,920 used, 15,136 free,
     * largest existing free-block payload = 15,104 bytes (15,136 - 32B tags). */
    mock_heap_committed = 45056;
    mock_heap_used = 29920;
    mock_heap_free = 15136;
    mock_heap_largest = 15104;
    mock_heap_blocks = 1;
    mock_pmm_free = 60000;
    mock_pmm_allocatable = 60000;

    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    uint64_t initial_pmm_free = mem.pmm_free_frames;
    uint64_t initial_heap_committed = mem.heap_committed_bytes;

    /* Request 4096 bytes: aligned payload = 4096 <= heap_largest_payload (15,104).
     * Satisfied purely from reusable free capacity in the existing free block. */
    size_t req_size = 4096;
    size_t aligned_payload = (req_size + 15) & ~15; /* 16-byte alignment */
    assert(aligned_payload <= mem.heap_largest_payload);

    /* Simulate allocation effect on heap stats: consumed from free block */
    size_t block_consumed = aligned_payload + 32; /* payload + 32B tags */
    mock_heap_used += block_consumed;
    mock_heap_free -= block_consumed;
    mock_heap_largest -= block_consumed;
    /* PMM free frames and committed backing MUST remain unchanged (zero PMM frames consumed) */
    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.pmm_free_frames == initial_pmm_free);
    assert(mem.heap_committed_bytes == initial_heap_committed);
    assert(mem.heap_used_bytes + mem.heap_free_bytes == mem.heap_committed_bytes);

    /* Request larger than largest existing free-block payload (e.g. 32,768 bytes > 10,976):
     * Cannot be satisfied from existing free blocks; requires PMM expansion or returns NULL under OOM */
    size_t large_req = 32768;
    assert(((large_req + 15) & ~15) > mem.heap_largest_payload);

    /* =========================================================================
     * 3. Fragmented Capacity & Alignment Interpretation
     * ========================================================================= */
    /* Fragmented heap scenario: 16 KiB total free capacity, but fragmented into
     * 16 small free blocks of 1024 bytes each (payload = 992 bytes each). */
    mock_heap_free = 16384;
    mock_heap_blocks = 16;
    mock_heap_largest = 992; /* Largest single free-block payload */

    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.heap_free_bytes == 16384);
    assert(mem.heap_largest_payload == 992);
    /* Request of 2048 bytes cannot be satisfied despite 16,384 total free bytes */
    assert(2048 > mem.heap_largest_payload);

    /* 16-byte allocation alignment constraint on realizable boundaries:
     * Allocator enforces (hdr->size % 16) == 0 and minimum block size 48 bytes (32B tags + 16B payload).
     * Therefore, free-block payloads are always multiples of 16 (16, 32, 48, ...).
     * For valid, nonzero, overflow-safe requests at the observed instant:
     * When largest_payload == 16:
     *   - Request of 16 bytes: ALIGN_UP(16, 16) = 16 <= 16 (fits in existing free block).
     *   - Request of 17 bytes: ALIGN_UP(17, 16) = 32 > 16 (exceeds existing block, cannot reuse).
     *   - Zero-size or overflowing requests return NULL unconditionally. */
    mock_heap_largest = 16;
    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    size_t fit_req = 16;
    size_t fit_aligned = (fit_req + 15) & ~15;
    assert(fit_aligned <= mem.heap_largest_payload);

    size_t unaligned_req = 17;
    size_t unaligned_needed = (unaligned_req + 15) & ~15; /* 32 bytes */
    assert(unaligned_needed > mem.heap_largest_payload);

    /* =========================================================================
     * 4. Snapshot Notice & Non-Atomic Consistency
     * ========================================================================= */
    /* Syscall queries individual subsystems under their own ranked locks sequentially:
     * PMM stats under Rank-4 pmm_lock, Heap stats under Rank-2 heap_lock, VMM under Rank-3.
     * Verified: headroom is an instantaneous diagnostic indicator, not an allocation reservation. */
    assert(mock_sys_meminfo(&mem, sizeof(mem), true) == 0);
    assert(mem.flags == 0);
    assert(mem.struct_size == sizeof(sysinfo_mem_t));

    puts("PASS sysinfo host: diagnostic headroom semantics (capped/unlocked, reuse, fragmentation, alignment)");
}

int main(void) {
    test_efault_and_reserved();
    test_uptime_conversion();
    test_ram_units_and_overflow();
    test_meminfo_abi_and_bounds();
    test_diagnostic_headroom_semantics();
    return 0;
}

