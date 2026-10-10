/* Actual heap code, single-threaded lock adapter, synthetic PMM/VMM.
 * Host virtual backing replaces privileged kernel addresses. No IRQ claim. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../src/mm/heap.c"

#define HOST_PAGES 4096
static unsigned char frames[HOST_PAGES];
static uintptr_t leaves[HOST_PAGES];
static uintptr_t tables[HOST_PAGES / 512];
static uintptr_t arena;
static uint64_t root[512];
static size_t frame_budget = HOST_PAGES;

void serial_puts(const char *s) { fputs(s, stdout); }
void serial_print_hex(uint64_t n) { printf("0x%llx", (unsigned long long)n); }
void serial_print_dec(uint64_t n) { printf("%llu", (unsigned long long)n); }
uint64_t *vmm_get_kernel_pml4_virt(void) { return root; }
uintptr_t pmm_alloc_page(void) {
    if (!frame_budget) return 0;
    for (size_t i = 1; i < HOST_PAGES; i++) {
        if (!frames[i]) { frames[i] = 1; frame_budget--; return i * PAGE_SIZE; }
    }
    return 0;
}
void pmm_free_page(uintptr_t p) {
    size_t i = p / PAGE_SIZE;
    assert(p && !(p % PAGE_SIZE) && i < HOST_PAGES && frames[i]);
    for (size_t j = 0; j < HOST_PAGES; j++) assert(leaves[j] != p);
    frames[i] = 0;
    frame_budget++;
}
int vmm_map_page(uint64_t *r, uintptr_t va, uintptr_t p, uint64_t flags) {
    assert(r == root && (flags & PTE_NX) && va >= arena);
    size_t i = (va - arena) / PAGE_SIZE;
    assert(i < HOST_PAGES && !leaves[i] && frames[p / PAGE_SIZE]);
    if (!tables[i / 512]) {
        uintptr_t t = pmm_alloc_page();
        if (!t) return VMM_ERR_NOMEM;
        tables[i / 512] = t;
    }
    leaves[i] = p;
    assert(mprotect((void *)va, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);
    return VMM_OK;
}
int vmm_unmap_page(uint64_t *r, uintptr_t va) {
    assert(r == root && va >= arena);
    size_t i = (va - arena) / PAGE_SIZE;
    assert(i < HOST_PAGES && leaves[i]);
    leaves[i] = 0;
    assert(mprotect((void *)va, PAGE_SIZE, PROT_NONE) == 0);
    return VMM_OK;
}

static size_t count_frames(void) {
    size_t count = 0;
    for (size_t i = 1; i < HOST_PAGES; i++) count += frames[i] != 0;
    return count;
}
static size_t count_tables(void) {
    size_t count = 0;
    for (size_t i = 0; i < HOST_PAGES / 512; i++) count += tables[i] != 0;
    return count;
}
static void initialize(void) {
    void *p = mmap(NULL, HOST_PAGES * PAGE_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    arena = (uintptr_t)p;
    g_heap_start = g_heap_end = arena;
    assert(heap_expand(4 * PAGE_SIZE));
    g_heap_ready = true;
    assert(heap_verify_integrity());
}
static void test_shrink(void) {
    unsigned char *p = kmalloc(256);
    void *guard = kmalloc(64);
    assert(p && guard);
    memset(p, 0x75, 256);
    size_t blocks = heap_get_allocated_blocks();
    assert(krealloc(p, 48) == p);
    for (size_t i = 0; i < 48; i++) assert(p[i] == 0x75);
    assert(heap_get_allocated_blocks() == blocks);
    assert(heap_verify_integrity());
    kfree(p); kfree(guard);
    assert(heap_get_allocated_blocks() == 0 && heap_get_used_bytes() == 0);
    assert(heap_get_free_blocks() == 1 && heap_verify_integrity());
    heap_stats_t stats;
    heap_get_stats(&stats);
    assert(stats.used_bytes == 0 && stats.allocated_blocks == 0 && stats.free_blocks == 1);
    assert(stats.free_bytes == stats.total_bytes && stats.largest_free_payload == stats.free_bytes - 32);
    puts("PASS shrink: payload, block count and coalescing");
}
static void test_overflow(void) {
    unsigned char *p = kmalloc(256);
    assert(p);
    memset(p, 0x69, 256);
    const size_t sizes[] = {SIZE_MAX, SIZE_MAX - 15, SIZE_MAX - 31,
        SIZE_MAX / 2, KERNEL_HEAP_MAX - KERNEL_HEAP_START};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        size_t used = heap_get_used_bytes(), blocks = heap_get_allocated_blocks();
        assert(krealloc(p, sizes[i]) == NULL);
        assert(heap_get_used_bytes() == used && heap_get_allocated_blocks() == blocks);
        for (size_t j = 0; j < 256; j++) assert(p[j] == 0x69);
        assert(heap_verify_integrity());
    }
    assert(kcalloc(SIZE_MAX, 2) == NULL);
    assert(kmalloc(SIZE_MAX) == NULL);
    kfree(p);
    puts("PASS overflow: rejected before mutation, original allocation preserved");
}
static void test_audit(void) {
    void *p = kmalloc(64);
    assert(p);
    g_allocated_blocks++;
    assert(!heap_verify_integrity());
    g_allocated_blocks--;
    assert(heap_verify_integrity());
    kfree(p);
    puts("PASS audit: independent live-block count detects corruption");
}
static void test_resize(void) {
    unsigned char *a = kmalloc(64);
    void *b = kmalloc(256), *guard = kmalloc(64);
    assert(a && b && guard);
    memset(a, 0x42, 64);
    kfree(b);
    assert(krealloc(a, 128) == a);
    for (size_t i = 0; i < 64; i++) assert(a[i] == 0x42);
    unsigned char *moved = krealloc(a, 1024);
    assert(moved && moved != a);
    for (size_t i = 0; i < 64; i++) assert(moved[i] == 0x42);
    assert(heap_get_allocated_blocks() == 2 && heap_verify_integrity());
    assert(krealloc(moved, 0) == NULL);
    kfree(guard);
    assert(heap_get_allocated_blocks() == 0 && heap_get_free_blocks() == 1);
    puts("PASS resize: growth, relocation and zero-size release");
}
static void test_rollback(void) {
    unsigned char *live = kmalloc(128);
    assert(live);
    memset(live, 0x81, 128);
    void *filler = kmalloc(heap_get_free_bytes() - 32);
    assert(filler);
    for (unsigned kind = HEAP_FAULT_PMM_AFTER_N_PAGES;
         kind <= HEAP_FAULT_VMM_AFTER_N_PAGES; kind++) {
        for (size_t cut = 0; cut < 5; cut++) {
            size_t total = heap_get_total_bytes(), used = heap_get_used_bytes();
            size_t blocks = heap_get_allocated_blocks();
            unsigned char before[HOST_PAGES];
            uintptr_t mapping_before[HOST_PAGES];
            memcpy(before, frames, sizeof(before));
            memcpy(mapping_before, leaves, sizeof(leaves));
            heap_set_fault_injection((heap_fault_type_t)kind, cut);
            assert(kmalloc(4 * PAGE_SIZE) == NULL);
            heap_clear_fault_injection();
            assert(total == heap_get_total_bytes() && used == heap_get_used_bytes());
            assert(blocks == heap_get_allocated_blocks());
            assert(memcmp(before, frames, sizeof(before)) == 0);
            assert(memcmp(mapping_before, leaves, sizeof(leaves)) == 0);
            for (size_t i = 0; i < 128; i++) assert(live[i] == 0x81);
            assert(heap_verify_integrity());
        }
    }
    kfree(filler);
    /* Start two pages before a new PT region; failed expansion retains only
     * the adapter's newly published table, never its data frames. */
    assert(heap_expand(510 * PAGE_SIZE - heap_get_total_bytes()));
    size_t baseline = count_frames(), old_tables = count_tables();
    size_t total = heap_get_total_bytes();
    unsigned char expected_frames[HOST_PAGES];
    uintptr_t old_leaves[HOST_PAGES];
    memcpy(expected_frames, frames, sizeof(expected_frames));
    memcpy(old_leaves, leaves, sizeof(old_leaves));
    heap_set_fault_injection(HEAP_FAULT_VMM_AFTER_N_PAGES, 4);
    assert(!heap_expand(5 * PAGE_SIZE));
    heap_clear_fault_injection();
    assert(heap_get_total_bytes() == total && count_tables() == old_tables + 1);
    assert(count_frames() == baseline + 1);
    assert(tables[1] && !expected_frames[tables[1] / PAGE_SIZE]);
    expected_frames[tables[1] / PAGE_SIZE] = 1;
    assert(!memcmp(expected_frames, frames, sizeof(expected_frames)));
    assert(!memcmp(old_leaves, leaves, sizeof(old_leaves)));
    for (size_t i = 510; i < 515; i++) assert(!leaves[i]);
    assert(heap_verify_integrity());
    kfree(live);
    puts("PASS rollback: ten cuts, exact frame/mapping sets and retained-table boundary");
}
static void test_pressure(void) {
    void *slots[128] = {0};
    size_t lengths[128] = {0};
    uint32_t rng = 0x18ac431;
    heap_stats_t fragmented = {0};
    for (size_t step = 0; step < 10000; step++) {
        rng = rng * 1664525U + 1013904223U;
        size_t i = (rng >> 16) % 128;
        if (slots[i]) {
            for (size_t j = 0; j < lengths[i]; j++)
                assert(((unsigned char *)slots[i])[j] == (unsigned char)i);
            kfree(slots[i]); slots[i] = NULL;
        } else {
            lengths[i] = 1 + (rng % 8192);
            slots[i] = kmalloc(lengths[i]);
            assert(slots[i]);
            memset(slots[i], (unsigned char)i, lengths[i]);
        }
        assert(heap_verify_integrity());
        heap_stats_t stats;
        heap_get_stats(&stats);
        assert(stats.used_bytes + stats.free_bytes == stats.total_bytes);
        if (stats.free_blocks > fragmented.free_blocks) fragmented = stats;
    }
    for (size_t i = 0; i < 128; i++) kfree(slots[i]);
    assert(heap_get_used_bytes() == 0 && heap_get_allocated_blocks() == 0);
    assert(heap_get_free_blocks() == 1 && heap_verify_integrity());
    size_t retained = count_frames();
    assert(retained == heap_get_total_bytes() / PAGE_SIZE + count_tables());
    assert(kmalloc(2 * 1024 * 1024) == NULL); /* 512-page expansion cap + metadata */
    assert(retained == count_frames());
    size_t budget = frame_budget;
    frame_budget = 0;
    void *reuse = kmalloc(heap_get_free_bytes() - 32);
    assert(reuse && retained == count_frames());
    assert(kmalloc(16) == NULL && retained == count_frames());
    kfree(reuse);
    assert(heap_get_used_bytes() == 0 && retained == count_frames());
    frame_budget = budget;
    printf("OBSERVE retained backing: %zu free heap bytes reuse succeeds with zero "
           "synthetic PMM budget; expansion fails; freeing retains %zu frames\n",
           heap_get_free_bytes(), retained);
    void *filler = kmalloc(heap_get_free_bytes() - 32);
    assert(filler);
    frame_budget = 1;
    assert(kmalloc(1024 * 1024) == NULL);
    assert(retained == count_frames() && frame_budget == 1);
    frame_budget = budget;
    kfree(filler);
    assert(heap_verify_integrity());
    printf("OBSERVE fragmentation: free_blocks=%zu free_bytes=%zu largest_payload=%zu\n",
           fragmented.free_blocks, fragmented.free_bytes, fragmented.largest_free_payload);
    printf("PASS pressure: 10000 mixed-size operations, free=%zu largest_payload=%zu "
           "committed_pages=%zu retained_tables=%zu; expansion-cap and frame OOM\n",
           heap_get_free_bytes(), heap_get_free_bytes() - 32,
           (size_t)(heap_get_total_bytes() / PAGE_SIZE), count_tables());
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    initialize();
    const char *which = argc == 2 ? argv[1] : "all";
    if (!strcmp(which, "all") || !strcmp(which, "shrink")) test_shrink();
    if (!strcmp(which, "all") || !strcmp(which, "overflow")) test_overflow();
    if (!strcmp(which, "all")) {
        test_audit(); test_resize(); test_pressure(); test_rollback();
    }
    assert(munmap((void *)arena, HOST_PAGES * PAGE_SIZE) == 0);
    return 0;
}
