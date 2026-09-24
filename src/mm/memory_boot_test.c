#include "memory_boot_test.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"

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
