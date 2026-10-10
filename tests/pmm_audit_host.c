/* Actual PMM, synthetic memmap and single-threaded checked lock adapter. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mm/pmm.c"

bool vmm_boot_memory_ready(void) { return true; }
uintptr_t vmm_get_kernel_pml4(void) { return 0x1000; }
uintptr_t vmm_get_current_pml4(void) { return 0x1000; }
void serial_puts(const char *s) { assert(!heap_host_lock_depth); fputs(s, stdout); }
void serial_print_dec(uint64_t n) { assert(!heap_host_lock_depth); printf("%llu", (unsigned long long)n); }
void serial_print_hex(uint64_t n) { assert(!heap_host_lock_depth); printf("0x%llx", (unsigned long long)n); }

int main(void) {
    unsigned char *ram = calloc(1, 8 * 1024 * 1024);
    assert(ram);
    struct limine_memmap_entry region = {0x100000, 4 * 1024 * 1024 + 3 * PAGE_SIZE, LIMINE_MEMMAP_USABLE};
    struct limine_memmap_entry *entries[] = {&region};
    struct limine_memmap_response map = {0, 1, entries};
    pmm_init(&map, (uintptr_t)ram);
    assert(pmm_unlock_high_memory() && pmm_audit());
    used_pages++;
    assert(!pmm_audit());
    used_pages--;
    free_pages--;
    assert(!pmm_audit());
    free_pages++;
    bitmap_clear(0);
    used_pages--; free_pages++;
    assert(!pmm_audit());
    bitmap_set(0);
    used_pages++; free_pages--;
    size_t reserved_hole = 0x80000 / PAGE_SIZE;
    bitmap_clear(reserved_hole);
    used_pages--; free_pages++;
    assert(!pmm_audit());
    bitmap_set(reserved_hole);
    used_pages++; free_pages--;
    assert(pmm_audit());
    puts("PASS audit rejects wrong used/free counters and freed reservations; partial final byte");

    unsigned char baseline[PMM_BITMAP_CAPACITY_BYTES], after[PMM_BITMAP_CAPACITY_BYTES];
    uintptr_t held[2048];
    size_t count = 0;
    assert(pmm_snapshot(baseline, sizeof(baseline)));
    for (uintptr_t p; (p = pmm_alloc_page()) != 0;) {
        assert(count < sizeof(held) / sizeof(*held));
        held[count++] = p;
    }
    assert(pmm_get_free_pages() == 0 && pmm_audit());
    for (size_t i = 0; i < count; i += 2) pmm_free_page(held[i]);
    assert(pmm_get_free_pages() > 0 && pmm_alloc_pages(2) == 0 && pmm_audit());
    uintptr_t p = pmm_alloc_page();
    assert(p); pmm_free_page(p);
    pmm_stats_t stats;
    pmm_get_stats(&stats);
    printf("PASS pressure: free=%zu largest_run=1 allocation_failures=%llu max_scan_steps=%llu\n",
           stats.free_pages, (unsigned long long)stats.allocation_failures,
           (unsigned long long)stats.max_scan_steps);
    for (size_t i = 1; i < count; i += 2) pmm_free_page(held[i]);
    assert(pmm_snapshot(after, sizeof(after)));
    assert(!memcmp(baseline, after, sizeof(after)) && pmm_audit());
    free(ram);
    puts("PASS exact allocation-set restoration after exhaustion and fragmentation");
    return 0;
}
