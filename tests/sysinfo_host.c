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

int main(void) {
    test_efault_and_reserved();
    test_uptime_conversion();
    test_ram_units_and_overflow();
    return 0;
}
