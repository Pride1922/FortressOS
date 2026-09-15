#include "types.h"
#include "limine.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "string.h"
#include "boot_info.h"
#include "vmm.h"

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
                   framebuffer_request.response);

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

    /* 12. Framebuffer Initialization & Test Pattern (Using Kernel-Owned boot_info) */
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

    serial_puts("\n[BOOT] FortressOS Phase 4A (VMM & 4-Level Paging) complete. CPU halted.\n");

    /* Clean halt state */
    hcf();
}
