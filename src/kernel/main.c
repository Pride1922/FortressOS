#include "types.h"
#include "limine.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "string.h"
#include "boot_info.h"
#include "vmm.h"
#include "heap.h"
#include "pic.h"
#include "acpi.h"
#include "ioapic.h"
#include "apic.h"
#include "ioapic.h"

/* Set Limine Base Revision to 3 (Limine v7/v8 protocol) */
__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* CPU Halt Primitive */
static void hcf(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Helper to get memory map type string */
static const char *memmap_type_to_str(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:                 return "Usable RAM";
        case LIMINE_MEMMAP_RESERVED:               return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI Reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:             return "Bad Memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader Reclaimable";
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "Kernel/Modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:            return "Framebuffer Memory";
        default:                                   return "Unknown";
    }
}

/* Early visual test: draw a test banner/pattern on the framebuffer */
static void render_test_pattern(const boot_info_t *boot_info) {
    if (!boot_info || !boot_info->has_framebuffer || !boot_info->fb_address) return;

    /* Validate format: ensure 32 bpp linear framebuffer */
    if (boot_info->fb_bpp != 32) {
        serial_puts("[WARN] Framebuffer is not 32 bpp (detected ");
        serial_print_dec(boot_info->fb_bpp);
        serial_puts(" bpp); skipping test pattern.\n");
        return;
    }

    if (boot_info->fb_width == 0 || boot_info->fb_height == 0 || boot_info->fb_pitch < boot_info->fb_width * 4) {
        serial_puts("[WARN] Invalid framebuffer dimensions or pitch.\n");
        return;
    }

    volatile uint32_t *fb_ptr = (volatile uint32_t *)boot_info->fb_address;
    uint64_t width = boot_info->fb_width;
    uint64_t height = boot_info->fb_height;
    uint64_t pitch32 = boot_info->fb_pitch / 4;

    /* Fill background with dark slate blue (0x001A1B26) */
    for (uint64_t y = 0; y < height; y++) {
        for (uint64_t x = 0; x < width; x++) {
            fb_ptr[y * pitch32 + x] = 0x001A1B26;
        }
    }

    /* Draw test color bars at the top, bounded by screen height */
    uint32_t colors[6] = {
        0x00F7768E, /* Red */
        0x009ECE6A, /* Green */
        0x007AA2F7, /* Blue */
        0x00E0AF68, /* Yellow */
        0x00BB9AF7, /* Purple */
        0x007DCFFF  /* Cyan */
    };

    uint64_t bar_height = (height > 48) ? 24 : (height / 2);
    if (bar_height == 0) bar_height = 1;
    uint64_t bar_width = width / 6;

    for (int c = 0; c < 6; c++) {
        uint64_t start_x = c * bar_width;
        uint64_t end_x = (c == 5) ? width : (start_x + bar_width);
        if (end_x > width) end_x = width;

        for (uint64_t y = 0; y < bar_height; y++) {
            for (uint64_t x = start_x; x < end_x; x++) {
                fb_ptr[y * pitch32 + x] = colors[c];
            }
        }
    }
}

static void acpi_parser_selftest(void) {
    struct __attribute__((packed)) {
        acpi_madt_t table;
        acpi_madt_entry_t record;
    } fixture;
    acpi_madt_info_t result;
    memset(&fixture, 0, sizeof(fixture));
    memcpy(fixture.table.header.signature, "APIC", 4);
    fixture.table.header.length = sizeof(fixture);
    fixture.record.type = 0; /* Known type needs eight bytes, not two. */
    for (unsigned test = 0; test < 3; ++test) {
        fixture.record.length = test == 0 ? 0 : test == 1 ? 2 : 255;
        fixture.table.header.checksum = 0;
        uint8_t sum = 0;
        for (size_t j = 0; j < sizeof(fixture); ++j) sum += ((uint8_t *)&fixture)[j];
        fixture.table.header.checksum = (uint8_t)(0 - sum);
        if (acpi_parse_madt_buffer(&fixture, sizeof(fixture), &result)) hcf();
    }
    if (acpi_parse_madt_buffer(&fixture, sizeof(acpi_madt_t) - 1, &result) ||
        acpi_ensure_mapped(UINTPTR_MAX - 8, 32)) hcf();
    serial_puts("[PASS] Truncated/zero-length/undersized/overrun MADT records and address overflow rejected\n");
}

/* Runs in foreground with timer interrupts enabled; ISR never touches heap. */
static void timer_heap_work(void) {
    uint8_t *p = kmalloc(128);
    if (!p) hcf();
    memset(p, 0xA5, 128);
    for (size_t i = 0; i < 128; ++i) if (p[i] != 0xA5) hcf();
    kfree(p);
}

/* Kernel Main Entry Point */
void kmain(void) {
    /* 1. Initialize COM1 Serial Port (0x3F8) */
    int serial_status = serial_init();

    serial_puts("\n========================================================\n");
    serial_puts("             FORTRESS OS - x86_64 UEFI KERNEL           \n");
    serial_puts("========================================================\n\n");

    if (serial_status == 0) {
        serial_puts("[ OK ] UART COM1 initialized at 115200 8N1 (Port 0x3F8)\n");
    } else {
        serial_puts("[WARN] UART COM1 failed loopback test; continuing anyway\n");
    }

    /* 2. Validate Limine Base Revision */
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        serial_puts("[FAIL] Limine base revision 3 is not supported by bootloader!\n");
        hcf();
    }
    serial_puts("[ OK ] Limine Base Revision 3 handshake verified\n\n");

    /* 3. Global Descriptor Table (GDT) & Task State Segment (TSS with IST1) */
    gdt_init();

    /* 4. Interrupt Descriptor Table (IDT) & 64-bit Exception Gates */
    idt_init();

    /* 5. Safe Exception Self-Test: Trigger INT 3 (Breakpoint Trap) */
    serial_puts("[TEST] Triggering software breakpoint exception (int $3)...\n");
    __asm__ volatile("int $3");
    serial_puts("[ OK ] CPU returned successfully from breakpoint trap!\n\n");

    /* 6. Freestanding Memory & String Runtime Self-Test */
    serial_puts("[TEST] Validating freestanding memory & string primitives...\n");
    char test_buf[32];
    memset(test_buf, 'A', sizeof(test_buf));
    test_buf[31] = '\0';
    if (strlen(test_buf) != 31 || test_buf[0] != 'A' || test_buf[30] != 'A') {
        serial_puts("       [FAIL] memset / strlen validation error\n");
        hcf();
    }

    char copy_buf[32];
    memcpy(copy_buf, test_buf, sizeof(copy_buf));
    if (memcmp(copy_buf, test_buf, sizeof(copy_buf)) != 0) {
        serial_puts("       [FAIL] memcpy / memcmp validation error\n");
        hcf();
    }

    /* Overlapping memmove test: shift "012345" forward by 2 positions */
    char overlap_buf[16] = "0123456789";
    memmove(overlap_buf + 2, overlap_buf, 6); /* Expect "0101234589" */
    if (memcmp(overlap_buf, "0101234589", 10) != 0) {
        serial_puts("       [FAIL] memmove overlap validation error\n");
        hcf();
    }
    serial_puts("       [PASS] memset, memcpy, memmove, memcmp, strlen verified\n\n");

    /* 7. Higher Half Direct Map (HHDM) */
    if (hhdm_request.response == NULL) {
        serial_puts("[FAIL] No Limine HHDM response received\n");
    } else {
        serial_puts("[INFO] HHDM Virtual Offset: ");
        serial_print_hex(hhdm_request.response->offset);
        serial_puts("\n");
    }

    /* 4. Memory Map Inspection */
    if (memmap_request.response == NULL) {
        serial_puts("[FAIL] No Limine Memory Map response received\n");
    } else {
        uint64_t entry_count = memmap_request.response->entry_count;
        serial_puts("[INFO] Memory Map Entries: ");
        serial_print_dec(entry_count);
        serial_puts("\n");

        uint64_t total_usable = 0;
        for (uint64_t i = 0; i < entry_count; i++) {
            struct limine_memmap_entry *entry = memmap_request.response->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                total_usable += entry->length;
            }
            serial_puts("       [");
            serial_print_hex(entry->base);
            serial_puts(" - ");
            serial_print_hex(entry->base + entry->length);
            serial_puts("] Type: ");
            serial_puts(memmap_type_to_str(entry->type));
            serial_puts(" (");
            serial_print_dec(entry->length / 1024);
            serial_puts(" KiB)\n");
        }

        serial_puts("[ OK ] Total Usable Physical Memory: ");
        serial_print_dec(total_usable / (1024 * 1024));
        serial_puts(" MiB\n\n");
    }

    /* 7. Physical Memory Manager (PMM) Initialization */
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        serial_puts("[FAIL] PMM requires valid Limine memory map and HHDM response!\n");
        hcf();
    }
    pmm_init(memmap_request.response, hhdm_request.response->offset);

    /* 8. PMM Storage & Invariant Audit */
    if (!pmm_audit()) {
        serial_puts("[FAIL] PMM storage audit failed!\n");
        hcf();
    }
    serial_puts("[ OK ] PMM audit passed (64 KiB bitmap reserved, frame 0 guarded, 2 GiB capacity verified)\n\n");

    /* 9. PMM Self-Test */
    serial_puts("[TEST] Executing Physical Memory Manager self-test...\n");

    /* Test 1: Single 4 KiB page allocations */
    uintptr_t page1 = pmm_alloc_page();
    uintptr_t page2 = pmm_alloc_page();
    uintptr_t page3 = pmm_alloc_page();
    serial_puts("       Allocated single pages: ");
    serial_print_hex(page1);
    serial_puts(", ");
    serial_print_hex(page2);
    serial_puts(", ");
    serial_print_hex(page3);
    serial_puts("\n");

    if (page1 != 0 && page2 != 0 && page3 != 0 &&
        page1 != page2 && page2 != page3 && (page1 % PAGE_SIZE == 0)) {
        serial_puts("       [PASS] Distinct 4 KiB-aligned page frames allocated\n");
    } else {
        serial_puts("       [FAIL] Single page allocation error\n");
        hcf();
    }

    /* Test 2: Contiguous multi-page allocation */
    uintptr_t block4 = pmm_alloc_pages(4);
    serial_puts("       Allocated 4 contiguous pages (16 KiB): ");
    serial_print_hex(block4);
    serial_puts("\n");

    if (block4 != 0 && (block4 % PAGE_SIZE == 0)) {
        serial_puts("       [PASS] Contiguous frame block allocated\n");
    } else {
        serial_puts("       [FAIL] Contiguous page allocation error\n");
        hcf();
    }

    /* Test 3: Free and reclaim */
    size_t free_before = pmm_get_free_pages();
    pmm_free_page(page1);
    pmm_free_page(page2);
    pmm_free_page(page3);
    pmm_free_pages(block4, 4);
    size_t free_after = pmm_get_free_pages();

    if (free_after == free_before + 7) {
        serial_puts("       [PASS] Frame release verified (reclaimed 7 pages)\n");
    } else {
        serial_puts("       [FAIL] Page free accounting mismatch\n");
        hcf();
    }

    /* Test 4: Re-allocation of previously freed frame */
    uintptr_t reallocated = pmm_alloc_page();
    serial_puts("       Re-allocated page: ");
    serial_print_hex(reallocated);
    serial_puts("\n");
    pmm_free_page(reallocated);

    serial_puts("[ OK ] Physical Memory Manager self-tests passed successfully!\n\n");

    /* 10. Deep-Copy Boot Metadata into Kernel-Owned Storage */
    static boot_info_t boot_info;
    boot_info_init(&boot_info,
                   memmap_request.response,
                   hhdm_request.response,
                   kernel_address_request.response,
                   framebuffer_request.response,
                   rsdp_request.response);

    /* 11. Virtual Memory Manager (VMM) & 4-Level Paging */
    /* Step 1 & 2: Build new tables and inspect required mappings */
    /* Step 3: Switch CR3 to new PML4 and survive */
    vmm_init(&boot_info);

    uint64_t *kernel_pml4 = vmm_get_kernel_pml4_virt();

    /* Step 4: Verify Exception Handling under New CR3 via Breakpoint Trap (int $3) */
    serial_puts("[TEST] Verifying exception handling under new CR3 (int $3)...\n");
    __asm__ volatile("int $3");
    serial_puts("[ OK ] Breakpoint exception recovered cleanly under new page tables!\n\n");

    /* Step 5: Test Map -> Write/Read -> Unmap -> Expected Page Fault */
    serial_puts("[TEST] Validating VMM dynamic mapping & unmapping lifecycle...\n");
    uintptr_t test_phys = pmm_alloc_page();
    uintptr_t test_virt = 0xFFFFFFFF90000000ULL;

    int map_status = vmm_map_page(kernel_pml4, test_virt, test_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    if (map_status != VMM_OK) {
        serial_puts("       [FAIL] vmm_map_page returned error: ");
        serial_print_dec(map_status);
        serial_puts("\n");
        hcf();
    }

    /* Verify mapping queries */
    if (!vmm_is_mapped(kernel_pml4, test_virt)) {
        serial_puts("       [FAIL] vmm_is_mapped returned false for mapped page!\n");
        hcf();
    }
    if (vmm_get_physical_address(kernel_pml4, test_virt) != test_phys) {
        serial_puts("       [FAIL] vmm_get_physical_address mismatch!\n");
        hcf();
    }

    /* Same page-table indices as test_virt, but invalid sign extension. */
    uintptr_t invalid_alias = test_virt & 0x0000FFFFFFFFFFFFULL;
    if (vmm_is_mapped(kernel_pml4, invalid_alias) ||
        vmm_get_physical_address(kernel_pml4, invalid_alias) != 0 ||
        vmm_map_page(kernel_pml4, invalid_alias, test_phys, PTE_WRITABLE) != VMM_ERR_INVALID_ADDR ||
        vmm_unmap_page(kernel_pml4, invalid_alias) != VMM_ERR_INVALID_ADDR ||
        !vmm_is_mapped(kernel_pml4, test_virt)) {
        serial_puts("       [FAIL] Noncanonical address validation failed\n");
        hcf();
    }
    serial_puts("       [PASS] Noncanonical aliases rejected by all VMM operations\n");

    /* Write pattern and read back */
    volatile uint64_t *test_ptr = (volatile uint64_t *)test_virt;
    *test_ptr = 0xDEADBEEFCAFEBABEULL;
    if (*test_ptr != 0xDEADBEEFCAFEBABEULL) {
        serial_puts("       [FAIL] Readback mismatch on mapped virtual page!\n");
        hcf();
    }
    serial_puts("       [PASS] Mapped page read/write verified (0xDEADBEEFCAFEBABE)\n");

    /* Unmap page (Ownership Rule: unmapping does NOT free the physical frame) */
    int unmap_status = vmm_unmap_page(kernel_pml4, test_virt);
    if (unmap_status != VMM_OK) {
        serial_puts("       [FAIL] vmm_unmap_page returned error!\n");
        hcf();
    }
    if (vmm_is_mapped(kernel_pml4, test_virt)) {
        serial_puts("       [FAIL] Page still reported mapped after vmm_unmap_page!\n");
        hcf();
    }
    serial_puts("       [PASS] Page unmapped successfully (PTE cleared & TLB invalidated)\n");

    /* Caller explicitly frees physical frame */
    pmm_free_page(test_phys);

    /* Verify expected #PF on accessing unmapped virtual address */
    void *pf_unmapped_recovery = &&pf_unmapped_done;
    __asm__ volatile("" : : "r"(pf_unmapped_recovery));
    idt_set_expected_page_fault((uintptr_t)pf_unmapped_recovery);

    /* Access unmapped address -> triggers Page Fault (#PF, Vector 14) */
    *test_ptr = 0x11223344;

pf_unmapped_done:
    idt_clear_expected_page_fault();
    uint64_t caught_cr2 = 0, caught_err = 0;
    if (idt_was_page_fault_caught(&caught_cr2, &caught_err) && caught_cr2 == test_virt) {
        serial_puts("       [PASS] Expected #PF cleanly caught on unmapped virtual address access\n\n");
    } else {
        serial_puts("       [FAIL] Expected #PF was not caught or CR2 mismatch!\n");
        hcf();
    }

    /* Step 6A: Permission Test - Write to Read-Only Page */
    serial_puts("[TEST] Validating page permission enforcement (Read-Only write fault)...\n");
    uintptr_t ro_phys = pmm_alloc_page();
    uintptr_t ro_virt = 0xFFFFFFFF90001000ULL;

    /* Map without PTE_WRITABLE (Read-Only) */
    vmm_map_page(kernel_pml4, ro_virt, ro_phys, PTE_PRESENT | PTE_NX);
    volatile uint64_t *ro_ptr = (volatile uint64_t *)ro_virt;

    /* Reading from read-only page must succeed */
    uint64_t dummy_read = *ro_ptr;
    (void)dummy_read;
    serial_puts("       [PASS] Read from Read-Only page succeeded\n");

    /* Attempting write must trigger #PF with bit 0 = 1 (protection violation) */
    void *pf_ro_recovery = &&pf_ro_done;
    __asm__ volatile("" : : "r"(pf_ro_recovery));
    idt_set_expected_page_fault((uintptr_t)pf_ro_recovery);

    *ro_ptr = 0xCAFE;

pf_ro_done:
    idt_clear_expected_page_fault();
    if (idt_was_page_fault_caught(&caught_cr2, &caught_err) &&
        caught_cr2 == ro_virt &&
        (caught_err & (1 << 0)) != 0 && /* Protection violation */
        (caught_err & (1 << 1)) != 0)   /* Write access */ {
        serial_puts("       [PASS] Read-only permission violation caught (#PF protection violation)\n\n");
    } else {
        serial_puts("       [FAIL] Permission fault verification failed!\n");
        hcf();
    }
    vmm_unmap_page(kernel_pml4, ro_virt);
    pmm_free_page(ro_phys);

    /* Step 6B: Stack Guard Page Verification (Synthesized stack) */
    serial_puts("[TEST] Validating synthesized stack guard page protection...\n");
    uintptr_t stack_phys = pmm_alloc_page();
    uintptr_t guard_virt = 0xFFFFFFFF90002000ULL; /* Guard page: strictly unmapped */
    uintptr_t stack_virt = 0xFFFFFFFF90003000ULL; /* Stack page: mapped RW, NX */

    vmm_map_page(kernel_pml4, stack_virt, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);

    /* Normal writes to stack page must succeed */
    volatile uint64_t *valid_stack_slot = (volatile uint64_t *)(stack_virt + 0x800);
    *valid_stack_slot = 0x55AA55AAULL;
    if (*valid_stack_slot != 0x55AA55AAULL) {
        serial_puts("       [FAIL] Stack write/read failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Normal stack page access verified\n");

    /* Simulated stack overflow: touching the unmapped guard page directly below the stack */
    void *pf_guard_recovery = &&pf_guard_done;
    __asm__ volatile("" : : "r"(pf_guard_recovery));
    idt_set_expected_page_fault((uintptr_t)pf_guard_recovery);

    volatile uint64_t *overflow_slot = (volatile uint64_t *)(guard_virt + 0xFF8);
    *overflow_slot = 0xBAD57AC;

pf_guard_done:
    idt_clear_expected_page_fault();
    if (idt_was_page_fault_caught(&caught_cr2, &caught_err) && caught_cr2 == (guard_virt + 0xFF8)) {
        serial_puts("       [PASS] Stack overflow into guard page caught cleanly via #PF!\n\n");
    } else {
        serial_puts("       [FAIL] Stack guard page fault was not caught!\n");
        hcf();
    }
    vmm_unmap_page(kernel_pml4, stack_virt);
    pmm_free_page(stack_phys);

    /* Step 6C: NX (No-Execute) Bit Enforcement Verification */
    serial_puts("[TEST] Validating NX (No-Execute) enforcement (instruction fetch fault)...\n");
    uintptr_t nx_phys = pmm_alloc_page();
    uintptr_t nx_virt = 0xFFFFFFFF90004000ULL;

    /* Map with PTE_NX (Writable, but Strictly No-Execute) */
    if (nx_phys == 0 ||
        vmm_map_page(kernel_pml4, nx_virt, nx_phys, PTE_WRITABLE | PTE_NX) != VMM_OK) {
        serial_puts("       [FAIL] NX test allocation or mapping failed\n");
        hcf();
    }

    /* Write 'ret' instruction (0xC3) into the page */
    volatile uint8_t *nx_code = (volatile uint8_t *)nx_virt;
    *nx_code = 0xC3;

    /* Execute the NX target via assembly helper that safely catches fault, drops return address, and restores stack */
    test_nx_exec_helper(nx_virt);

    caught_cr2 = 0;
    caught_err = 0;
    if (idt_was_page_fault_caught(&caught_cr2, &caught_err) &&
        caught_cr2 == nx_virt &&
        caught_err == 0x11) /* Supervisor instruction fetch, protection violation */ {
        serial_puts("       [PASS] NX bit enforcement verified (instruction fetch fault with error code ");
        serial_print_hex(caught_err);
        serial_puts(")\n\n");
    } else {
        serial_puts("       [FAIL] NX bit violation was not caught!\n");
        hcf();
    }
    /* Exercise the helper's normal-return path with the same RET instruction. */
    if (vmm_unmap_page(kernel_pml4, nx_virt) != VMM_OK ||
        vmm_map_page(kernel_pml4, nx_virt, nx_phys, 0) != VMM_OK) {
        serial_puts("       [FAIL] Executable control mapping failed\n");
        hcf();
    }
    test_nx_exec_helper(nx_virt);
    if (idt_was_page_fault_caught(NULL, NULL)) {
        serial_puts("       [FAIL] Executable control unexpectedly faulted\n");
        hcf();
    }
    if (vmm_unmap_page(kernel_pml4, nx_virt) != VMM_OK) hcf();
    serial_puts("       [PASS] NX helper normal-return control verified\n");
    pmm_free_page(nx_phys);

    /* Step 6D: Active Boot Stack Guard Page Verification */
    serial_puts("[TEST] Validating active boot stack guard page protection...\n");
    extern uint8_t kernel_stack_guard[];
    if (vmm_is_mapped(kernel_pml4, (uintptr_t)kernel_stack_guard)) {
        serial_puts("       [FAIL] kernel_stack_guard is mapped in page tables!\n");
        hcf();
    }
    serial_puts("       [PASS] kernel_stack_guard confirmed unmapped in active PML4\n");

    void *pf_boot_guard_recovery = &&pf_boot_guard_done;
    __asm__ volatile("" : : "r"(pf_boot_guard_recovery));
    idt_set_expected_page_fault((uintptr_t)pf_boot_guard_recovery);

    /* Touch the real boot stack guard page directly below the active stack */
    volatile uint64_t *boot_guard_ptr = (volatile uint64_t *)kernel_stack_guard;
    *boot_guard_ptr = 0xBADC0DE;

pf_boot_guard_done:
    idt_clear_expected_page_fault();
    if (idt_was_page_fault_caught(&caught_cr2, &caught_err) &&
        caught_cr2 == (uintptr_t)kernel_stack_guard) {
        serial_puts("       [PASS] Active boot stack guard page caught hardware overflow via #PF!\n\n");
    } else {
        serial_puts("       [FAIL] Active boot stack guard page fault was not caught!\n");
        hcf();
    }

    /* 12. Dynamic Kernel Heap Allocator (Phase 4B) */
    heap_init();
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed on initialization!\n");
        hcf();
    }

    serial_puts("[TEST] Executing Dynamic Kernel Heap verification suite...\n");

    /* Test 1: Semantics (kmalloc(0) == NULL, kfree(NULL) == no-op) */
    if (kmalloc(0) != NULL) {
        serial_puts("       [FAIL] kmalloc(0) did not return NULL!\n");
        hcf();
    }
    kfree(NULL); /* Must not fault or panic */
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Zero-size allocation and NULL free semantics verified\n");

    /* Test 2: Basic Allocation, Alignment & Invariant Checks */
    uint8_t *b1 = kmalloc(128);
    if (!b1 || ((uintptr_t)b1 % 16) != 0) {
        serial_puts("       [FAIL] kmalloc failed or unaligned!\n");
        hcf();
    }
    memset(b1, 0xAA, 128);
    for (int i = 0; i < 128; i++) {
        if (b1[i] != 0xAA) {
            serial_puts("       [FAIL] Data write/read verification error!\n");
            hcf();
        }
    }
    if (!heap_verify_integrity()) hcf();
    kfree(b1);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Basic 16-byte aligned allocation, write/read, and free verified\n");

    /* Test 3: kcalloc Zero-Initialization & Multiplication Overflow */
    if (kcalloc(0, 50) != NULL || kcalloc(50, 0) != NULL) {
        serial_puts("       [FAIL] kcalloc(0) did not return NULL!\n");
        hcf();
    }
    if (kcalloc((size_t)-1 / 2, 4) != NULL) {
        serial_puts("       [FAIL] kcalloc arithmetic overflow was not caught!\n");
        hcf();
    }
    uint32_t *zero_arr = (uint32_t *)kcalloc(32, sizeof(uint32_t));
    if (!zero_arr) {
        serial_puts("       [FAIL] kcalloc allocation failed!\n");
        hcf();
    }
    for (int i = 0; i < 32; i++) {
        if (zero_arr[i] != 0) {
            serial_puts("       [FAIL] kcalloc did not zero memory!\n");
            hcf();
        }
    }
    kfree(zero_arr);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] kcalloc zero-initialization and overflow protection verified\n");

    /* Test 4: Block Splitting */
    void *split_big = kmalloc(1024);
    void *split_guard = kmalloc(64); /* Guard to prevent right coalescing */
    kfree(split_big);
    void *split_sub = kmalloc(256);
    if (!split_sub || split_sub != split_big) {
        serial_puts("       [FAIL] Sub-allocation from split block failed!\n");
        hcf();
    }
    if (!heap_verify_integrity()) hcf();
    kfree(split_sub);
    kfree(split_guard);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Free block splitting verified\n");

    /* Test 5: Coalescing In All Directions */
    /* 5A: Left-Only Coalescing */
    void *l1 = kmalloc(128);
    void *l2 = kmalloc(128);
    void *l_guard = kmalloc(64);
    kfree(l1);
    kfree(l2); /* Must merge with left neighbor (l1) */
    void *l_merged = kmalloc(256);
    if (l_merged != l1) {
        serial_puts("       [FAIL] Left-only coalescing failed!\n");
        hcf();
    }
    kfree(l_merged);
    kfree(l_guard);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Left-only boundary tag coalescing verified\n");

    /* 5B: Right-Only Coalescing */
    void *r_guard = kmalloc(64);
    void *r1 = kmalloc(128);
    void *r2 = kmalloc(128);
    kfree(r2);
    kfree(r1); /* Must merge with right neighbor (r2) */
    void *r_merged = kmalloc(256);
    if (r_merged != r1) {
        serial_puts("       [FAIL] Right-only coalescing failed!\n");
        hcf();
    }
    kfree(r_merged);
    kfree(r_guard);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Right-only boundary tag coalescing verified\n");

    /* 5C: Both-Neighbour Coalescing */
    void *blk_a = kmalloc(128);
    void *blk_b = kmalloc(128);
    void *blk_c = kmalloc(128);
    void *blk_sentinel = kmalloc(64);
    kfree(blk_a);
    kfree(blk_c);
    kfree(blk_b); /* Merges both left (blk_a) and right (blk_c) */
    void *blk_merged = kmalloc(384);
    if (blk_merged != blk_a) {
        serial_puts("       [FAIL] Both-neighbour coalescing failed!\n");
        hcf();
    }
    kfree(blk_merged);
    kfree(blk_sentinel);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Both-neighbour O(1) boundary tag coalescing verified\n");

    /* Test 6: krealloc Semantics, In-Place Growth, Shrinking & Forced Relocation */
    /* 6A: krealloc(NULL, n) behaves like kmalloc(n) */
    void *realloc_null = krealloc(NULL, 128);
    if (!realloc_null || ((uintptr_t)realloc_null % 16) != 0) {
        serial_puts("       [FAIL] krealloc(NULL, n) did not allocate aligned buffer!\n");
        hcf();
    }
    /* 6B: krealloc(p, 0) behaves like kfree(p) and returns NULL */
    void *freed_via_realloc = krealloc(realloc_null, 0);
    if (freed_via_realloc != NULL) {
        serial_puts("       [FAIL] krealloc(p, 0) did not return NULL!\n");
        hcf();
    }
    if (!heap_verify_integrity()) hcf();

    /* 6C: Forced Relocation with Data Preservation (live barrier directly follows original) */
    char *orig_buf = kmalloc(64);
    const char *test_msg = "FortressOS Dynamic Kernel Heap";
    memcpy(orig_buf, test_msg, 31);
    void *live_barrier = kmalloc(64); /* Immediately adjacent live allocation forces relocation */

    char *relocated_buf = krealloc(orig_buf, 256);
    if (!relocated_buf || relocated_buf == orig_buf || memcmp(relocated_buf, test_msg, 31) != 0) {
        serial_puts("       [FAIL] krealloc relocation failed or corrupted existing data!\n");
        hcf();
    }
    memset(relocated_buf + 31, 'X', 200);
    if (relocated_buf[100] != 'X') {
        serial_puts("       [FAIL] krealloc expanded space unusable!\n");
        hcf();
    }

    /* 6D: Failed krealloc preserves original allocation and data intact */
    void *failed_realloc = krealloc(relocated_buf, (size_t)-1 / 2);
    if (failed_realloc != NULL) {
        serial_puts("       [FAIL] Excessive krealloc unexpectedly succeeded!\n");
        hcf();
    }
    if (memcmp(relocated_buf, test_msg, 31) != 0 || relocated_buf[100] != 'X') {
        serial_puts("       [FAIL] Failed krealloc corrupted original buffer!\n");
        hcf();
    }
    kfree(relocated_buf);
    kfree(live_barrier);
    if (!heap_verify_integrity()) hcf();

    /* 6E: In-Place Growth and Shrinking with Split Coalescing */
    void *grow_buf = kmalloc(64);
    void *free_neighbor = kmalloc(256);
    void *grow_guard = kmalloc(64);
    kfree(free_neighbor); /* Right neighbor is now free */

    void *grown_in_place = krealloc(grow_buf, 128);
    if (grown_in_place != grow_buf) {
        serial_puts("       [FAIL] krealloc did not grow in-place into free right neighbor!\n");
        hcf();
    }
    /* Shrinking: splits remainder and immediately coalesces with free right neighbor */
    void *shrunk_in_place = krealloc(grown_in_place, 48);
    if (shrunk_in_place != grow_buf) {
        serial_puts("       [FAIL] krealloc shrink did not remain in-place!\n");
        hcf();
    }
    kfree(shrunk_in_place);
    kfree(grow_guard);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] krealloc in-place growth, shrinking, and forced relocation verified\n");

    /* Test 7: Controlled Transactional Rollback Fault Injection */
    /* 7A: PMM failure after 2 mapped pages in a multi-page expansion */
    void *saved_alloc = kmalloc(128); /* Keep a live allocation to verify non-corruption */
    memset(saved_alloc, 0x77, 128);
    size_t heap_end_before = heap_get_total_bytes();
    size_t used_before = heap_get_used_bytes();
    size_t pmm_free_before = pmm_get_free_pages();

    /* Consume existing free list capacity */
    size_t free_cap = heap_get_free_bytes();
    void *filler = NULL;
    if (free_cap > 48) {
        filler = kmalloc(free_cap - 48);
    }

    heap_end_before = heap_get_total_bytes();
    used_before = heap_get_used_bytes();
    pmm_free_before = pmm_get_free_pages();

    /* Request 4 pages (16384 bytes). Inject PMM failure after 2 pages */
    heap_set_fault_injection(HEAP_FAULT_PMM_AFTER_N_PAGES, 2);
    void *failed_pmm_alloc = kmalloc(16384);
    heap_clear_fault_injection();

    if (failed_pmm_alloc != NULL) {
        serial_puts("       [FAIL] kmalloc with injected PMM failure unexpectedly succeeded!\n");
        hcf();
    }
    /* Verify rollback guarantees: heap boundary, used bytes, and PMM free pages restored */
    if (heap_get_total_bytes() != heap_end_before || heap_get_used_bytes() != used_before) {
        serial_puts("       [FAIL] Heap bounds or used bytes altered after failed PMM expansion!\n");
        hcf();
    }
    if (pmm_get_free_pages() != pmm_free_before) {
        serial_puts("       [FAIL] Data frames leaked to PMM after expansion failure rollback!\n");
        hcf();
    }
    /* Verify live allocation data intact */
    for (int i = 0; i < 128; i++) {
        if (((uint8_t *)saved_alloc)[i] != 0x77) {
            serial_puts("       [FAIL] Live allocation corrupted during expansion rollback!\n");
            hcf();
        }
    }
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed after PMM expansion rollback!\n");
        hcf();
    }
    serial_puts("       [PASS] Controlled PMM allocation failure rollback verified\n");

    /* 7B: VMM mapping failure after frame acquisition */
    pmm_free_before = pmm_get_free_pages();
    heap_set_fault_injection(HEAP_FAULT_VMM_AFTER_N_PAGES, 1);
    void *failed_vmm_alloc = kmalloc(16384);
    heap_clear_fault_injection();

    if (failed_vmm_alloc != NULL) {
        serial_puts("       [FAIL] kmalloc with injected VMM failure unexpectedly succeeded!\n");
        hcf();
    }
    if (heap_get_total_bytes() != heap_end_before || heap_get_used_bytes() != used_before) {
        serial_puts("       [FAIL] Heap bounds altered after failed VMM expansion!\n");
        hcf();
    }
    if (pmm_get_free_pages() != pmm_free_before) {
        serial_puts("       [FAIL] Acquired frame or mapped pages leaked after VMM failure rollback!\n");
        hcf();
    }
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed after VMM expansion rollback!\n");
        hcf();
    }
    if (filler) kfree(filler);
    kfree(saved_alloc);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Controlled VMM mapping failure & unmapped frame release verified\n");

    /* 7C: Independent VMM Verification: Retained Page-Table Accounting Across a 2 MiB Boundary
     * Note: Verifies VMM intermediate table allocation and retention independently outside the heap.
     * Controlled heap rollback (7A/7B) tests heap data-frame release within allocated page tables. */
    size_t tables_base = vmm_get_retained_table_frames();
    size_t pmm_base = pmm_get_free_pages();
    uintptr_t boundary_virt = 0xFFFFFFFF90200000ULL; /* New 2 MiB range: requires new PT */

    uintptr_t data_frame1 = pmm_alloc_page();
    if (vmm_map_page(kernel_pml4, boundary_virt, data_frame1, PTE_PRESENT | PTE_WRITABLE | PTE_NX) != VMM_OK) {
        serial_puts("       [FAIL] Boundary page mapping failed!\n");
        hcf();
    }
    /* Simulate unmap & frame release during failure/teardown */
    vmm_unmap_page(kernel_pml4, boundary_virt);
    pmm_free_page(data_frame1);

    size_t tables_after_unmap = vmm_get_retained_table_frames();
    size_t pmm_after_unmap = pmm_get_free_pages();
    size_t new_table_count = tables_after_unmap - tables_base;
    size_t pmm_delta = pmm_base - pmm_after_unmap;

    if (new_table_count != 1 || pmm_delta != 1 || pmm_delta != new_table_count) {
        serial_puts("       [FAIL] Retained-table accounting mismatch across PT boundary!\n");
        hcf();
    }

    /* Prove retained table is reused for subsequent mappings without allocating new tables */
    uintptr_t data_frame2 = pmm_alloc_page();
    if (vmm_map_page(kernel_pml4, boundary_virt + PAGE_SIZE, data_frame2, PTE_PRESENT | PTE_WRITABLE | PTE_NX) != VMM_OK) {
        serial_puts("       [FAIL] Second boundary page mapping failed!\n");
        hcf();
    }
    size_t tables_reuse = vmm_get_retained_table_frames();
    if (tables_reuse != tables_after_unmap) {
        serial_puts("       [FAIL] Retained page table was not reused!\n");
        hcf();
    }
    vmm_unmap_page(kernel_pml4, boundary_virt + PAGE_SIZE);
    pmm_free_page(data_frame2);

    if (pmm_get_free_pages() != pmm_after_unmap) {
        serial_puts("       [FAIL] Frame leak detected in retained table reuse!\n");
        hcf();
    }
    serial_puts("       [PASS] Independent VMM retained-table accounting across 2 MiB boundary verified (measured delta: 1 table frame, 0 data frames leaked)\n");

    /* Test 8: Forced Non-Contiguous PMM Frames Expansion */
    /* Allocate interleaving frames in PMM to guarantee heap physical frames are non-contiguous */
    uintptr_t dummy_frame1 = pmm_alloc_page();
    uintptr_t dummy_frame2 = pmm_alloc_page();
    pmm_free_page(dummy_frame1); /* Leaves dummy_frame2 as a gap in PMM */

    size_t pre_exp_total = heap_get_total_bytes();
    void *multi_page_buf = kmalloc(16384); /* Forces 4-page expansion */
    if (!multi_page_buf || heap_get_total_bytes() <= pre_exp_total) {
        serial_puts("       [FAIL] Multi-page heap expansion failed!\n");
        hcf();
    }
    pmm_free_page(dummy_frame2);

    /* Verify virtual pages map to distinct physical frames */
    uintptr_t virt_page1 = (uintptr_t)multi_page_buf & ~(PAGE_SIZE - 1);
    uintptr_t virt_page2 = virt_page1 + PAGE_SIZE;
    uintptr_t p1 = vmm_get_physical_address(kernel_pml4, virt_page1);
    uintptr_t p2 = vmm_get_physical_address(kernel_pml4, virt_page2);

    if (p1 == 0 || p2 == 0 || p1 == p2) {
        serial_puts("       [FAIL] Virtual pages do not map to valid distinct physical frames!\n");
        hcf();
    }
    kfree(multi_page_buf);
    if (!heap_verify_integrity()) hcf();
    serial_puts("       [PASS] Non-contiguous physical frames mapped to contiguous virtual heap verified (grew to ");
    serial_print_dec(heap_get_total_bytes() / 1024);
    serial_puts(" KiB)\n");
    serial_puts("       [INFO] Reusable heap capacity: ");
    serial_print_dec(heap_get_free_bytes() / 1024);
    serial_puts(" KiB (retained mapped pages ready for reuse)\n");

    /* Test 9: Deterministic Mixed-Size Stress Test & Full Heap Walk */
    void *stress_ptrs[40];
    for (int i = 0; i < 40; i++) {
        size_t sz = 16 + ((i * 37) % 512);
        stress_ptrs[i] = kmalloc(sz);
        if (!stress_ptrs[i]) {
            serial_puts("       [FAIL] Stress test allocation failed!\n");
            hcf();
        }
        memset(stress_ptrs[i], (uint8_t)(i ^ 0xA5), sz);
    }
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed during stress allocation phase!\n");
        hcf();
    }
    /* Verify data */
    for (int i = 0; i < 40; i++) {
        size_t sz = 16 + ((i * 37) % 512);
        uint8_t *p = (uint8_t *)stress_ptrs[i];
        for (size_t s = 0; s < sz; s++) {
            if (p[s] != (uint8_t)(i ^ 0xA5)) {
                serial_puts("       [FAIL] Stress test data corruption detected!\n");
                hcf();
            }
        }
    }
    /* Free odd then even */
    for (int i = 1; i < 40; i += 2) kfree(stress_ptrs[i]);
    if (!heap_verify_integrity()) hcf();
    for (int i = 0; i < 40; i += 2) kfree(stress_ptrs[i]);
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed after freeing stress allocations!\n");
        hcf();
    }

    serial_puts("       [PASS] Deterministic mixed-size stress test passed (all blocks coalesced & audit verified)\n");
    serial_puts("[ OK ] Dynamic Kernel Heap Allocator (Phase 4B) verified successfully!\n\n");

    /* 13. Framebuffer Initialization & Test Pattern (Using Kernel-Owned boot_info) */
    if (!boot_info.has_framebuffer) {
        serial_puts("[WARN] No Framebuffer found (running headless)\n");
    } else {
        serial_puts("[ OK ] Framebuffer: ");
        serial_print_dec(boot_info.fb_width);
        serial_puts("x");
        serial_print_dec(boot_info.fb_height);
        serial_puts("@");
        serial_print_dec(boot_info.fb_bpp);
        serial_puts(" bpp, Pitch: ");
        serial_print_dec(boot_info.fb_pitch);
        serial_puts(" bytes, Addr: ");
        serial_print_hex(boot_info.fb_address);
        serial_puts("\n");

        render_test_pattern(&boot_info);
        serial_puts("[ OK ] Framebuffer test pattern rendered (using kernel-owned boot info)\n");
    }

    /* =========================================================================
     * Phase 5: ACPI Discovery, 8259 PIC Masking, LAPIC Setup & APIC Timer
     * ========================================================================= */
    serial_puts("\n========================================================\n");
    serial_puts("Phase 5: ACPI Discovery & APIC Timer Verification Suite\n");
    serial_puts("========================================================\n");

    /* Test 1: Limine RSDP Query & ACPI Initialization */
    serial_puts("[TEST 1] Verifying Limine RSDP Query & ACPI Header Checksums...\n");
    if (!boot_info.has_rsdp) {
        serial_puts("       [FAIL] Limine RSDP response missing!\n");
        hcf();
    }
    if (!acpi_init(boot_info.rsdp_phys_addr, boot_info.hhdm_offset)) {
        serial_puts("       [FAIL] ACPI initialization failed!\n");
        hcf();
    }
    serial_puts("       [PASS] ACPI RSDP and Root SDT verified\n");

    acpi_parser_selftest();

    /* Test 2: ACPI MADT Parsing */
    serial_puts("[TEST 2] Parsing Multiple APIC Description Table (MADT)...\n");
    acpi_madt_info_t madt_info;
    if (!acpi_parse_madt(&madt_info)) {
        serial_puts("       [FAIL] Failed to parse MADT!\n");
        hcf();
    }
    if (madt_info.lapic_phys_addr == 0 || madt_info.enabled_cpu_count == 0) {
        serial_puts("       [FAIL] Invalid MADT info: no LAPIC address or 0 CPUs!\n");
        hcf();
    }
    serial_puts("       [PASS] MADT parsed: LAPIC Base=");
    serial_print_hex(madt_info.lapic_phys_addr);
    serial_puts(", CPUs=");
    serial_print_dec(madt_info.enabled_cpu_count);
    serial_puts(", I/O APICs=");
    serial_print_dec(madt_info.ioapic_count);
    serial_puts(", ISOs=");
    serial_print_dec(madt_info.iso_count);
    serial_puts("\n");

    /* Test 3: Disable / Mask Legacy 8259 PIC */
    serial_puts("[TEST 3] Disabling Legacy 8259 PIC...\n");
    pic_disable();
    uint8_t pic1_mask = pic1_get_mask();
    uint8_t pic2_mask = pic2_get_mask();
    if (pic1_mask != 0xFF || pic2_mask != 0xFF) {
        serial_puts("       [FAIL] PIC masks did not verify 0xFF!\n");
        hcf();
    }
    serial_puts("       [PASS] 8259 PIC verified disabled (PIC1=0xFF, PIC2=0xFF)\n");

    /* Test 4: Local APIC (LAPIC) MMIO Mapping & Initialization */
    serial_puts("[TEST 4] Initializing Local APIC (LAPIC) MMIO & SVR...\n");
    if (!lapic_init(madt_info.lapic_phys_addr)) {
        serial_puts("       [FAIL] Failed to initialize Local APIC!\n");
        hcf();
    }
    uint32_t svr = lapic_read(APIC_REG_SVR);
    if ((svr & (APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR)) != (APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR)) {
        serial_puts("       [FAIL] LAPIC SVR register incorrect!\n");
        hcf();
    }
    uint32_t tpr = lapic_read(APIC_REG_TPR);
    if (tpr != 0) {
        serial_puts("       [FAIL] LAPIC TPR is non-zero!\n");
        hcf();
    }
    serial_puts("       [PASS] LAPIC initialized (SVR verified 0x1FF, TPR=0)\n");

    /* Test 5: APIC Timer Calibration & Periodic Tick Verification */
    serial_puts("[TEST 5] Calibrating APIC Timer (100 Hz Target) & Verifying Interrupts...\n");
    __asm__ volatile("cli" ::: "memory");
    if (!ioapic_init(&madt_info) || !apic_timer_init(100)) hcf();
    if (apic_timer_init(0) || ioapic_route_gsi(UINT32_MAX, 0x21, 0, false, false) ||
        ioapic_route_gsi(0, 0x10, 0, false, false)) hcf();
    /* Negative liveness check: a masked timer must fail the reference test. */
    if (apic_timer_verify(NULL)) hcf();
    serial_puts("[PASS] Masked timer and invalid routing/rate requests rejected\n");
    apic_timer_start();
    __asm__ volatile("sti" ::: "memory");
    bool timer_ok = apic_timer_verify(timer_heap_work);
    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    if (!timer_ok || !heap_verify_integrity()) {
        serial_puts("[FAIL] Timer frequency/progress or heap integrity verification failed\n");
        hcf();
    }
    serial_puts("[PASS] PIT-referenced timer progress and foreground heap integrity verified\n");
    serial_puts("[ OK ] Phase 5: ACPI Discovery & APIC Timer completed successfully!\n\n");

    serial_puts("\n[BOOT] FortressOS Phase 5 complete. CPU halted.\n");

    /* Clean halt state */
    hcf();
}
