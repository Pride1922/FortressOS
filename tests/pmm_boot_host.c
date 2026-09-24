/* Actual PMM with single-threaded host lock/CR3/readiness/serial shims.
 * This establishes boot ceiling policy, not SMP exclusion or hardware CR3. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
#include "vmm.h"
#include "memory_boot_test.h"

static bool ready;
static uintptr_t current_root;
bool vmm_boot_memory_ready(void) { return ready; }
uintptr_t vmm_get_kernel_pml4(void) { return 0x1000; }
uintptr_t vmm_get_current_pml4(void) { return current_root; }
void *vmm_phys_to_virt(uintptr_t p) { (void)p; abort(); }
uint64_t *vmm_get_kernel_pml4_virt(void) { abort(); }
uintptr_t vmm_get_physical_address(uint64_t *root, uintptr_t va) {
    (void)root; (void)va; abort();
}
void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t value) { (void)value; }
void serial_print_dec(uint64_t value) { (void)value; }

static unsigned char baseline[PMM_BITMAP_CAPACITY_BYTES];
static unsigned char snapshot[PMM_BITMAP_CAPACITY_BYTES];
static uintptr_t allocated[2048];

static void test_cmdline(void) {
    boot_info_t info = {0};
    assert(!memory_boot_test_enabled(NULL));
    assert(!memory_boot_test_enabled(&info));
    strcpy(info.cmdline, "quiet\tsmp_memory_test=boot\nusb_data_mode=ro");
    assert(memory_boot_test_enabled(&info));
    strcpy(info.cmdline, "xsmp_memory_test=boot smp_memory_test=boot-extra");
    assert(!memory_boot_test_enabled(&info));
    memset(info.cmdline, 'x', sizeof(info.cmdline));
    assert(!memory_boot_test_enabled(&info));
    const char token[] = "smp_memory_test=boot";
    size_t start = sizeof(info.cmdline) - (sizeof(token) - 1);
    info.cmdline[start - 1] = ' ';
    memcpy(info.cmdline + start, token, sizeof(token) - 1);
    assert(!memory_boot_test_enabled(&info));
}

int main(void) {
    test_cmdline();
    /* Only bitmap backing is dereferenced by PMM; high physical ranges are
     * synthetic. An unaligned usable region must not free its partial edges. */
    unsigned char *ram = calloc(1, 4 * 1024 * 1024);
    assert(ram);
    struct limine_memmap_entry low = {0x100003, 0x200000, LIMINE_MEMMAP_USABLE};
    struct limine_memmap_entry edge = {
        PMM_BOOT_ALLOC_LIMIT - 2 * PAGE_SIZE, 4 * PAGE_SIZE, LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry high = {0x780000000ULL, 3 * PAGE_SIZE, LIMINE_MEMMAP_USABLE};
    struct limine_memmap_entry invalid = {UINT64_MAX - 8, 16, LIMINE_MEMMAP_USABLE};
    struct limine_memmap_entry reclaim = {0x400000, PAGE_SIZE, LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE};
    /* Try high and ceiling-straddling placements first: bitmap must still
     * be placed in low RAM or the host backing access itself will fail. */
    struct limine_memmap_entry *entries[] = {&high, &edge, &low, &invalid, &reclaim};
    struct limine_memmap_response map = {0, sizeof(entries) / sizeof(entries[0]), entries};
    pmm_init(&map, (uintptr_t)ram);
    assert(pmm_audit());
    assert(!pmm_high_memory_enabled());
    assert(!pmm_unlock_high_memory());
    assert(pmm_snapshot(baseline, sizeof(baseline)));
    /* Partial first/last pages remain reserved. */
    assert(baseline[0x100000 / PAGE_SIZE / 8] & (1U << ((0x100000 / PAGE_SIZE) % 8)));
    assert(baseline[0x300000 / PAGE_SIZE / 8] & (1U << ((0x300000 / PAGE_SIZE) % 8)));
    size_t eligible = pmm_get_allocatable_pages();
    size_t free_before = pmm_get_free_pages();
    assert(free_before == eligible + 5); /* Two above ceiling plus three high. */
    assert(pmm_alloc_page_above(PMM_BOOT_ALLOC_LIMIT) == 0);
    assert(pmm_alloc_page_above(UINT64_MAX) == 0);
    assert(pmm_alloc_pages(SIZE_MAX) == 0);
    assert(pmm_alloc_pages(0) == 0);

    size_t count = 0;
    for (uintptr_t p; (p = pmm_alloc_page()) != 0;) {
        assert(count < sizeof(allocated) / sizeof(allocated[0]));
        assert(p % PAGE_SIZE == 0 && p < PMM_BOOT_ALLOC_LIMIT);
        for (size_t i = 0; i < count; i++) assert(allocated[i] != p);
        allocated[count++] = p;
    }
    assert(count == eligible && pmm_get_allocatable_pages() == 0);
    assert(pmm_get_free_pages() == 5); /* OOM below ceiling despite high free RAM. */
    assert(pmm_alloc_pages(1) == 0 && pmm_alloc_pages(2) == 0);
    assert(pmm_alloc_page_above(0) == 0);
    for (size_t i = 0; i < count; i++) pmm_free_page(allocated[i]);

    /* No contiguous run may straddle the ceiling. Occupy all other low RAM. */
    count = 0;
    for (uintptr_t p; (p = pmm_alloc_page()) != 0;) {
        assert(count < sizeof(allocated) / sizeof(allocated[0]));
        allocated[count++] = p;
    }
    pmm_free_pages(edge.base, 2);
    assert(pmm_alloc_pages(3) == 0);
    uintptr_t pair = pmm_alloc_pages(2);
    assert(pair == edge.base);
    pmm_free_pages(pair, 2);
    ready = true;
    current_root = 0x2000;
    assert(!pmm_unlock_high_memory());
    assert(!pmm_high_memory_enabled());
    current_root = vmm_get_kernel_pml4();
    assert(pmm_unlock_high_memory());
    assert(pmm_high_memory_enabled());
    uintptr_t run = pmm_alloc_pages(4);
    assert(run == edge.base); /* Crossing is allowed only after activation. */
    pmm_free_pages(run, 4);
    for (size_t i = 0; i < count; i++)
        if (allocated[i] < edge.base) pmm_free_page(allocated[i]);
    assert(pmm_get_allocatable_pages() == pmm_get_free_pages());
    uintptr_t high_page = pmm_alloc_page_above(high.base + 1);
    assert(high_page == high.base + PAGE_SIZE);
    pmm_free_page(high_page);
    assert(pmm_reclaim_bootloader_memory(&map) == 0);
    assert(pmm_snapshot(snapshot, sizeof(snapshot)));
    assert(memcmp(baseline, snapshot, sizeof(snapshot)) == 0);
    assert(pmm_get_free_pages() == free_before);

    /* Less than 1 GiB managed: unlock remains required for AP readiness, but
     * the eligible count already equals all free RAM. */
    struct limine_memmap_entry *small_entries[] = {&low};
    struct limine_memmap_response small = {0, 1, small_entries};
    ready = false;
    current_root = 0;
    pmm_init(&small, (uintptr_t)ram);
    assert(!pmm_high_memory_enabled());
    assert(pmm_get_allocatable_pages() == pmm_get_free_pages());
    assert(!pmm_unlock_high_memory());
    ready = true;
    current_root = vmm_get_kernel_pml4();
    assert(pmm_unlock_high_memory());
    assert(pmm_get_allocatable_pages() == pmm_get_free_pages());
    free(ram);
    puts("PASS PMM boot: capped exhaustion, contiguous boundary, unlock gates, rounding, exact allocation set, cmdline");
    return 0;
}
