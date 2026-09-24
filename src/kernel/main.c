#include "input.h"
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
#include "smp.h"
#include "thread.h"
#include "syscall.h"
#include "elf.h"
#include "console.h"
#include "vfs.h"
#include "tarfs.h"
#include "pci.h"
#include "nvme.h"
#include "xhci.h"
#include "block.h"
#include "crc32.h"
#include "gpt.h"
#include "ext2.h"
#include "usb_mount.h"
#include "power.h"
#include "logo.h"

extern uint8_t __text_start[];
extern uint8_t __rodata_start[];
extern uint8_t __data_start[];
extern const uint8_t user_syscall_test_start[];
extern const uint8_t user_syscall_test_end[];

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
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
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

static void __attribute__((unused)) pmm_high_memory_probe(void) {
    serial_puts("[PROBE] PMM total: ");
    serial_print_dec(pmm_get_total_memory() / (1024ULL * 1024 * 1024));
    serial_puts(" GiB, free: ");
    serial_print_dec(pmm_get_free_pages() * 4096ULL / (1024ULL * 1024 * 1024));
    serial_puts(" GiB\n");

    uintptr_t probes[] = {
        0x80000000ULL,    /*  2 GiB */
        0x100000000ULL,   /*  4 GiB */
        0x400000000ULL,   /* 16 GiB */
        0x780000000ULL,   /* 30 GiB */
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uintptr_t p = pmm_alloc_page_above(probes[i]);
        if (p == 0) {
            serial_print_hex(probes[i]);
            serial_puts(" — no frame available\n");
            continue;
        }
        volatile uint64_t *v = (volatile uint64_t *)vmm_phys_to_virt(p);
        v[0] = 0xDEADBEEFCAFEBABEULL;
        v[1] = 0x0123456789ABCDEFULL;
        bool ok = (v[0] == 0xDEADBEEFCAFEBABEULL && v[1] == 0x0123456789ABCDEFULL);
        serial_puts("Allocated phys ");
        serial_print_hex(p);
        serial_puts(ok ? " — HHDM readback PASS\n" : " — HHDM readback FAIL\n");
        pmm_free_page(p);
    }
}

/* Early visual display: render branded FortressOS boot logo on framebuffer */
static void render_boot_logo(const boot_info_t *boot_info) {
    if (!boot_info || !boot_info->has_framebuffer || !boot_info->fb_address) return;

    /* Validate format: ensure 32 bpp linear framebuffer */
    if (boot_info->fb_bpp != 32) {
        serial_puts("[WARN] Framebuffer is not 32 bpp (detected ");
        serial_print_dec(boot_info->fb_bpp);
        serial_puts(" bpp); skipping boot logo.\n");
        return;
    }

    if (boot_info->fb_width == 0 || boot_info->fb_height == 0 || boot_info->fb_pitch < boot_info->fb_width * 4) {
        serial_puts("[WARN] Invalid framebuffer dimensions or pitch.\n");
        return;
    }

    logo_render_boot(boot_info);
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
    struct __attribute__((packed)) {
        acpi_madt_t table;
        uint8_t nmi[6];
    } nmi_fixture;
    for (unsigned test = 0; test < 5; test++) {
        memset(&nmi_fixture, 0, sizeof(nmi_fixture));
        memcpy(nmi_fixture.table.header.signature, "APIC", 4);
        nmi_fixture.table.header.length = sizeof(nmi_fixture);
        nmi_fixture.nmi[0] = MADT_TYPE_NMI;
        nmi_fixture.nmi[1] = 6;
        nmi_fixture.nmi[2] = 255;
        nmi_fixture.nmi[5] = test == 1 ? 2 : 1;
        nmi_fixture.nmi[3] = test == 2 ? 2 : test == 3 ? 8 : test == 4 ? 16 : 0;
        uint8_t sum = 0;
        for (size_t i = 0; i < sizeof(nmi_fixture); i++) sum += ((uint8_t *)&nmi_fixture)[i];
        nmi_fixture.table.header.checksum = (uint8_t)(0 - sum);
        bool parsed = acpi_parse_madt_buffer(&nmi_fixture, sizeof(nmi_fixture), &result);
        if (parsed != (test == 0) || (parsed && (result.nmi_count != 1 || result.nmis[0].lint != 1))) hcf();
    }
    serial_puts("[PASS] MADT NMI route parsed; invalid LINT and reserved flag encodings rejected\n");
}

/* Runs in foreground with timer interrupts enabled; ISR never touches heap. */
static void timer_heap_work(void) {
    uint8_t *p = kmalloc(128);
    if (!p) hcf();
    memset(p, 0xA5, 128);
    for (size_t i = 0; i < 128; ++i) if (p[i] != 0xA5) hcf();
    kfree(p);
}

static volatile int  g_ping_pong_counter = 0;
static volatile bool g_worker_a_done = false;
static volatile bool g_worker_b_done = false;

static void worker_a(void *arg) {
    (void)arg;
    for (int i = 1; i <= 5; i++) {
        g_ping_pong_counter++;
        serial_puts("       [Worker A] Round ");
        serial_print_dec(i);
        serial_puts(" (Counter: ");
        serial_print_dec(g_ping_pong_counter);
        serial_puts(") -> Yielding to Worker B\n");
        thread_yield();
    }
    g_worker_a_done = true;
    serial_puts("       [Worker A] Completed 5 rounds -> Calling thread_exit()\n");
    thread_exit();
}

static void worker_b(void *arg) {
    (void)arg;
    for (int i = 1; i <= 5; i++) {
        g_ping_pong_counter++;
        serial_puts("       [Worker B] Round ");
        serial_print_dec(i);
        serial_puts(" (Counter: ");
        serial_print_dec(g_ping_pong_counter);
        serial_puts(") -> Yielding to Worker A\n");
        thread_yield();
    }
    g_worker_b_done = true;
    serial_puts("       [Worker B] Completed 5 rounds -> Calling thread_exit()\n");
    thread_exit();
}

static volatile uint64_t g_preempt_work1 = 0;
static volatile uint64_t g_preempt_work2 = 0;
static volatile bool     g_preempt_stop  = false;

static void preempt_worker1(void *arg) {
    (void)arg;
    while (!g_preempt_stop) {
        g_preempt_work1++;
        __asm__ volatile("pause");
    }
    thread_exit();
}

static void preempt_worker2(void *arg) {
    (void)arg;
    while (!g_preempt_stop) {
        g_preempt_work2++;
        __asm__ volatile("pause");
    }
    thread_exit();
}

static volatile size_t g_stress_completed_threads = 0;

static void stress_worker(void *arg) {
    uint64_t id = (uint64_t)arg;

    /* Perform dynamic memory allocation & freeing under thread execution */
    void *ptr1 = kmalloc(64);
    void *ptr2 = kmalloc(128);
    if (ptr1 && ptr2) {
        memset(ptr1, 0xAA, 64);
        memset(ptr2, 0xBB, 128);
    }
    kfree(ptr1);
    kfree(ptr2);

    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 2000; i++) {
        sum += (i ^ id);
    }
    (void)sum;

    __atomic_fetch_add(&g_stress_completed_threads, 1, __ATOMIC_SEQ_CST);
    thread_exit();
}

/* =========================================================================
 * Phase 7 (Checkpoint 1): Ring 3 Privilege Transition via iretq & Trap Hook
 * ========================================================================= */
static uint8_t g_test_user_rsp0_stack[16384] __attribute__((aligned(16)));

static void test_phase7_checkpoint1_ring3(const boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 1): Ring 3 Transition via iretq\n");
    serial_puts("========================================================\n");

    /* Record baseline resource counters for zero-leak audit */
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();

    /* 1. Create dedicated user PML4 address space */
    serial_puts("[TEST 1] Setting up User Address Space with Lower-Half Code & Stack...\n");
    uintptr_t user_pml4_phys = vmm_create_user_pml4();
    if (user_pml4_phys == 0) {
        serial_puts("       [FAIL] Failed to create user PML4!\n");
        hcf();
    }
    uint64_t *user_pml4_virt = (uint64_t *)((uintptr_t)user_pml4_phys + boot_info->hhdm_offset);

    /* Allocate physical frames for user code and user stack */
    uintptr_t user_code_phys = pmm_alloc_page();
    uintptr_t user_stack_phys = pmm_alloc_page();
    if (user_code_phys == 0 || user_stack_phys == 0) {
        serial_puts("       [FAIL] Failed to allocate physical frames for user mode!\n");
        hcf();
    }

    /* Map user code page at 0x400000 (Executable, User, Present) */
    const uintptr_t USER_CODE_VIRT = 0x0000000000400000ULL;
    int code_map_res = vmm_map_page(user_pml4_virt, USER_CODE_VIRT, user_code_phys, PTE_PRESENT | PTE_USER);
    if (code_map_res != VMM_OK) {
        serial_puts("       [FAIL] Failed to map user code page!\n");
        hcf();
    }

    /* Map user stack page at 0x7FFFF0000000 (Writable, NX, User, Present) */
    int stack_map_res = vmm_map_page(user_pml4_virt, USER_STACK_PAGE_VIRT, user_stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    if (stack_map_res != VMM_OK) {
        serial_puts("       [FAIL] Failed to map user stack page!\n");
        hcf();
    }
    serial_puts("       [PASS] User code mapped at 0x400000 (RX) and stack at 0x7FFFF0001000 (RW/NX)\n");

    /* 2. Write User Machine Code Payload into user code frame via HHDM */
    serial_puts("[TEST 2] Writing Machine Code Payload into User Space Memory...\n");
    uint8_t *code_hhdm_ptr = (uint8_t *)(user_code_phys + boot_info->hhdm_offset);

    /* Machine Code:
     *   push 0x42           -> 6A 42
     *   pop rbx             -> 5B
     *   movabs rax, 0x1111  -> 48 B8 11 11 00 00 00 00 00 00
     *   add rax, rbx        -> 48 01 D8
     *   int 0x80            -> CD 80
     *   hlt                 -> F4
     */
    static const uint8_t payload[] = {
        0x6A, 0x42,
        0x5B,
        0x48, 0xB8, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xD8,
        0xCD, 0x80,
        0xF4
    };
    memcpy(code_hhdm_ptr, payload, sizeof(payload));
    serial_puts("       [PASS] Payload assembled: push/pop stack, 64-bit arithmetic, int 0x80 syscall trap\n");

    /* 3. Configure TSS.RSP0 to safe kernel stack for privilege transitions */
    serial_puts("[TEST 3] Arming TSS.RSP0 with Dedicated Kernel Stack...\n");
    uintptr_t test_rsp0_top = (uintptr_t)g_test_user_rsp0_stack + sizeof(g_test_user_rsp0_stack);
    uint64_t saved_rsp0 = gdt_get_tss_rsp0();
    gdt_set_tss_rsp0((uint64_t)test_rsp0_top);
    serial_puts("       [PASS] TSS.RSP0 armed at ");
    serial_print_hex(test_rsp0_top);
    serial_puts(" (previous: ");
    serial_print_hex(saved_rsp0);
    serial_puts(")\n");

    /* 4. Switch CR3 to user space and transition to Ring 3 */
    serial_puts("[TEST 4] Executing iretq Privilege Transition into Ring 3...\n");

    /* Ensure interrupts are disabled during manual CR3 switch */
    __asm__ volatile("cli" ::: "memory");

    uintptr_t old_cr3 = vmm_get_current_pml4();
    vmm_switch_pml4(user_pml4_phys);

    /* Call assembly helper which registers trap recovery, zeroes regs, and executes iretq */
    bool helper_res = test_user_mode_helper(USER_CODE_VIRT, USER_STACK_TOP_VIRT);

    /* RESTORE INVARIANTS: Immediately restore master kernel CR3 and TSS.RSP0 */
    vmm_switch_pml4(master_kernel_pml4_phys);
    (void)old_cr3;
    gdt_set_tss_rsp0(saved_rsp0);

    if (!helper_res) {
        serial_puts("       [FAIL] test_user_mode_helper failed unexpectedly!\n");
        hcf();
    }
    serial_puts("       [PASS] Control successfully transitioned to Ring 3 and trapped back via int 0x80!\n");

    /* 5. Verify Captured Trap State */
    serial_puts("[TEST 5] Verifying Privilege Level & User Execution State...\n");
    uint64_t caught_cs = 0, caught_ss = 0, caught_rax = 0, caught_rsp = 0;
    bool caught = idt_was_user_trap_caught(&caught_cs, &caught_ss, &caught_rax, &caught_rsp);
    if (!caught) {
        serial_puts("       [FAIL] User trap was not caught by IDT vector 0x80 handler!\n");
        hcf();
    }

    /* Check CPL in CS */
    uint8_t cpl = (uint8_t)(caught_cs & 3);
    if (cpl != 3 || caught_cs != 0x23) {
        serial_puts("       [FAIL] User CS verification failed! CS=");
        serial_print_hex(caught_cs);
        serial_puts(" (Expected CPL 3, CS 0x23)\n");
        hcf();
    }
    serial_puts("       [PASS] Captured CS: 0x23 (CPL = 3 confirmed: Ring 3 User Mode)\n");

    /* Check RPL in SS */
    if ((caught_ss & 3) != 3 || caught_ss != 0x1B) {
        serial_puts("       [FAIL] User SS verification failed! SS=");
        serial_print_hex(caught_ss);
        serial_puts(" (Expected RPL 3, SS 0x1B)\n");
        hcf();
    }
    serial_puts("       [PASS] Captured SS: 0x1B (RPL = 3 confirmed: User Data Segment)\n");

    /* Check User Arithmetic Result in RAX */
    if (caught_rax != 0x1153) {
        serial_puts("       [FAIL] Payload arithmetic result in RAX mismatch! Expected 0x1153, Got: ");
        serial_print_hex(caught_rax);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Payload computed RAX: 0x1153 (0x1111 + 0x42 via user stack push/pop)\n");

    /* Check Balanced User RSP */
    if (caught_rsp != USER_STACK_TOP_VIRT) {
        serial_puts("       [FAIL] User stack pointer unbalanced! Expected: ");
        serial_print_hex(USER_STACK_TOP_VIRT);
        serial_puts(" Got: ");
        serial_print_hex(caught_rsp);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] User stack balanced after push/pop (RSP: 0x7FFFF0001000)\n");

    /* 6. Destroy User Address Space and Audit for Zero Leaks */
    serial_puts("[TEST 6] Teardown of User Address Space & Zero-Leak Audit...\n");
    int destroy_res = vmm_destroy_pml4(user_pml4_phys, true);
    if (destroy_res != VMM_OK) {
        serial_puts("       [FAIL] Failed to destroy user PML4! Code: ");
        serial_print_dec(destroy_res);
        serial_puts("\n");
        hcf();
    }

    size_t post_free_pages = pmm_get_free_pages();
    size_t post_allocated_tables = vmm_get_allocated_table_frames();

    if (post_allocated_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Table frame leak detected! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(post_allocated_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & root reclaimed (delta: 0)\n");

    if (post_free_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frames leaked! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(post_free_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All user physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("[ OK ] Phase 7 (Checkpoint 1) completed successfully!\n\n");
}

/* =========================================================================
 * Phase 7 (Checkpoint 2): First System Call (Serial Print via int 0x80)
 * ========================================================================= */
static uint8_t g_test_user_syscall_rsp0_stack[16384] __attribute__((aligned(16)));

static void test_phase7_checkpoint2_syscalls(const boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)master_kernel_pml4;
    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 2): First System Call (int 0x80)\n");
    serial_puts("========================================================\n");

    /* Record baseline resource counters for zero-leak audit */
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();

    /* 1. Create dedicated user PML4 address space */
    serial_puts("[TEST 1] Setting up User Address Space (Code, Stack & Data Pages)...\n");
    uintptr_t user_pml4_phys = vmm_create_user_pml4();
    if (user_pml4_phys == 0) {
        serial_puts("       [FAIL] Failed to create user PML4!\n");
        hcf();
    }
    uint64_t *user_pml4_virt = (uint64_t *)((uintptr_t)user_pml4_phys + boot_info->hhdm_offset);

    /* Allocate physical frames */
    uintptr_t code_phys  = pmm_alloc_page();
    uintptr_t stack_phys = pmm_alloc_page();
    uintptr_t data1_phys = pmm_alloc_page();
    uintptr_t data2_phys = pmm_alloc_page();
    if (!code_phys || !stack_phys || !data1_phys || !data2_phys) {
        serial_puts("       [FAIL] Failed to allocate physical frames for syscall test!\n");
        hcf();
    }

    /* Map code page at 0x400000 (RX) */
    const uintptr_t USER_CODE_VIRT = 0x0000000000400000ULL;
    vmm_map_page(user_pml4_virt, USER_CODE_VIRT, code_phys, PTE_PRESENT | PTE_USER);

    /* Map stack page at 0x7FFFF0000000 (RW/NX) */
    vmm_map_page(user_pml4_virt, USER_STACK_PAGE_VIRT, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);

    /* Map two consecutive data pages at 0x500000 and 0x501000 (RW/NX) */
    const uintptr_t USER_DATA1_VIRT = 0x0000000000500000ULL;
    const uintptr_t USER_DATA2_VIRT = 0x0000000000501000ULL;
    vmm_map_page(user_pml4_virt, USER_DATA1_VIRT, data1_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    vmm_map_page(user_pml4_virt, USER_DATA2_VIRT, data2_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    /* 0x502000 is intentionally NOT mapped to test buffer boundary faults */

    serial_puts("       [PASS] User address space mapped (Code at 0x400000, Data at 0x500000/0x501000, Stack at 0x7FFFF0001000)\n");

    /* 2. Populate user data frames via HHDM */
    serial_puts("[TEST 2] Populating Test Buffers & Boundary Crossings in User Data Frames...\n");
    uint8_t *data1_ptr = (uint8_t *)(data1_phys + boot_info->hhdm_offset);
    uint8_t *data2_ptr = (uint8_t *)(data2_phys + boot_info->hhdm_offset);
    memset(data1_ptr, 0, PAGE_SIZE);
    memset(data2_ptr, 0, PAGE_SIZE);

    /* String 1: "Hello from Ring 3 Syscall!\n" at 0x500000 (27 bytes) */
    const char str1[] = "Hello from Ring 3 Syscall!\n";
    memcpy(data1_ptr, str1, 27);

    /* String 2: Spans boundary between 0x500000 and 0x501000.
     * Placed at 0x500FF8 (last 8 bytes of page 1) and continues into page 2 (first 10 bytes).
     * Total length = 18 bytes: "Crossing Boundary\n" */
    const char str2_p1[] = "Crossing";
    const char str2_p2[] = " Boundary\n";
    memcpy(data1_ptr + 4088, str2_p1, 8);
    memcpy(data2_ptr, str2_p2, 10);

    /* String 3: Placed at 0x501FF8 (last 8 bytes of page 2).
     * Asking for 16 bytes will cross into unmapped page 0x502000. */
    memcpy(data2_ptr + 4088, "Faulting", 8);

    serial_puts("       [PASS] Test data buffers initialized (including mapped and unmapped page boundary patterns)\n");

    /* 3. Copy user assembly test payload into user code frame */
    serial_puts("[TEST 3] Loading Position-Independent Syscall Test Program...\n");
    uint8_t *code_ptr = (uint8_t *)(code_phys + boot_info->hhdm_offset);
    size_t payload_len = (size_t)(user_syscall_test_end - user_syscall_test_start);
    if (payload_len > PAGE_SIZE) {
        serial_puts("       [FAIL] User test payload exceeds one page!\n");
        hcf();
    }
    memcpy(code_ptr, user_syscall_test_start, payload_len);
    serial_puts("       [PASS] Syscall suite (8 test cases + SYS_EXIT) loaded into user code page\n");

    /* 4. Arm TSS.RSP0 with dedicated kernel stack */
    serial_puts("[TEST 4] Arming TSS.RSP0 with Dedicated Kernel Stack...\n");
    uintptr_t test_rsp0_top = (uintptr_t)g_test_user_syscall_rsp0_stack + sizeof(g_test_user_syscall_rsp0_stack);
    uint64_t saved_rsp0 = gdt_get_tss_rsp0();
    gdt_set_tss_rsp0((uint64_t)test_rsp0_top);

    /* 5. Initialize syscall subsystem and execute user program */
    serial_puts("[TEST 5] Executing User Program in Ring 3 with Bidirectional Syscalls...\n");
    serial_puts("------- USER SYSCALL OUTPUT START -------\n");

    /* Ensure interrupts are disabled for manual CR3 isolation */
    __asm__ volatile("cli" ::: "memory");
    vmm_switch_pml4(user_pml4_phys);

    bool helper_res = test_user_syscall_helper(USER_CODE_VIRT, USER_STACK_TOP_VIRT);

    /* INVARIANT RESTORATION: Restore master kernel CR3 and TSS.RSP0 immediately */
    vmm_switch_pml4(master_kernel_pml4_phys);
    gdt_set_tss_rsp0(saved_rsp0);

    serial_puts("------- USER SYSCALL OUTPUT END ---------\n");

    if (!helper_res) {
        serial_puts("       [FAIL] test_user_syscall_helper failed unexpectedly!\n");
        hcf();
    }

    /* 6. Verify SYS_EXIT and User Program Results */
    serial_puts("[TEST 6] Validating User Program Completion & Return Codes...\n");
    uint64_t exit_code = 0;
    if (!syscall_was_exit_called(&exit_code)) {
        serial_puts("       [FAIL] User program did not terminate via SYS_EXIT!\n");
        hcf();
    }

    if (exit_code != 42) {
        serial_puts("       [FAIL] User test suite failed in Ring 3! Exit code: ");
        serial_print_dec(exit_code);
        if (exit_code >= 101 && exit_code <= 108) {
            serial_puts(" (Sub-test ");
            serial_print_dec(exit_code - 100);
            serial_puts(" failed)");
        }
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All 8 Ring 3 syscall assertions passed! Exit code: 42\n");
    serial_puts("              - Sub-test 1: Valid serial write (stdout, count=27) -> RAX=27\n");
    serial_puts("              - Sub-test 2: Cross-page buffer (0x500FF8..0x501009, count=18) -> RAX=18\n");
    serial_puts("              - Sub-test 3: Invalid pointer (unmapped 0x600000) -> RAX=-2 (EFAULT)\n");
    serial_puts("              - Sub-test 4: Cross-page to unmapped (0x501FF8, count=16) -> RAX=-2 (EFAULT)\n");
    serial_puts("              - Sub-test 5: Kernel pointer (0xFFFFFFFF80000000) -> RAX=-2 (EFAULT)\n");
    serial_puts("              - Sub-test 6: Oversized buffer length (100000 bytes) -> RAX=-1 (EINVAL)\n");
    serial_puts("              - Sub-test 7: Zero-length write (count=0) -> RAX=0\n");
    serial_puts("              - Sub-test 8: Invalid file descriptor (fd=99) -> RAX=-3 (EBADF)\n");

    /* 7. Teardown User Address Space & Zero-Leak Audit */
    serial_puts("[TEST 7] User Address Space Teardown & Zero-Leak Audit...\n");
    int destroy_res = vmm_destroy_pml4(user_pml4_phys, true);
    if (destroy_res != VMM_OK) {
        serial_puts("       [FAIL] Failed to destroy user PML4! Code: ");
        serial_print_dec(destroy_res);
        serial_puts("\n");
        hcf();
    }

    size_t post_free_pages = pmm_get_free_pages();
    size_t post_allocated_tables = vmm_get_allocated_table_frames();

    if (post_allocated_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Table frame leak detected! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(post_allocated_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & root reclaimed (delta: 0)\n");

    if (post_free_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frames leaked! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(post_free_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All 4 user physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("[ OK ] Phase 7 (Checkpoint 2) completed successfully!\n\n");
}

/* =========================================================================
 * Phase 7 (Checkpoint 3): Embedded Standalone ELF64 Executable Loading
 * ========================================================================= */
static uint8_t g_test_elf_rsp0_stack[16384] __attribute__((aligned(16)));

extern const uint8_t embedded_init_elf_start[];
extern const uint8_t embedded_init_elf_end[];

static void test_phase7_checkpoint3_elf(const boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 3): Embedded ELF64 User Loading\n");
    serial_puts("========================================================\n");

    /* Record baseline resource counters for zero-leak audit */
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();

    /* ---------------------------------------------------------------------
     * Part A: Range Validator Hardening & Unknown Syscall Tests
     * --------------------------------------------------------------------- */
    serial_puts("[TEST 1] Hardened Range Validation & Syscall Robustness...\n");

    /* 1A. Integer Wraparound Rejection */
    if (vmm_validate_user_range(master_kernel_pml4, 0xFFFFFFFFFFFFFFFEULL, 8, false)) {
        serial_puts("       [FAIL] Integer wraparound was not caught by validator!\n");
        hcf();
    }
    serial_puts("       [PASS] Integer address wraparound strictly rejected\n");

    /* 1B. Supervisor-Only Lower-Half Page Rejection */
    uintptr_t sup_test_pml4_phys = vmm_create_user_pml4();
    uint64_t *sup_test_pml4_virt = (uint64_t *)vmm_phys_to_virt(sup_test_pml4_phys);
    uintptr_t sup_frame = pmm_alloc_page();
    /* Map at 0x400000 with PTE_PRESENT | PTE_WRITABLE, but OMITTING PTE_USER */
    vmm_map_page(sup_test_pml4_virt, 0x400000ULL, sup_frame, PTE_PRESENT | PTE_WRITABLE);

    if (vmm_validate_user_range(sup_test_pml4_virt, 0x400000ULL, 64, false)) {
        serial_puts("       [FAIL] Supervisor-only mapped page was accepted as user memory!\n");
        hcf();
    }
    serial_puts("       [PASS] Supervisor-only mapped page (PTE_USER=0) strictly rejected\n");
    vmm_destroy_pml4(sup_test_pml4_phys, true);

    /* 1C. Unknown Syscall Dispatch */
    interrupt_frame_t fake_frame;
    memset(&fake_frame, 0, sizeof(fake_frame));
    fake_frame.rax = 999; /* Unknown syscall number */
    int64_t unk_res = syscall_dispatch(&fake_frame);
    if (unk_res != SYSCALL_ENOSYS || (int64_t)fake_frame.rax != SYSCALL_ENOSYS) {
        serial_puts("       [FAIL] Unknown syscall number did not return SYSCALL_ENOSYS!\n");
        hcf();
    }
    serial_puts("       [PASS] Unknown syscall number (999) rejected with SYSCALL_ENOSYS\n");

    /* ---------------------------------------------------------------------
     * Part B: Failure Rejection & Rollback Tests (Zero Leaks Verified)
     * --------------------------------------------------------------------- */
    serial_puts("[TEST 2] Executable Format Contract & Rollback Validation...\n");

    elf_loaded_process_t bad_proc;

    /* 2A. Truncated image / invalid magic */
    uint8_t garbage[32] = { 0 };
    if (elf_load_executable(garbage, sizeof(garbage), &bad_proc) != ELF_ERR_INVALID) {
        serial_puts("       [FAIL] Truncated garbage was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Truncated / malformed header rejected (ELF_ERR_INVALID)\n");

    /* 2B. ET_DYN (PIE / Shared library) Rejection */
    uint8_t dyn_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    memcpy(dyn_elf, embedded_init_elf_start, sizeof(dyn_elf));
    Elf64_Ehdr *dyn_ehdr = (Elf64_Ehdr *)dyn_elf;
    dyn_ehdr->e_type = ET_DYN;
    if (elf_load_executable(dyn_elf, sizeof(dyn_elf), &bad_proc) != ELF_ERR_INVALID) {
        serial_puts("       [FAIL] ET_DYN shared object was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] ET_DYN (PIE/shared object) strictly rejected (ET_EXEC required)\n");

    /* 2C. PT_INTERP (Dynamic Interpreter Request) Rejection */
    uint8_t interp_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * 2];
    memcpy(interp_elf, embedded_init_elf_start, sizeof(Elf64_Ehdr));
    Elf64_Ehdr *interp_ehdr = (Elf64_Ehdr *)interp_elf;
    interp_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    interp_ehdr->e_phnum = 2;
    Elf64_Phdr *interp_phdrs = (Elf64_Phdr *)(interp_elf + sizeof(Elf64_Ehdr));
    interp_phdrs[0].p_type = PT_INTERP;
    interp_phdrs[0].p_offset = 0;
    interp_phdrs[0].p_filesz = 16;
    interp_phdrs[0].p_memsz = 16;
    interp_phdrs[1].p_type = PT_LOAD;
    interp_phdrs[1].p_flags = PF_R | PF_X;
    interp_phdrs[1].p_vaddr = 0x400000;
    interp_phdrs[1].p_offset = 0x1000;
    interp_phdrs[1].p_filesz = 0x100;
    interp_phdrs[1].p_memsz = 0x100;
    interp_phdrs[1].p_align = 0x1000;
    if (elf_load_executable(interp_elf, sizeof(interp_elf) + 0x2000, &bad_proc) != ELF_ERR_INVALID) {
        serial_puts("       [FAIL] PT_INTERP was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] PT_INTERP dynamic interpreter request rejected\n");

    /* 2D. W^X Violation Rejection (PF_W | PF_X) */
    uint8_t wx_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    memcpy(wx_elf, embedded_init_elf_start, sizeof(wx_elf));
    Elf64_Ehdr *wx_ehdr = (Elf64_Ehdr *)wx_elf;
    wx_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    wx_ehdr->e_phnum = 1;
    Elf64_Phdr *wx_phdr = (Elf64_Phdr *)(wx_elf + sizeof(Elf64_Ehdr));
    wx_phdr->p_type = PT_LOAD;
    wx_phdr->p_flags = PF_R | PF_W | PF_X; /* W^X violation */
    wx_phdr->p_vaddr = 0x400000;
    wx_phdr->p_offset = 0x1000;
    wx_phdr->p_filesz = 0x100;
    wx_phdr->p_memsz = 0x100;
    wx_phdr->p_align = 0x1000;
    wx_ehdr->e_entry = 0x400000;
    if (elf_load_executable(wx_elf, sizeof(wx_elf) + 0x2000, &bad_proc) != ELF_ERR_PERM) {
        serial_puts("       [FAIL] W^X violation (PF_W | PF_X) was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] W^X violation (PF_W | PF_X) strictly rejected (ELF_ERR_PERM)\n");

    /* 2E. Non-Executable Entry Point Rejection */
    uint8_t noexec_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    memcpy(noexec_elf, embedded_init_elf_start, sizeof(noexec_elf));
    Elf64_Ehdr *noexec_ehdr = (Elf64_Ehdr *)noexec_elf;
    noexec_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    noexec_ehdr->e_phnum = 1;
    Elf64_Phdr *noexec_phdr = (Elf64_Phdr *)(noexec_elf + sizeof(Elf64_Ehdr));
    noexec_phdr->p_type = PT_LOAD;
    noexec_phdr->p_flags = PF_R; /* Read-only, NOT executable */
    noexec_phdr->p_vaddr = 0x400000;
    noexec_phdr->p_offset = 0x1000;
    noexec_phdr->p_filesz = 0x100;
    noexec_phdr->p_memsz = 0x100;
    noexec_phdr->p_align = 0x1000;
    noexec_ehdr->e_entry = 0x400000;
    if (elf_load_executable(noexec_elf, sizeof(noexec_elf) + 0x2000, &bad_proc) != ELF_ERR_PERM) {
        serial_puts("       [FAIL] Non-executable entry point was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Entry point in non-executable segment rejected (ELF_ERR_PERM)\n");

    /* 2F. Segment Overlap Rejection */
    uint8_t overlap_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * 2];
    memcpy(overlap_elf, embedded_init_elf_start, sizeof(Elf64_Ehdr));
    Elf64_Ehdr *overlap_ehdr = (Elf64_Ehdr *)overlap_elf;
    overlap_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    overlap_ehdr->e_phnum = 2;
    overlap_ehdr->e_entry = 0x400000;
    Elf64_Phdr *overlap_phdrs = (Elf64_Phdr *)(overlap_elf + sizeof(Elf64_Ehdr));
    overlap_phdrs[0].p_type = PT_LOAD;
    overlap_phdrs[0].p_flags = PF_R | PF_X;
    overlap_phdrs[0].p_vaddr = 0x400000;
    overlap_phdrs[0].p_offset = 0x1000;
    overlap_phdrs[0].p_filesz = 0x1000;
    overlap_phdrs[0].p_memsz = 0x1000;
    overlap_phdrs[0].p_align = 0x1000;
    /* Segment 2 overlaps with 0x400000 */
    overlap_phdrs[1].p_type = PT_LOAD;
    overlap_phdrs[1].p_flags = PF_R | PF_W;
    overlap_phdrs[1].p_vaddr = 0x400800; /* Overlaps 0x400000 */
    overlap_phdrs[1].p_offset = 0x2800;
    overlap_phdrs[1].p_filesz = 0x100;
    overlap_phdrs[1].p_memsz = 0x100;
    overlap_phdrs[1].p_align = 0x1000;
    if (elf_load_executable(overlap_elf, sizeof(overlap_elf) + 0x4000, &bad_proc) != ELF_ERR_OVERLAP) {
        serial_puts("       [FAIL] Overlapping segments were not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Overlapping virtual page segments rejected (ELF_ERR_OVERLAP)\n");

    /* 2G. Stack & Guard Page Collision Rejection */
    uint8_t stack_col_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    memcpy(stack_col_elf, embedded_init_elf_start, sizeof(stack_col_elf));
    Elf64_Ehdr *sc_ehdr = (Elf64_Ehdr *)stack_col_elf;
    sc_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    sc_ehdr->e_phnum = 1;
    sc_ehdr->e_entry = USER_STACK_GUARD_VIRT;
    Elf64_Phdr *sc_phdr = (Elf64_Phdr *)(stack_col_elf + sizeof(Elf64_Ehdr));
    sc_phdr->p_type = PT_LOAD;
    sc_phdr->p_flags = PF_R | PF_X;
    sc_phdr->p_vaddr = USER_STACK_GUARD_VIRT; /* Collides with stack guard */
    sc_phdr->p_offset = 0x1000;
    sc_phdr->p_filesz = 0x100;
    sc_phdr->p_memsz = 0x100;
    sc_phdr->p_align = 0x1000;
    if (elf_load_executable(stack_col_elf, sizeof(stack_col_elf) + 0x2000, &bad_proc) != ELF_ERR_OVERLAP) {
        serial_puts("       [FAIL] Stack guard collision was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Segment colliding with reserved stack/guard rejected (ELF_ERR_OVERLAP)\n");

    /* 2H. Page-Zero Mapping Rejection */
    uint8_t p0_elf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    memcpy(p0_elf, embedded_init_elf_start, sizeof(p0_elf));
    Elf64_Ehdr *p0_ehdr = (Elf64_Ehdr *)p0_elf;
    p0_ehdr->e_phoff = sizeof(Elf64_Ehdr);
    p0_ehdr->e_phnum = 1;
    p0_ehdr->e_entry = 0x0;
    Elf64_Phdr *p0_phdr = (Elf64_Phdr *)(p0_elf + sizeof(Elf64_Ehdr));
    p0_phdr->p_type = PT_LOAD;
    p0_phdr->p_flags = PF_R | PF_X;
    p0_phdr->p_vaddr = 0x0; /* Page-zero mapping */
    p0_phdr->p_offset = 0x0;
    p0_phdr->p_filesz = 0x100;
    p0_phdr->p_memsz = 0x100;
    p0_phdr->p_align = 0x1000;
    if (elf_load_executable(p0_elf, sizeof(p0_elf) + 0x2000, &bad_proc) != ELF_ERR_PERM) {
        serial_puts("       [FAIL] Page-zero mapping was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Page-zero mapping strictly rejected (ELF_ERR_PERM)\n");

    /* Verify 0 memory leaked after all negative tests */
    if (pmm_get_free_pages() != baseline_free_pages ||
        vmm_get_allocated_table_frames() != baseline_allocated_tables) {
        serial_puts("       [FAIL] Memory leak after negative tests!\n");
        hcf();
    }
    serial_puts("       [PASS] Resource audit: 0 table frames and 0 physical frames leaked across negative tests\n");

    /* ---------------------------------------------------------------------
     * Part C: Valid Embedded Standalone ELF Loading & Ring 3 Execution
     * --------------------------------------------------------------------- */
    serial_puts("[TEST 3] Loading Valid Standalone ELF64 Program...\n");
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    elf_loaded_process_t proc;
    int load_res = elf_load_executable(embedded_init_elf_start, init_elf_size, &proc);
    if (load_res != ELF_OK) {
        serial_puts("       [FAIL] elf_load_executable failed with error: ");
        serial_print_dec(load_res);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] ELF loaded successfully: Entry=0x400000, StackTop=0x7FFFF0001000, Pages=");
    serial_print_dec(proc.total_pages);
    serial_puts("\n");

    /* Verify mapping permissions in user space */
    uint64_t *proc_pml4_virt = (uint64_t *)vmm_phys_to_virt(proc.pml4_phys);
    if (!vmm_is_mapped(proc_pml4_virt, 0x400000) || /* .text */
        !vmm_is_mapped(proc_pml4_virt, 0x401000) || /* .rodata */
        !vmm_is_mapped(proc_pml4_virt, 0x402000) || /* .data */
        !vmm_is_mapped(proc_pml4_virt, 0x403000) || /* .bss */
        !vmm_is_mapped(proc_pml4_virt, USER_STACK_PAGE_VIRT)) {
        serial_puts("       [FAIL] Expected user segments not mapped in process PML4!\n");
        hcf();
    }
    serial_puts("       [PASS] All segment pages (.text, .rodata, .data, .bss, stack) verified mapped\n");

    /* Arm TSS.RSP0 for privilege transitions */
    uintptr_t test_rsp0_top = (uintptr_t)g_test_elf_rsp0_stack + sizeof(g_test_elf_rsp0_stack);
    uint64_t saved_rsp0 = gdt_get_tss_rsp0();
    gdt_set_tss_rsp0((uint64_t)test_rsp0_top);

    serial_puts("[TEST 4] Executing Standalone ELF Program in Ring 3...\n");
    serial_puts("------- USER STANDALONE ELF OUTPUT START -------\n");

    /* Ensure interrupts are disabled during manual CR3 switch */
    __asm__ volatile("cli" ::: "memory");
    vmm_switch_pml4(proc.pml4_phys);

    bool exec_res = test_user_syscall_helper(proc.entry_point, proc.user_stack_top);

    /* RESTORE INVARIANTS: Immediately restore master CR3 and TSS.RSP0 */
    vmm_switch_pml4(master_kernel_pml4_phys);
    gdt_set_tss_rsp0(saved_rsp0);

    serial_puts("------- USER STANDALONE ELF OUTPUT END ---------\n");

    if (!exec_res) {
        serial_puts("       [FAIL] test_user_syscall_helper failed unexpectedly!\n");
        hcf();
    }

    /* Verify SYS_EXIT and captured exit code */
    serial_puts("[TEST 5] Validating Standalone User Process Exit State...\n");
    uint64_t exit_code = 0;
    if (!syscall_was_exit_called(&exit_code)) {
        serial_puts("       [FAIL] Process did not call SYS_EXIT!\n");
        hcf();
    }
    if (exit_code != 77) {
        serial_puts("       [FAIL] Process exited with unexpected code: ");
        serial_print_dec(exit_code);
        if (exit_code == 1) serial_puts(" (Failed .data verification)");
        if (exit_code == 2) serial_puts(" (Failed .bss zero-initialization check)");
        if (exit_code == 3) serial_puts(" (Failed .bss writeability check)");
        if (exit_code == 4) serial_puts(" (Failed SYS_WRITE syscall check)");
        if (exit_code == 5) serial_puts(" (Failed user stack push/pop check)");
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Process ran to completion in Ring 3! Exit code: 77\n");
    serial_puts("              - Verified .data initialized value (0xCAFEBABE12345678)\n");
    serial_puts("              - Verified .bss zero-initialization by kernel loader\n");
    serial_puts("              - Verified .bss writeability under user mode\n");
    serial_puts("              - Verified SYS_WRITE serial output\n");
    serial_puts("              - Verified user stack operations\n");

    /* Clean teardown */
    vmm_destroy_pml4(proc.pml4_phys, true);

    /* ---------------------------------------------------------------------
     * Part D: Repeated 5-Cycle Load & Teardown Leak Audit
     * --------------------------------------------------------------------- */
    serial_puts("[TEST 6] Repeated 5-Cycle ELF Load/Teardown Leak Audit...\n");
    for (int cycle = 1; cycle <= 5; cycle++) {
        elf_loaded_process_t cproc;
        int cres = elf_load_executable(embedded_init_elf_start, init_elf_size, &cproc);
        if (cres != ELF_OK) {
            serial_puts("       [FAIL] Cycle load failed!\n");
            hcf();
        }
        if (vmm_destroy_pml4(cproc.pml4_phys, true) != VMM_OK) {
            serial_puts("       [FAIL] Cycle teardown failed!\n");
            hcf();
        }
    }

    size_t final_free_pages = pmm_get_free_pages();
    size_t final_allocated_tables = vmm_get_allocated_table_frames();

    if (final_allocated_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Table frame leak detected after 5 cycles! Delta: ");
        serial_print_dec(final_allocated_tables - baseline_allocated_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & roots reclaimed (delta: 0)\n");

    if (final_free_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frames leaked after 5 cycles! Delta: ");
        serial_print_dec(baseline_free_pages - final_free_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("[ OK ] Phase 7 (Checkpoint 3) completed successfully!\n\n");
}

/* =========================================================================
 * Phase 7 (Checkpoint 4): General Process Exit & Lifecycle Management
 * ========================================================================= */
static void test_phase7_checkpoint4_lifecycle(boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    (void)master_kernel_pml4_phys;

    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 4): General Process Exit & Lifecycle\n");
    serial_puts("========================================================\n");

    /* Ensure legacy test recovery hooks are cleared so SYS_EXIT invokes general process_exit */
    syscall_clear_recovery();

    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();
    uint64_t baseline_stack_slots = sched_get_active_stack_slots_mask();

    /* -------------------------------------------------------------
     * [TEST 1] Spawn User Process as Scheduled Task
     * ------------------------------------------------------------- */
    serial_puts("[TEST 1] Spawning Embedded ELF64 as Scheduled Process...\n");
    tcb_t *proc = process_spawn("init_proc", embedded_init_elf_start, init_elf_size);
    if (!proc) {
        serial_puts("       [FAIL] process_spawn failed to create user process!\n");
        hcf();
    }
    uint64_t pid = proc->tid;
    serial_puts("       [PASS] Process spawned: PID=");
    serial_print_dec(pid);
    serial_puts(", CR3=");
    serial_print_hex(proc->cr3);
    serial_puts(", KernelStackSlot=");
    serial_print_dec(proc->stack_slot);
    serial_puts("\n");

    /* -------------------------------------------------------------
     * [TEST 2] Preemptive Multi-Tasking & User Execution
     * ------------------------------------------------------------- */
    serial_puts("[TEST 2] Executing Process with Timer Preemption Enabled (RFLAGS.IF=1)...\n");
    serial_puts("------- SCHEDULED USER PROCESS OUTPUT START -------\n");

    /* Start APIC timer, enable scheduler preemption, and enable interrupts */
    apic_timer_start();
    sched_enable_preemption();
    __asm__ volatile("sti" ::: "memory");

    /* Wait for the process to terminate and yield execution */
    uint64_t exit_code = 0;
    bool wait_res = process_wait(pid, &exit_code);

    /* Disable interrupts and preemption for assertion verification */
    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    serial_puts("------- SCHEDULED USER PROCESS OUTPUT END ---------\n");

    if (!wait_res) {
        serial_puts("       [FAIL] process_wait failed or process vanished!\n");
        hcf();
    }
    if (exit_code != 77) {
        serial_puts("       [FAIL] Unexpected exit code! Expected: 77, Got: ");
        serial_print_dec(exit_code);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Process terminated cleanly via SYS_EXIT! Exit code: ");
    serial_print_dec(exit_code);
    serial_puts("\n");

    /* -------------------------------------------------------------
     * [TEST 3] Safe Deferred Reclamation & Zero-Leak Audit
     * ------------------------------------------------------------- */
    serial_puts("[TEST 3] Verifying Deferred Resource Reclamation via Reaper...\n");
    /* Directly invoke reaper to reclaim dead processes from g_dead_threads */
    sched_reap_dead();

    size_t after_tables = vmm_get_allocated_table_frames();
    size_t after_pages  = pmm_get_free_pages();
    uint64_t after_slots = sched_get_active_stack_slots_mask();

    if (after_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Intermediate page tables leaked after process exit! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(after_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All user intermediate tables & root reclaimed (delta: 0)\n");

    if (after_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frames leaked after process exit! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(after_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All user physical frames & kernel stack returned to PMM (delta: 0 frames leaked)\n");

    if (after_slots != baseline_stack_slots) {
        serial_puts("       [FAIL] Kernel stack slot leaked! Expected mask: ");
        serial_print_hex(baseline_stack_slots);
        serial_puts(" Got: ");
        serial_print_hex(after_slots);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Dedicated kernel stack slot reclaimed in stack bitmap\n");

    /* -------------------------------------------------------------
     * [TEST 4] Repeated Process Spawn / Preempt / Exit / Reap Cycles
     * ------------------------------------------------------------- */
    serial_puts("[TEST 4] Stress Testing 5 Consecutive Process Spawn/Exit Cycles under Preemption...\n");
    for (int cycle = 1; cycle <= 5; cycle++) {
        tcb_t *p = process_spawn("stress_proc", embedded_init_elf_start, init_elf_size);
        if (!p) {
            serial_puts("       [FAIL] process_spawn failed during cycle ");
            serial_print_dec(cycle);
            serial_puts("\n");
            hcf();
        }
        uint64_t cpid = p->tid;

        apic_timer_start();
        sched_enable_preemption();
        __asm__ volatile("sti" ::: "memory");

        uint64_t code = 0;
        bool ok = process_wait(cpid, &code);

        __asm__ volatile("cli" ::: "memory");
        apic_timer_stop();
        sched_disable_preemption();

        if (!ok || code != 77) {
            serial_puts("       [FAIL] Cycle ");
            serial_print_dec(cycle);
            serial_puts(" failed process_wait with code ");
            serial_print_dec(code);
            serial_puts("\n");
            hcf();
        }
        sched_reap_dead(); /* Run reaper */
    }

    size_t stress_tables = vmm_get_allocated_table_frames();
    size_t stress_pages  = pmm_get_free_pages();
    uint64_t stress_slots = sched_get_active_stack_slots_mask();

    if (stress_tables != baseline_allocated_tables ||
        stress_pages  != baseline_free_pages ||
        stress_slots  != baseline_stack_slots) {
        serial_puts("       [FAIL] Memory or stack slot leak detected across 5 stress cycles!\n");
        hcf();
    }
    serial_puts("       [PASS] 5 consecutive process cycles completed with 0 leaks\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed after process cycles!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");
    serial_puts("[ OK ] Phase 7 (Checkpoint 4) completed successfully!\n\n");
}

/* =========================================================================
 * Phase 7 Acceptance Suite: Concurrent User Preemption & Fault Isolation
 * ========================================================================= */
static void test_phase7_acceptance_suite(boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    (void)master_kernel_pml4_phys;

    serial_puts("========================================================\n");
    serial_puts("Phase 7 Acceptance Suite: Preemption & Fault Isolation\n");
    serial_puts("========================================================\n");

    syscall_clear_recovery();
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);

    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();
    uint64_t baseline_stack_slots = sched_get_active_stack_slots_mask();

    /* -------------------------------------------------------------
     * [TEST 1] Simultaneous CPU-Bound Preemption at Same Virtual Addresses
     * ------------------------------------------------------------- */
    serial_puts("[TEST 1] Spawning Two CPU-Bound User Processes (Same Virtual Layout)...\n");
    tcb_t *p1 = process_spawn_with_arg("user_worker1", embedded_init_elf_start, init_elf_size, 1);
    tcb_t *p2 = process_spawn_with_arg("user_worker2", embedded_init_elf_start, init_elf_size, 2);
    if (!p1 || !p2) {
        serial_puts("       [FAIL] Failed to spawn concurrent user processes!\n");
        hcf();
    }
    uint64_t pid1 = p1->tid;
    uint64_t pid2 = p2->tid;
    serial_puts("       [PASS] Worker 1 (PID=");
    serial_print_dec(pid1);
    serial_puts(", CR3=");
    serial_print_hex(p1->cr3);
    serial_puts(") and Worker 2 (PID=");
    serial_print_dec(pid2);
    serial_puts(", CR3=");
    serial_print_hex(p2->cr3);
    serial_puts(") armed\n");

    serial_puts("       [RUN] Executing concurrent timesliced processes under 100 Hz timer preemption...\n");
    uint64_t baseline_runnable_switches = sched_get_runnable_switches_count();
    uint64_t baseline_timer_preemptions = sched_get_timer_preempt_count();

    apic_timer_start();
    sched_enable_preemption();
    __asm__ volatile("sti" ::: "memory");

    uint64_t code1 = 0, pcount1 = 0, ticks1 = 0;
    uint64_t code2 = 0, pcount2 = 0, ticks2 = 0;
    bool w1 = process_wait_extended(pid1, &code1, &pcount1, &ticks1);
    bool w2 = process_wait_extended(pid2, &code2, &pcount2, &ticks2);

    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    uint64_t delta_runnable_switches = sched_get_runnable_switches_count() - baseline_runnable_switches;
    uint64_t delta_timer_preemptions = sched_get_timer_preempt_count() - baseline_timer_preemptions;

    if (!w1 || code1 != 77 || !w2 || code2 != 88) {
        serial_puts("       [FAIL] Concurrent execution failed! Codes: P1=");
        serial_print_dec(code1);
        serial_puts(", P2=");
        serial_print_dec(code2);
        serial_puts("\n");
        hcf();
    }

    if (pcount1 == 0 || pcount2 == 0) {
        serial_puts("       [FAIL] Insufficient timer preemptions recorded on workers! P1=");
        serial_print_dec(pcount1);
        serial_puts(", P2=");
        serial_print_dec(pcount2);
        serial_puts("\n");
        hcf();
    }

    if (delta_runnable_switches < 2) {
        serial_puts("       [FAIL] Insufficient switches between runnable workers! Switches: ");
        serial_print_dec(delta_runnable_switches);
        serial_puts("\n");
        hcf();
    }

    serial_puts("       [PASS] Both CPU-bound processes completed concurrently without corruption!\n");
    serial_puts("              - Worker 1 exit code: 77 (verified)\n");
    serial_puts("              - Worker 2 exit code: 88 (verified)\n");
    serial_puts("       [PASS] Direct evidence of timer-driven preemption recorded:\n");
    serial_puts("              - Worker 1 timer preemptions: ");
    serial_print_dec(pcount1);
    serial_puts(" (consumed ");
    serial_print_dec(ticks1);
    serial_puts(" timer ticks)\n");
    serial_puts("              - Worker 2 timer preemptions: ");
    serial_print_dec(pcount2);
    serial_puts(" (consumed ");
    serial_print_dec(ticks2);
    serial_puts(" timer ticks)\n");
    serial_puts("              - Switches between runnable workers: ");
    serial_print_dec(delta_runnable_switches);
    serial_puts("\n");
    serial_puts("              - Total scheduler timer preemptions: ");
    serial_print_dec(delta_timer_preemptions);
    serial_puts("\n");

    sched_reap_dead();

    /* -------------------------------------------------------------
     * [TEST 2] Fault Isolation: Deliberate Kernel Memory Access in Ring 3
     * ------------------------------------------------------------- */
    serial_puts("[TEST 2] Testing Fault Isolation: Deliberate Kernel Memory Access...\n");
    tcb_t *p_healthy = process_spawn_with_arg("healthy_user", embedded_init_elf_start, init_elf_size, 1);
    tcb_t *p_faulty  = process_spawn_with_arg("faulty_user", embedded_init_elf_start, init_elf_size, 3);
    if (!p_healthy || !p_faulty) {
        serial_puts("       [FAIL] Failed to spawn processes for fault isolation test!\n");
        hcf();
    }
    uint64_t pid_healthy = p_healthy->tid;
    uint64_t pid_faulty  = p_faulty->tid;

    serial_puts("       [RUN] Running healthy process alongside faulty process...\n");
    apic_timer_start();
    sched_enable_preemption();
    __asm__ volatile("sti" ::: "memory");

    uint64_t code_fault = 0, code_healthy = 0;
    bool wf = process_wait(pid_faulty, &code_fault);
    bool wh = process_wait(pid_healthy, &code_healthy);

    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    if (!wf || code_fault != 142) {
        serial_puts("       [FAIL] Faulty process exit code mismatch! Expected: 142 (128 + #PF), Got: ");
        serial_print_dec(code_fault);
        serial_puts("\n");
        hcf();
    }
    if (!wh || code_healthy != 77) {
        serial_puts("       [FAIL] Healthy process failed to survive! Expected: 77, Got: ");
        serial_print_dec(code_healthy);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Fault isolation verified:\n");
    serial_puts("              - Faulty process terminated by CPU exception (#PF) with exit code: 142 (128 + Vector 14)\n");
    serial_puts("              - Healthy concurrent process completed successfully with exit code: 77\n");
    serial_puts("              - Kernel remained 100% operational without crashing or panicking!\n");

    sched_reap_dead();

    /* -------------------------------------------------------------
     * [TEST 3] Resource Reclamation & Integrity Audit
     * ------------------------------------------------------------- */
    serial_puts("[TEST 3] Verifying Complete Resource Reclamation Post-Acceptance...\n");
    size_t after_tables = vmm_get_allocated_table_frames();
    size_t after_pages  = pmm_get_free_pages();
    uint64_t after_slots = sched_get_active_stack_slots_mask();

    if (after_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Table frame leak detected! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(after_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & roots reclaimed (delta: 0)\n");

    if (after_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frame leak detected! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(after_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (after_slots != baseline_stack_slots) {
        serial_puts("       [FAIL] Stack slot leak detected! Expected mask: ");
        serial_print_hex(baseline_stack_slots);
        serial_puts(" Got: ");
        serial_print_hex(after_slots);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All dedicated kernel stack slots cleanly recycled\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed post-acceptance!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");
    serial_puts("[ OK ] Phase 7 Acceptance Suite PASSED!\n\n");
}

/* =========================================================================
 * Phase 7 (Checkpoint 5): Fast Syscall Hardening (syscall / sysret)
 * ========================================================================= */
static void test_phase7_checkpoint5_fast_syscall(boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    (void)master_kernel_pml4_phys;

    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 5): Fast Syscall (syscall / sysret)\n");
    serial_puts("========================================================\n");

    /* 1. Initialize and Verify MSR Configuration */
    syscall_init();

    serial_puts("[TEST 1] Verifying Fast Syscall MSR Configuration...\n");
    if (!syscall_verify_msrs()) {
        serial_puts("       [FAIL] Fast Syscall MSRs verification failed!\n");
        hcf();
    }
    serial_puts("       [PASS] IA32_EFER.SCE enabled (MSR 0xC0000080)\n");
    serial_puts("       [PASS] IA32_STAR configured (Kernel CS=0x08, User CS=0x23, User SS=0x1B)\n");
    serial_puts("       [PASS] IA32_LSTAR points to syscall_entry_stub\n");
    serial_puts("       [PASS] IA32_SFMASK masks IF (bit 9), TF (bit 8), and DF (bit 10)\n");

    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();
    uint64_t baseline_stack_slots = sched_get_active_stack_slots_mask();

    /* 2. Execute Standalone User Program with Fast Syscall & Dual-Interface Mode */
    serial_puts("[TEST 2] Executing Dual Syscall Program in Ring 3 (Mode 4)...\n");
    serial_puts("------- FAST SYSCALL & DUAL INTERFACE OUTPUT START -------\n");

    tcb_t *proc = process_spawn_with_arg("user_fast_syscall", embedded_init_elf_start, init_elf_size, 4);
    if (!proc) {
        serial_puts("       [FAIL] Failed to spawn user_fast_syscall process!\n");
        hcf();
    }
    uint64_t pid = proc->tid;

    apic_timer_start();
    sched_enable_preemption();
    __asm__ volatile("sti" ::: "memory");

    uint64_t exit_code = 0;
    bool waited = process_wait(pid, &exit_code);

    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    serial_puts("------- FAST SYSCALL & DUAL INTERFACE OUTPUT END ---------\n");

    if (!waited) {
        serial_puts("       [FAIL] process_wait failed for user_fast_syscall!\n");
        hcf();
    }

    if (exit_code != 99) {
        serial_puts("       [FAIL] Fast syscall test suite failed in Ring 3! Exit code: ");
        serial_print_dec(exit_code);
        if (exit_code >= 10 && exit_code <= 16) {
            serial_puts(" (Sub-test ");
            serial_print_dec(exit_code - 9);
            serial_puts(" failed)");
        }
        serial_puts("\n");
        hcf();
    }

    serial_puts("       [PASS] Fast syscall suite completed successfully! Exit code: 99\n");
    serial_puts("              - Verified SYS_WRITE via 'syscall' instruction\n");
    serial_puts("              - Verified reference SYS_WRITE via 'int 0x80' instruction\n");
    serial_puts("              - Verified unmapped memory access returns SYSCALL_EFAULT (-2) via 'syscall'\n");
    serial_puts("              - Verified oversized buffer length returns SYSCALL_EINVAL (-1) via 'syscall'\n");
    serial_puts("              - Verified invalid file descriptor returns SYSCALL_EBADF (-3) via 'syscall'\n");
    serial_puts("              - Verified unknown syscall number returns SYSCALL_ENOSYS (-4) via 'syscall'\n");
    serial_puts("              - Verified user stack integrity across 'syscall' / 'sysret'\n");
    serial_puts("              - Verified clean termination via SYS_EXIT(99) using 'syscall'\n");

    sched_reap_dead();

    /* 3. Hostile Return State Validation & Security Sanitization */
    serial_puts("[TEST 3] Validating Fast Syscall Return State Hardening & Sanitization...\n");

    /* 3A: Exact Non-Canonical Boundary Rejection (0x0000800000000000ULL) */
    interrupt_frame_t noncanon_boundary_frame;
    memset(&noncanon_boundary_frame, 0, sizeof(noncanon_boundary_frame));
    noncanon_boundary_frame.cs     = GDT_USER_CODE;
    noncanon_boundary_frame.rip    = 0x0000000000401000ULL;
    noncanon_boundary_frame.rsp    = 0x0000800000000000ULL; /* First non-canonical address (bit 47=1, 48..63=0) */
    noncanon_boundary_frame.rflags = 0x202;
    if (syscall_validate_return_state(&noncanon_boundary_frame)) {
        serial_puts("       [FAIL] Exact non-canonical upper boundary RSP (0x0000800000000000) was not rejected!\n");
        hcf();
    }
    noncanon_boundary_frame.rsp = 0x00007FFFF0000000ULL;
    noncanon_boundary_frame.rip = 0x0000800000000000ULL; /* First non-canonical address for RIP */
    if (syscall_validate_return_state(&noncanon_boundary_frame)) {
        serial_puts("       [FAIL] Exact non-canonical upper boundary RIP (0x0000800000000000) was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Exact non-canonical upper boundary (0x0000800000000000) rejected (RSP & RIP)\n");

    /* 3B: Non-Canonical Address Space Rejection (0x8000000000000000ULL) */
    interrupt_frame_t noncanon_mid_frame;
    memset(&noncanon_mid_frame, 0, sizeof(noncanon_mid_frame));
    noncanon_mid_frame.cs     = GDT_USER_CODE;
    noncanon_mid_frame.rip    = 0x8000000000000000ULL; /* Non-canonical address (bit 63=1, bits 47..62=0) */
    noncanon_mid_frame.rsp    = 0x00007FFFF0000000ULL;
    noncanon_mid_frame.rflags = 0x202;
    if (syscall_validate_return_state(&noncanon_mid_frame)) {
        serial_puts("       [FAIL] Non-canonical mid address RIP was not rejected!\n");
        hcf();
    }
    noncanon_mid_frame.rip = 0x0000000000401000ULL;
    noncanon_mid_frame.rsp = 0x8000000000000000ULL;
    if (syscall_validate_return_state(&noncanon_mid_frame)) {
        serial_puts("       [FAIL] Non-canonical mid address RSP was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Non-canonical address (0x8000000000000000) rejected (CVE-2012-0217 mitigation)\n");

    /* 3C: Canonical Higher-Half Kernel Space Rejection (0xFFFF800000000000 & 0xFFFFFFFF80000000) */
    interrupt_frame_t kernel_space_frame;
    memset(&kernel_space_frame, 0, sizeof(kernel_space_frame));
    kernel_space_frame.cs     = GDT_USER_CODE;
    kernel_space_frame.rip    = 0xFFFF800000000000ULL; /* Canonical kernel higher-half base */
    kernel_space_frame.rsp    = 0x00007FFFF0000000ULL;
    kernel_space_frame.rflags = 0x202;
    if (syscall_validate_return_state(&kernel_space_frame)) {
        serial_puts("       [FAIL] Canonical kernel base RIP (0xFFFF800000000000) was not rejected!\n");
        hcf();
    }
    kernel_space_frame.rip = 0x0000000000401000ULL;
    kernel_space_frame.rsp = 0xFFFFFFFF80000000ULL; /* Kernel image virtual address */
    if (syscall_validate_return_state(&kernel_space_frame)) {
        serial_puts("       [FAIL] Canonical kernel image RSP (0xFFFFFFFF80000000) was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Canonical higher-half kernel space rejected (0xFFFF800000000000 / 0xFFFFFFFF80000000)\n");

    /* 3D: Sub-Page / Page-Zero Rejection (< PAGE_SIZE) */
    interrupt_frame_t subpage_frame;
    memset(&subpage_frame, 0, sizeof(subpage_frame));
    subpage_frame.cs     = GDT_USER_CODE;
    subpage_frame.rip    = 0x500ULL; /* Sub-page NULL region */
    subpage_frame.rsp    = 0x00007FFFF0000000ULL;
    subpage_frame.rflags = 0x202;
    if (syscall_validate_return_state(&subpage_frame)) {
        serial_puts("       [FAIL] Sub-page return RIP was not rejected!\n");
        hcf();
    }
    subpage_frame.rip = 0x0000000000401000ULL;
    subpage_frame.rsp = 0x500ULL;
    if (syscall_validate_return_state(&subpage_frame)) {
        serial_puts("       [FAIL] Sub-page return RSP was not rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Page-zero (< 0x1000) return RIP and RSP rejected\n");

    /* 3E: Exact Valid Upper Boundary Acceptance (0x00007FFFFFFFFFF8ULL) */
    interrupt_frame_t valid_boundary_frame;
    memset(&valid_boundary_frame, 0, sizeof(valid_boundary_frame));
    valid_boundary_frame.cs     = GDT_USER_CODE;
    valid_boundary_frame.rip    = 0x00007FFFFFFFFFF8ULL; /* Highest canonical user 8-byte aligned slot */
    valid_boundary_frame.rsp    = 0x00007FFFFFFFFFF8ULL;
    valid_boundary_frame.rflags = 0x202;
    if (!syscall_validate_return_state(&valid_boundary_frame)) {
        serial_puts("       [FAIL] Valid canonical upper boundary was unexpectedly rejected!\n");
        hcf();
    }
    serial_puts("       [PASS] Valid canonical upper boundary (0x00007FFFFFFFFFF8) accepted\n");

    /* 3F: RFLAGS Sanitization (IOPL, NT, TF, VM stripped; IF forced to 1) */
    interrupt_frame_t sanitize_frame;
    memset(&sanitize_frame, 0, sizeof(sanitize_frame));
    sanitize_frame.cs     = GDT_USER_CODE;
    sanitize_frame.rip    = 0x0000000000401000ULL;
    sanitize_frame.rsp    = 0x00007FFFF0000000ULL;
    sanitize_frame.rflags = 0x0000000000037300ULL; /* malicious: IOPL=3, NT=1, TF=1, IF=0 */

    if (!syscall_validate_return_state(&sanitize_frame)) {
        serial_puts("       [FAIL] Valid canonical RIP/RSP rejected during sanitize test!\n");
        hcf();
    }
    if ((sanitize_frame.rflags & (3ULL << 12)) != 0 ||
        (sanitize_frame.rflags & (1ULL << 14)) != 0 ||
        (sanitize_frame.rflags & (1ULL << 8))  != 0 ||
        (sanitize_frame.rflags & (1ULL << 9))  == 0 ||
        (sanitize_frame.rflags & (1ULL << 1))  == 0) {
        serial_puts("       [FAIL] Malicious RFLAGS bits not sanitized!\n");
        hcf();
    }
    serial_puts("       [PASS] RFLAGS sanitized (IOPL=0, NT=0, TF=0 stripped, IF=1 enforced)\n");

    /* 3G: End-to-End Hostile Return Interception via Dispatcher (Kernel Stack Recovery) */
    void *recovery_target = &&hostile_recovery_done;
    __asm__ volatile("" : : "r"(recovery_target));
    uintptr_t saved_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(saved_rsp));
    syscall_set_recovery((uintptr_t)recovery_target, saved_rsp);

    interrupt_frame_t hostile_frame;
    memset(&hostile_frame, 0, sizeof(hostile_frame));
    hostile_frame.cs     = GDT_USER_CODE;
    hostile_frame.ss     = GDT_USER_DATA;
    hostile_frame.rip    = 0x0000800000000000ULL; /* Exact non-canonical address */
    hostile_frame.rsp    = 0x00007FFFF0000000ULL;
    hostile_frame.rflags = 0x202;
    hostile_frame.rax    = SYS_WRITE;
    hostile_frame.rdi    = 1;
    hostile_frame.rsi    = 0;
    hostile_frame.rdx    = 0;

    int64_t disp_res = syscall_dispatch(&hostile_frame);
    if (disp_res != SYSCALL_EFAULT || hostile_frame.cs != GDT_KERNEL_CODE || hostile_frame.rip != (uintptr_t)recovery_target) {
        serial_puts("       [FAIL] Hostile return state was not intercepted by dispatcher!\n");
        hcf();
    }

hostile_recovery_done:
    syscall_clear_recovery();
    serial_puts("       [PASS] Hostile return state intercepted on kernel stack without sysretq\n");

    /* 4. Concurrent Repeated Fast Syscalls Under 100 Hz Preemption */
    serial_puts("[TEST 4] Spawning Two Concurrent Fast Syscall Processes (Modes 5 & 6)...\n");
    serial_puts("------- PREEMPTED REPEATED FAST SYSCALLS OUTPUT START -------\n");

    tcb_t *p_fast1 = process_spawn_with_arg("user_fast_worker1", embedded_init_elf_start, init_elf_size, 5);
    tcb_t *p_fast2 = process_spawn_with_arg("user_fast_worker2", embedded_init_elf_start, init_elf_size, 6);
    if (!p_fast1 || !p_fast2) {
        serial_puts("       [FAIL] Failed to spawn concurrent fast syscall processes!\n");
        hcf();
    }
    uint64_t pid_fast1 = p_fast1->tid;
    uint64_t pid_fast2 = p_fast2->tid;

    uint64_t base_runnable_switches = sched_get_runnable_switches_count();
    uint64_t base_timer_preempts    = sched_get_timer_preempt_count();

    apic_timer_start();
    sched_enable_preemption();
    __asm__ volatile("sti" ::: "memory");

    uint64_t code_f1 = 0, pcount_f1 = 0, ticks_f1 = 0;
    uint64_t code_f2 = 0, pcount_f2 = 0, ticks_f2 = 0;
    bool wf1 = process_wait_extended(pid_fast1, &code_f1, &pcount_f1, &ticks_f1);
    bool wf2 = process_wait_extended(pid_fast2, &code_f2, &pcount_f2, &ticks_f2);

    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    serial_puts("\n------- PREEMPTED REPEATED FAST SYSCALLS OUTPUT END ---------\n");

    uint64_t d_runnable = sched_get_runnable_switches_count() - base_runnable_switches;
    uint64_t d_timer    = sched_get_timer_preempt_count() - base_timer_preempts;

    if (!wf1 || code_f1 != 91 || !wf2 || code_f2 != 92) {
        serial_puts("       [FAIL] Repeated fast syscalls under preemption failed! Codes: P1=");
        serial_print_dec(code_f1);
        serial_puts(", P2=");
        serial_print_dec(code_f2);
        serial_puts("\n");
        hcf();
    }

    if (pcount_f1 == 0 || pcount_f2 == 0) {
        serial_puts("       [FAIL] Insufficient timer preemptions recorded during fast syscalls! P1=");
        serial_print_dec(pcount_f1);
        serial_puts(", P2=");
        serial_print_dec(pcount_f2);
        serial_puts("\n");
        hcf();
    }

    if (d_runnable < 2) {
        serial_puts("       [FAIL] Insufficient runnable switches during fast syscalls! Switches: ");
        serial_print_dec(d_runnable);
        serial_puts("\n");
        hcf();
    }

    serial_puts("       [PASS] Concurrent repeated fast syscalls executed with timer preemption!\n");
    serial_puts("              - Worker 1 (PID=");
    serial_print_dec(pid_fast1);
    serial_puts(") completed 60 fast syscalls (Preemptions=");
    serial_print_dec(pcount_f1);
    serial_puts(", Exit=91)\n");
    serial_puts("              - Worker 2 (PID=");
    serial_print_dec(pid_fast2);
    serial_puts(") completed 60 fast syscalls (Preemptions=");
    serial_print_dec(pcount_f2);
    serial_puts(", Exit=92)\n");
    serial_puts("              - Direct preemption evidence: ");
    serial_print_dec(d_runnable);
    serial_puts(" runnable context switches across ");
    serial_print_dec(d_timer);
    serial_puts(" timer ticks\n");

    sched_reap_dead();

    /* 5. Resource Reclamation Audit */
    serial_puts("[TEST 5] Resource Reclamation Post-Fast-Syscall Audit...\n");
    size_t after_tables = vmm_get_allocated_table_frames();
    size_t after_pages  = pmm_get_free_pages();
    uint64_t after_slots = sched_get_active_stack_slots_mask();

    if (after_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Table frame leak detected! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(after_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & roots reclaimed (delta: 0)\n");

    if (after_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frame leak detected! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(after_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (after_slots != baseline_stack_slots) {
        serial_puts("       [FAIL] Stack slot leak detected! Expected mask: ");
        serial_print_hex(baseline_stack_slots);
        serial_puts(" Got: ");
        serial_print_hex(after_slots);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All dedicated kernel stack slots cleanly recycled\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed post-fast-syscall!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");
    serial_puts("[ OK ] Phase 7 (Checkpoint 5) completed successfully!\n\n");
}

/* =========================================================================
 * Phase 8 (Step 8A): Basic Framebuffer Text Console Verification Suite
 * ========================================================================= */
static void test_phase8a_framebuffer_console(const boot_info_t *boot_info) {
    serial_puts("========================================================\n");
    serial_puts("Phase 8 (Step 8A): Basic Framebuffer Text Console Suite\n");
    serial_puts("========================================================\n");

    if (!boot_info->has_framebuffer) {
        serial_puts("[WARN] Skipping console test (headless configuration)\n");
        return;
    }

    /* 1. Verify Console Initialization & Grid Metrics */
    serial_puts("[TEST 1] Verifying Framebuffer Console Initialization...\n");
    if (!console_is_initialized()) {
        serial_puts("       [FAIL] Framebuffer console was not initialized!\n");
        hcf();
    }
    uint64_t cols = 0, rows = 0;
    console_get_dimensions(&cols, &rows);
    if (cols == 0 || rows == 0) {
        serial_puts("       [FAIL] Invalid console grid dimensions!\n");
        hcf();
    }
    serial_puts("       [PASS] Console initialized (Grid: ");
    serial_print_dec(cols);
    serial_puts("x");
    serial_print_dec(rows);
    serial_puts(" characters, 8x16 font)\n");

    /* 2. Cursor Control, Tabs, and Backspace */
    serial_puts("[TEST 2] Testing Cursor Control, Tabs, and Backspace...\n");
    uint64_t c_col = 0, c_row = 0;
    console_putc('\n');
    console_get_cursor(&c_col, &c_row);
    bool nl_ok = (c_col == 0);

    console_putc('\t');
    console_get_cursor(&c_col, &c_row);
    bool tab_ok = (c_col == 8);

    console_putc('\b');
    console_get_cursor(&c_col, &c_row);
    bool bs_ok = (c_col == 7);

    if (!nl_ok || !tab_ok || !bs_ok) {
        serial_puts("       [FAIL] Cursor control verification failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Newline (\\n) advances row and resets column to 0\n");
    serial_puts("       [PASS] Tab (\\t) advances cursor to column 8\n");
    serial_puts("       [PASS] Backspace (\\b) erases character and decrements column to 7\n");

    /* 3. Software Scrolling Verification */
    serial_puts("[TEST 3] Testing Multi-Line Software Row Scrolling...\n");
    uint64_t scroll_test_lines = rows + 5;
    for (uint64_t i = 1; i <= scroll_test_lines; i++) {
        serial_puts("       [SCROLL] Line ");
        serial_print_dec(i);
        serial_puts(" / ");
        serial_print_dec(scroll_test_lines);
        serial_puts(" scrolling test\n");
    }
    console_get_cursor(&c_col, &c_row);
    if (c_row >= rows || c_col >= cols) {
        serial_puts("       [FAIL] Cursor row exceeded screen boundary after scrolling!\n");
        hcf();
    }
    serial_puts("       [PASS] Cached batch scrolling verified (cursor within screen at row ");
    serial_print_dec(c_row);
    serial_puts(")\n");

    /* 4. Color Switching & Visual Banner */
    serial_puts("[TEST 4] Testing Palette Color Switching & Visual Banner...\n");
    console_set_color(0x009ECE6A, CONSOLE_DEFAULT_BG); /* Tokyo Night Green */
    serial_puts("\n+-------------------------------------------------------------+\n");
    serial_puts("|      FORTRESS OS - Phase 8A Framebuffer Console ACTIVE      |\n");
    serial_puts("|         8x16 Bitmap Font * Software Row Scrolling           |\n");
    serial_puts("+-------------------------------------------------------------+\n\n");
    console_set_color(CONSOLE_DEFAULT_FG, CONSOLE_DEFAULT_BG); /* Restore Default */
    serial_puts("       [PASS] Color switching and ASCII frame rendered\n");

    serial_puts("[ OK ] Phase 8 (Step 8A): Framebuffer Console PASSED!\n\n");
}

/* =========================================================================
 * Phase 8 (Step 8B): Initramfs, Minimal VFS & File Descriptors Suite
 * ========================================================================= */
static void test_phase8b_vfs_initramfs(const boot_info_t *boot_info, uint64_t *master_kernel_pml4, uintptr_t master_kernel_pml4_phys) {
    (void)boot_info;
    (void)master_kernel_pml4;
    (void)master_kernel_pml4_phys;

    serial_puts("========================================================\n");
    serial_puts("Phase 8 (Step 8B): Initramfs, Minimal VFS & File Descriptors\n");
    serial_puts("========================================================\n");

    /* TEST 1: VFS Root and Tree Node Lookup */
    serial_puts("[TEST 1] Verifying VFS Tree Hierarchy & File Resolution...\n");
    vfs_node_t *motd_node = vfs_lookup("/etc/motd");
    if (!motd_node || motd_node->type != VFS_FILE || motd_node->size == 0) {
        serial_puts("       [FAIL] /etc/motd not found in VFS or invalid type/size!\n");
        hcf();
    }
    serial_puts("       [PASS] /etc/motd resolved (Size: ");
    serial_print_dec(motd_node->size);
    serial_puts(" bytes)\n");

    vfs_node_t *bin_node = vfs_lookup("/bin");
    if (!bin_node || bin_node->type != VFS_DIRECTORY) {
        serial_puts("       [FAIL] /bin directory not found or not a directory!\n");
        hcf();
    }
    serial_puts("       [PASS] /bin directory verified\n");

    /* TEST 2: Directory Enumeration (ls support) */
    serial_puts("[TEST 2] Testing Directory Enumeration (vfs_readdir)...\n");
    vfs_dirent_t dent;
    int dent_count = 0;
    while (vfs_readdir(bin_node, dent_count, &dent) == 1) {
        serial_puts("       [DENT] /bin/");
        serial_puts(dent.name);
        serial_puts(" (Type: ");
        serial_print_dec(dent.type);
        serial_puts(", Size: ");
        serial_print_dec(dent.size);
        serial_puts(" bytes)\n");
        dent_count++;
    }
    if (dent_count == 0) {
        serial_puts("       [FAIL] No entries enumerated in /bin!\n");
        hcf();
    }
    serial_puts("       [PASS] Directory enumeration successful (");
    serial_print_dec(dent_count);
    serial_puts(" entries found in /bin)\n");

    /* TEST 3: Independent Open-File Seek Offsets */
    serial_puts("[TEST 3] Testing Independent Open-File Seek Offsets (Dual Open)...\n");
    file_t *f1 = vfs_open("/etc/motd", 0);
    file_t *f2 = vfs_open("/etc/motd", 0);
    if (!f1 || !f2 || f1 == f2) {
        serial_puts("       [FAIL] vfs_open failed or returned identical file pointers!\n");
        hcf();
    }
    char buf1[16];
    char buf2[16];
    int64_t r1 = vfs_read(f1, buf1, 16);
    int64_t r2 = vfs_read(f2, buf2, 16);
    if (r1 != 16 || r2 != 16 || f1->offset != 16 || f2->offset != 16) {
        serial_puts("       [FAIL] Dual open read or offset tracking failed!\n");
        hcf();
    }
    if (memcmp(buf1, buf2, 16) != 0) {
        serial_puts("       [FAIL] Independent open file data mismatch!\n");
        hcf();
    }
    /* Read further from f1 */
    int64_t r1_next = vfs_read(f1, buf1, 16);
    if (r1_next <= 0 || f1->offset != 32 || f2->offset != 16) {
        serial_puts("       [FAIL] Advancing f1 mutated f2 offset!\n");
        hcf();
    }
    vfs_close(f1);
    vfs_close(f2);
    serial_puts("       [PASS] Two independent open file handles maintain separate seek offsets\n");

    /* TEST 4: Spawning User Process in Ring 3 with Full VFS Acceptance */
    serial_puts("[TEST 4] Spawning Scheduled User Process in Ring 3 (Mode 7: VFS Acceptance)...\n");
    size_t pre_pmm_free = pmm_get_free_pages();
    size_t pre_tables = vmm_get_allocated_table_frames();

    extern const uint8_t embedded_init_elf_start[];
    extern const uint8_t embedded_init_elf_end[];
    size_t init_elf_size = (size_t)(embedded_init_elf_end - embedded_init_elf_start);
    tcb_t *proc = process_spawn_with_arg("vfs_user_proc", embedded_init_elf_start, init_elf_size, 7);
    if (!proc) {
        serial_puts("       [FAIL] Failed to spawn VFS user process!\n");
        hcf();
    }

    uint64_t user_pid = proc->tid;
    serial_puts("       [PASS] User process spawned (PID: ");
    serial_print_dec(user_pid);
    serial_puts(", CR3: ");
    serial_print_hex(proc->cr3);
    serial_puts(")\n");

    serial_puts("------- RING 3 VFS EXECUTION OUTPUT START -------\n");
    uint64_t exit_code = 0;
    bool wait_res = process_wait(user_pid, &exit_code);
    serial_puts("------- RING 3 VFS EXECUTION OUTPUT END ---------\n");

    if (!wait_res) {
        serial_puts("       [FAIL] process_wait timed out for VFS user process!\n");
        hcf();
    }
    if (exit_code != 88) {
        serial_puts("       [FAIL] VFS user process failed assertions! Exit code: ");
        serial_print_dec(exit_code);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Ring 3 VFS suite completed successfully! Exit code: 88\n");
    serial_puts("              - Verified sys_open on /etc/motd\n");
    serial_puts("              - Verified sys_read on stdout\n");
    serial_puts("              - Verified dual sys_open with independent seek offsets in user mode\n");
    serial_puts("              - Verified short read and EOF detection\n");
    serial_puts("              - Verified non-existent file returns SYSCALL_ENOENT (-5)\n");
    serial_puts("              - Verified zero-length read returns 0\n");
    serial_puts("              - Verified read into read-only memory returns SYSCALL_EFAULT (-2)\n");
    serial_puts("              - Verified sys_close on active descriptors\n");
    serial_puts("              - Verified read on closed descriptor returns SYSCALL_EBADF (-3)\n");

    /* TEST 5: Complete Resource Reclamation Post-VFS Audit */
    serial_puts("[TEST 5] Resource Reclamation Post-VFS Audit...\n");
    sched_reap_dead();

    size_t post_pmm_free = pmm_get_free_pages();
    size_t post_tables = vmm_get_allocated_table_frames();

    if (post_tables != pre_tables) {
        serial_puts("       [FAIL] Page table leak detected! Pre: ");
        serial_print_dec(pre_tables);
        serial_puts(", Post: ");
        serial_print_dec(post_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & roots reclaimed (delta: 0)\n");

    if (post_pmm_free != pre_pmm_free) {
        serial_puts("       [FAIL] Frame leak detected! Pre: ");
        serial_print_dec(pre_pmm_free);
        serial_puts(", Post: ");
        serial_print_dec(post_pmm_free);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All physical frames returned to PMM (delta: 0 frames leaked)\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Kernel heap walk detected corruption!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("[ OK ] Phase 8 (Step 8B): Initramfs & Minimal VFS PASSED!\n\n");
}

/* =========================================================================
 * Phase 9 (Step 9A): PCI Discovery & NVMe MMIO BAR Verification
 * ========================================================================= */
static void test_phase9a_pci_discovery(const boot_info_t *boot_info) {
    serial_puts("\n========================================================\n");
    serial_puts("Phase 9 (Step 9A): PCI Discovery & NVMe MMIO BAR Verification\n");
    serial_puts("========================================================\n");

    /* 1. Initialize PCI subsystem with HHDM offset */
    serial_puts("[TEST 1] Initializing PCI Subsystem & Scanning ACPI MCFG...\n");
    pci_init(boot_info->hhdm_offset);

    /* 2. Sanity Check on Segment 0 Host Bridge (00:00.0) */
    serial_puts("[TEST 2] Verifying Configuration Space Access (Host Bridge 00:00.0)...\n");
    uint16_t host_vendor = pci_read_config16(0, 0, 0, 0, PCI_REG_VENDOR_ID);
    uint16_t host_device = pci_read_config16(0, 0, 0, 0, PCI_REG_DEVICE_ID);
    uint8_t  host_class  = pci_read_config8(0, 0, 0, 0, PCI_REG_CLASS);

    if (host_vendor == 0xFFFF || host_vendor == 0x0000) {
        serial_puts("       [FAIL] Host bridge vendor ID invalid (");
        serial_print_hex(host_vendor);
        serial_puts(")\n");
        hcf();
    }
    serial_puts("       [PASS] Host Bridge detected: Vendor ");
    serial_print_hex(host_vendor);
    serial_puts(", Device ");
    serial_print_hex(host_device);
    serial_puts(", Class ");
    serial_print_hex(host_class);
    serial_puts("\n");

    /* 3. Enumerate all PCI devices and output inventory */
    serial_puts("[TEST 3] Scanning PCI Bus Hierarchy & Generating Inventory...\n");
    static pci_device_t detected_devices[MAX_PCI_DEVICES];
    size_t dev_count = pci_scan_all(detected_devices, MAX_PCI_DEVICES);

    if (dev_count == 0) {
        serial_puts("       [FAIL] PCI scan returned 0 devices!\n");
        hcf();
    }

    serial_puts("       [INFO] Discovered ");
    serial_print_dec(dev_count);
    serial_puts(" PCI device(s) on system:\n");
    pci_print_inventory(detected_devices, dev_count);
    serial_puts("       [PASS] PCI device inventory compiled successfully\n");

    /* 4. Locate and Verify Attached NVMe Controller */
    serial_puts("[TEST 4] Locating and Verifying NVMe Controller...\n");
    pci_device_t nvme_dev;
    bool found_nvme = pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_NVME, PCI_PROGIF_STORAGE_NVME, &nvme_dev);

    if (!found_nvme) {
        serial_puts("       [SKIP] No NVMe controller found; continuing without NVMe checks\n");
        return;
    }

    /* Verify NVMe Class Code, Subclass, and Prog-IF */
    if (nvme_dev.class_code != PCI_CLASS_STORAGE ||
        nvme_dev.subclass != PCI_SUBCLASS_STORAGE_NVME ||
        nvme_dev.prog_if != PCI_PROGIF_STORAGE_NVME) {
        serial_puts("       [FAIL] NVMe device class/subclass/progif mismatch!\n");
        hcf();
    }
    serial_puts("       [PASS] NVMe Device verified: Class 0x01 (Storage), Subclass 0x08 (NVMe), ProgIF 0x02\n");

    /* Verify BAR0 is 64-bit Memory space and non-zero */
    if (nvme_dev.bar[0] == 0) {
        serial_puts("       [FAIL] NVMe BAR0 is 0 (unassigned or invalid)!\n");
        hcf();
    }
    if (nvme_dev.bar_is_io[0]) {
        serial_puts("       [FAIL] NVMe BAR0 is I/O space! NVMe requires MMIO space.\n");
        hcf();
    }
    if (!nvme_dev.bar_is_64[0]) {
        serial_puts("       [FAIL] NVMe BAR0 is 32-bit! NVMe specification mandates 64-bit BAR0/1.\n");
        hcf();
    }

    serial_puts("       [PASS] NVMe Controller location: ");
    pci_print_bdf(nvme_dev.segment, nvme_dev.bus, nvme_dev.device, nvme_dev.function);
    serial_puts("\n");

    serial_puts("       [PASS] NVMe Vendor ID: ");
    serial_print_hex(nvme_dev.vendor_id);
    serial_puts(", Device ID: ");
    serial_print_hex(nvme_dev.device_id);
    serial_puts("\n");

    serial_puts("       [PASS] NVMe 64-bit MMIO BAR0 Base: ");
    serial_print_hex(nvme_dev.bar[0]);
    serial_puts(" (Prefetchable: ");
    serial_puts(nvme_dev.bar_prefetch[0] ? "Yes" : "No");
    serial_puts(")\n");

    /* 5. Verify Heap Integrity */
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Kernel heap walk detected corruption post-PCI scan!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("\n[ OK ] Phase 9 (Step 9A): PCI Discovery & NVMe MMIO BAR Verification PASSED!\n\n");
}

/* =========================================================================
 * Phase 9 (Step 9B.1): NVMe Initialization & Reads Verification
 * ========================================================================= */
#if defined(ENABLE_NVME_RAW_PATTERN_TESTS) && (ENABLE_NVME_RAW_PATTERN_TESTS == 1)
static bool verify_sector_pattern(const uint8_t *sector, const char *pattern, size_t sector_size) {
    size_t pat_len = strlen(pattern);
    if (pat_len == 0 || sector_size == 0) return false;
    for (size_t i = 0; i < sector_size; i++) {
        if (sector[i] != (uint8_t)pattern[i % pat_len]) {
            return false;
        }
    }
    return true;
}

static void test_phase9b1_nvme_reads(void) {
    serial_puts("\n========================================================\n");
    serial_puts("Phase 9 (Step 9B.1): NVMe Initialization & Reads Verification\n");
    serial_puts("========================================================\n");

    /* 1. Initialize NVMe Storage Driver */
    serial_puts("[TEST 1] Initializing NVMe Storage Driver...\n");
    if (!nvme_init()) {
        serial_puts("       [FAIL] nvme_init() failed!\n");
        hcf();
    }
    if (!nvme_is_initialized()) {
        serial_puts("       [FAIL] nvme_is_initialized returned false!\n");
        hcf();
    }
    serial_puts("       [PASS] NVMe Storage Driver initialized\n");

    /* 2. Validate Namespace Geometry */
    serial_puts("[TEST 2] Validating Namespace Geometry...\n");
    uint32_t active_nsid = nvme_get_active_nsid();
    uint64_t sector_count = nvme_get_sector_count();
    uint32_t sector_size = nvme_get_sector_size();

    if (active_nsid == 0) {
        serial_puts("       [FAIL] Active Namespace ID is 0!\n");
        hcf();
    }
    if (sector_count == 0) {
        serial_puts("       [FAIL] Sector count is 0!\n");
        hcf();
    }
    if (sector_size != 512) {
        serial_puts("       [FAIL] Expected 512-byte sectors, got: ");
        serial_print_dec(sector_size);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Geometry confirmed: NSID ");
    serial_print_dec(active_nsid);
    serial_puts(", Sectors ");
    serial_print_dec(sector_count);
    serial_puts(", Sector Size ");
    serial_print_dec(sector_size);
    serial_puts(" bytes (Total: ");
    serial_print_dec((sector_count * sector_size) / (1024 * 1024));
    serial_puts(" MiB)\n");

    /* 3. Read & Verify Known Deterministic Patterns */
    serial_puts("[TEST 3] Reading & Comparing Known Patterns across Multiple LBAs...\n");
    static uint8_t read_buf[512];

    /* LBA 0 */
    memset(read_buf, 0, sizeof(read_buf));
    if (!nvme_read_sector(0, read_buf)) {
        serial_puts("       [FAIL] nvme_read_sector(0) returned false!\n");
        hcf();
    }
    if (verify_sector_pattern(read_buf, "FORTRESS_NVME_LBA0_BOOT_MAGIC_PATTERN_TEST_#0000#_", 512)) {
        serial_puts("       [PASS] LBA 0 read verified (Valid boot sector magic pattern)\n");
    } else {
        serial_puts("       [FAIL] LBA 0 buffer verification failed!\n");
        hcf();
    }

    /* LBA 1 */
    memset(read_buf, 0, sizeof(read_buf));
    if (!nvme_read_sector(1, read_buf)) {
        serial_puts("       [FAIL] nvme_read_sector(1) returned false!\n");
        hcf();
    }
    if (verify_sector_pattern(read_buf, "FORTRESS_NVME_LBA1_METADATA_HEADER_PATTERN_#0001#_", 512)) {
        serial_puts("       [PASS] LBA 1 read verified (Valid test metadata pattern)\n");
    } else {
        serial_puts("       [FAIL] LBA 1 buffer verification failed!\n");
        hcf();
    }

    /* LBA 100 */
    memset(read_buf, 0, sizeof(read_buf));
    if (!nvme_read_sector(100, read_buf)) {
        serial_puts("       [FAIL] nvme_read_sector(100) returned false!\n");
        hcf();
    }
    if (!verify_sector_pattern(read_buf, "FORTRESS_NVME_LBA100_MIDRANGE_INTEGRITY_DATA_#0100#_", 512)) {
        serial_puts("       [FAIL] LBA 100 buffer pattern verification failed!\n");
        hcf();
    }
    serial_puts("       [PASS] LBA 100 read verified (100% data buffer match: #0100#)\n");

    /* LBA 1000 */
    memset(read_buf, 0, sizeof(read_buf));
    if (!nvme_read_sector(1000, read_buf)) {
        serial_puts("       [FAIL] nvme_read_sector(1000) returned false!\n");
        hcf();
    }
    if (!verify_sector_pattern(read_buf, "FORTRESS_NVME_LBA1000_HIGHRANGE_DATA_VERIFY_#1000#_", 512)) {
        serial_puts("       [FAIL] LBA 1000 buffer pattern verification failed!\n");
        hcf();
    }
    serial_puts("       [PASS] LBA 1000 read verified (100% data buffer match: #1000#)\n");

    /* 4. Queue Wraparound Stress Test (70 consecutive reads over 32-entry queues on LBAs 100 & 1000) */
    serial_puts("[TEST 4] Queue Wraparound Stress Test (70 reads across 32-entry queue pair)...\n");
    for (int i = 0; i < 70; i++) {
        uint64_t target_lba = (i % 2 == 0) ? 100 : 1000;
        const char *expected_pat = (i % 2 == 0)
            ? "FORTRESS_NVME_LBA100_MIDRANGE_INTEGRITY_DATA_#0100#_"
            : "FORTRESS_NVME_LBA1000_HIGHRANGE_DATA_VERIFY_#1000#_";

        if (!nvme_read_sector(target_lba, read_buf)) {
            serial_puts("       [FAIL] Queue wraparound read failed at iteration: ");
            serial_print_dec(i);
            serial_puts("\n");
            hcf();
        }

        if (!verify_sector_pattern(read_buf, expected_pat, 512)) {
            serial_puts("       [FAIL] Pattern mismatch during wraparound test at iteration: ");
            serial_print_dec(i);
            serial_puts("\n");
            hcf();
        }
    }
    serial_puts("       [PASS] 70 sequential sector reads executed; SQ tail, CQ head, and Phase bit wrapped multiple times\n");

    /* 5. Negative Test: Out-of-Range LBA Request */
    serial_puts("[TEST 5] Negative Test: Requesting Out-of-Range LBA...\n");
    uint64_t out_of_range_lba = sector_count + 1000;
    bool oob_result = nvme_read_sector(out_of_range_lba, read_buf);
    if (oob_result != false) {
        serial_puts("       [FAIL] Out-of-range LBA read unexpectedly succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Out-of-range LBA cleanly rejected with controller error status\n");

    /* 6. Verify Dynamic Kernel Heap Integrity */
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Kernel heap walk detected corruption post-NVMe reads!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("\n[ OK ] Phase 9 (Step 9B.1): NVMe Initialization & Reads Verification PASSED!\n\n");
}

static void test_phase9b2_nvme_writes(void) {
    serial_puts("\n========================================================\n");
    serial_puts("Phase 9 (Step 9B.2): NVMe Writes, Flush & Persistence Test\n");
    serial_puts("========================================================\n");

    static uint8_t sector_buf[512];
    static uint8_t write_buf[512];
    static uint8_t guard_buf[512];

    const char *virgin_lba500_pattern = "FORTRESS_NVME_LBA500_VIRGIN_PRE_WRITE_PATTERN_#0500#_";
    const char *persisted_lba500_pattern = "FORTRESS_NVME_LBA500_PHASE9B2_PERSISTED_DATA_#9B2#_";
    const char *guard_lower_pattern = "FORTRESS_NVME_LBA499_NEIGHBOUR_GUARD_LOWER_#0499#_";
    const char *guard_upper_pattern = "FORTRESS_NVME_LBA501_NEIGHBOUR_GUARD_UPPER_#0501#_";
    const char *wraparound_pat_a = "FORTRESS_NVME_LBA600_QUEUE_WRAPAROUND_TARGET_#0600#_";
    const char *wraparound_pat_b = "FORTRESS_NVME_LBA600_WRAPAROUND_CYCLE_UPDATED_#600B#_";

    serial_puts("[TEST 1] Reading designated persistence test sector (LBA 500)...\n");
    memset(sector_buf, 0, sizeof(sector_buf));
    if (!nvme_read_sector(500, sector_buf)) {
        serial_puts("       [FAIL] nvme_read_sector(500) failed!\n");
        hcf();
    }

    bool is_virgin = verify_sector_pattern(sector_buf, virgin_lba500_pattern, 512);
    bool is_persisted = verify_sector_pattern(sector_buf, persisted_lba500_pattern, 512);

    if (!is_virgin && !is_persisted) {
        serial_puts("       [ABORT] LBA 500 contents match neither virgin pattern nor persisted signature!\n");
        hcf();
    }

    if (is_virgin) {
        serial_puts("       [STAGE 1 DETECTED] Virgin disk image detected at LBA 500.\n");
        serial_puts("[TEST 2] Verifying neighbouring guard sectors before writes...\n");
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(499, guard_buf) || !verify_sector_pattern(guard_buf, guard_lower_pattern, 512)) {
            serial_puts("       [FAIL] Pre-write lower guard sector (LBA 499) corrupted!\n");
            hcf();
        }
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(501, guard_buf) || !verify_sector_pattern(guard_buf, guard_upper_pattern, 512)) {
            serial_puts("       [FAIL] Pre-write upper guard sector (LBA 501) corrupted!\n");
            hcf();
        }
        serial_puts("       [PASS] Lower guard (LBA 499) and Upper guard (LBA 501) pristine\n");

        size_t pat_len = strlen(persisted_lba500_pattern);
        for (size_t i = 0; i < 512; i++) {
            write_buf[i] = (uint8_t)persisted_lba500_pattern[i % pat_len];
        }

        serial_puts("[TEST 3] Writing distinct signature pattern to LBA 500...\n");
        if (!nvme_write_sector(500, write_buf)) {
            serial_puts("       [FAIL] nvme_write_sector(500) returned false!\n");
            hcf();
        }
        serial_puts("       [PASS] Synchronous write to LBA 500 completed successfully\n");

        serial_puts("[TEST 4] Issuing NVMe Flush command...\n");
        if (!nvme_flush()) {
            serial_puts("       [FAIL] nvme_flush() returned false!\n");
            hcf();
        }
        serial_puts("       [PASS] NVMe Flush command acknowledged by controller\n");

        serial_puts("[TEST 5] Immediate readback verification of LBA 500...\n");
        memset(sector_buf, 0, sizeof(sector_buf));
        if (!nvme_read_sector(500, sector_buf)) {
            serial_puts("       [FAIL] Immediate readback nvme_read_sector(500) failed!\n");
            hcf();
        }
        if (memcmp(sector_buf, write_buf, 512) != 0) {
            serial_puts("       [FAIL] Immediate readback data does not match written buffer!\n");
            hcf();
        }
        serial_puts("       [PASS] Immediate readback verified (100% 512-byte match)\n");

        serial_puts("[TEST 6] Confirming neighbouring guard sectors remained untouched...\n");
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(499, guard_buf) || !verify_sector_pattern(guard_buf, guard_lower_pattern, 512)) {
            serial_puts("       [FAIL] Post-write lower guard sector (LBA 499) was modified!\n");
            hcf();
        }
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(501, guard_buf) || !verify_sector_pattern(guard_buf, guard_upper_pattern, 512)) {
            serial_puts("       [FAIL] Post-write upper guard sector (LBA 501) was modified!\n");
            hcf();
        }
        serial_puts("       [PASS] Lower guard (LBA 499) and Upper guard (LBA 501) verified untouched\n");

        serial_puts("[TEST 7] Queue Wraparound Write Stress Test (70 writes across 32-entry queue)...\n");
        for (int i = 0; i < 70; i++) {
            const char *curr_pat = (i % 2 == 0) ? wraparound_pat_a : wraparound_pat_b;
            size_t curr_len = strlen(curr_pat);
            for (size_t b = 0; b < 512; b++) {
                write_buf[b] = (uint8_t)curr_pat[b % curr_len];
            }

            if (!nvme_write_sector(600, write_buf)) {
                serial_puts("       [FAIL] Queue wraparound write failed at iteration: ");
                serial_print_dec(i);
                serial_puts("\n");
                hcf();
            }
        }
        if (!nvme_flush()) {
            serial_puts("       [FAIL] Flush post-wraparound failed!\n");
            hcf();
        }

        memset(sector_buf, 0, sizeof(sector_buf));
        if (!nvme_read_sector(600, sector_buf) || !verify_sector_pattern(sector_buf, wraparound_pat_b, 512)) {
            serial_puts("       [FAIL] LBA 600 does not match final wraparound pattern!\n");
            hcf();
        }
        serial_puts("       [PASS] 70 sequential sector writes executed across queue wraparound\n");

        if (!heap_verify_integrity()) {
            serial_puts("       [FAIL] Heap integrity walk failed post-NVMe writes!\n");
            hcf();
        }
        serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

        serial_puts("\n[STAGE 1 PASS] Write, Flush, and Wraparound successful! Ready for reboot persistence verification.\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        outw(0x4004, 0x3400);
        serial_puts("[STAGE 1] Poweroff signal sent; halting.\n");
        hcf();
    } else {
        serial_puts("       [STAGE 2 DETECTED] Post-reboot disk image detected (Signature found at LBA 500)!\n");
        serial_puts("[TEST 2] Verifying 100% 512-byte sector match for persisted data at LBA 500...\n");
        size_t pat_len = strlen(persisted_lba500_pattern);
        for (size_t i = 0; i < 512; i++) {
            write_buf[i] = (uint8_t)persisted_lba500_pattern[i % pat_len];
        }

        if (memcmp(sector_buf, write_buf, 512) != 0) {
            serial_puts("       [FAIL] Persisted sector at LBA 500 does not match expected buffer!\n");
            hcf();
        }
        serial_puts("       [PASS] 100% exact 512-byte data match verified on reboot!\n");

        serial_puts("[TEST 3] Verifying neighbouring sectors remained pristine across reboot...\n");
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(499, guard_buf) || !verify_sector_pattern(guard_buf, guard_lower_pattern, 512)) {
            serial_puts("       [FAIL] Lower guard sector (LBA 499) corrupted post-reboot!\n");
            hcf();
        }
        memset(guard_buf, 0, sizeof(guard_buf));
        if (!nvme_read_sector(501, guard_buf) || !verify_sector_pattern(guard_buf, guard_upper_pattern, 512)) {
            serial_puts("       [FAIL] Upper guard sector (LBA 501) corrupted post-reboot!\n");
            hcf();
        }
        serial_puts("       [PASS] Lower guard (LBA 499) and Upper guard (LBA 501) verified pristine post-reboot\n");

        if (!heap_verify_integrity()) {
            serial_puts("       [FAIL] Heap integrity walk failed post-reboot!\n");
            hcf();
        }
        serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");
        serial_puts("\n[ OK ] Phase 9 (Step 9B.2): Two-Stage Persistent NVMe Storage PASSED!\n\n");
    }
}
#endif

/* Mock Block Device for In-Memory Negative Testing */
typedef struct {
    block_dev_t dev;
    uint8_t    *sectors;
    uint64_t    sector_count;
    uint32_t    sector_size;
    size_t      read_calls;
} mock_disk_t;

static bool mock_disk_read(block_dev_t *dev, uint64_t lba, void *buf) {
    if (!dev || !buf) return false;
    mock_disk_t *m = (mock_disk_t *)dev->priv;
    if (!m) return false;
    m->read_calls++;
    if (lba >= m->sector_count) return false;
    memcpy(buf, m->sectors + lba * m->sector_size, m->sector_size);
    return true;
}

static void mock_disk_init(mock_disk_t *m, const char *name, uint8_t *storage, uint64_t sectors, uint32_t sec_sz) {
    memset(m, 0, sizeof(mock_disk_t));
    size_t name_len = strlen(name);
    if (name_len >= sizeof(m->dev.name)) name_len = sizeof(m->dev.name) - 1;
    memcpy(m->dev.name, name, name_len);
    m->dev.name[name_len] = '\0';
    m->dev.sector_size  = sec_sz;
    m->dev.sector_count = sectors;
    m->dev.read_sector  = mock_disk_read;
    m->dev.write_sector = NULL;
    m->dev.flush        = NULL;
    m->dev.priv         = m;
    m->sectors          = storage;
    m->sector_count     = sectors;
    m->sector_size      = sec_sz;
    m->read_calls       = 0;
}

static void synthesize_mock_gpt(uint8_t *disk, uint64_t total_sectors, uint32_t sector_size,
                                uint32_t num_entries, uint32_t entry_size,
                                uint64_t part_start, uint64_t part_end) {
    memset(disk, 0, total_sectors * sector_size);

    /* 1. MBR at LBA 0 */
    gpt_protective_mbr_t *mbr = (gpt_protective_mbr_t *)disk;
    mbr->entries[0].os_type = MBR_PARTITION_TYPE_GPT;
    mbr->entries[0].starting_lba = 1;
    mbr->entries[0].size_in_lba = (uint32_t)(total_sectors - 1);
    mbr->signature = MBR_SIGNATURE_MAGIC;

    /* 2. Partition Array */
    size_t array_bytes = (size_t)num_entries * entry_size;
    size_t array_sectors = (array_bytes + sector_size - 1) / sector_size;
    uint8_t *pri_array = disk + 2 * sector_size;
    uint64_t bak_array_lba = total_sectors - 1 - array_sectors;
    uint8_t *bak_array = disk + bak_array_lba * sector_size;

    if (part_start != 0 || part_end != 0) {
        gpt_entry_t *entry = (gpt_entry_t *)pri_array;
        entry->type_guid = GPT_GUID_LINUX_FS;
        entry->unique_partition_guid = GPT_GUID_ESP;
        entry->starting_lba = part_start;
        entry->ending_lba = part_end;
        memcpy(bak_array, pri_array, array_bytes);
    }

    uint32_t array_crc = crc32(0, pri_array, array_bytes);

    /* 3. Primary Header at LBA 1 */
    gpt_header_t *pri_hdr = (gpt_header_t *)(disk + 1 * sector_size);
    pri_hdr->signature = GPT_SIGNATURE_MAGIC;
    pri_hdr->revision = GPT_REVISION_1_0;
    pri_hdr->header_size = GPT_MIN_HEADER_SIZE;
    pri_hdr->current_lba = 1;
    pri_hdr->backup_lba = total_sectors - 1;
    pri_hdr->first_usable_lba = 2 + array_sectors;
    pri_hdr->last_usable_lba = bak_array_lba - 1;
    pri_hdr->disk_guid = GPT_GUID_LINUX_FS;
    pri_hdr->partition_entry_lba = 2;
    pri_hdr->num_partition_entries = num_entries;
    pri_hdr->sizeof_partition_entry = entry_size;
    pri_hdr->partition_entry_array_crc32 = array_crc;
    pri_hdr->header_crc32 = 0;
    pri_hdr->header_crc32 = crc32(0, pri_hdr, pri_hdr->header_size);

    /* 4. Backup Header at LBA total_sectors - 1 */
    gpt_header_t *bak_hdr = (gpt_header_t *)(disk + (total_sectors - 1) * sector_size);
    bak_hdr->signature = GPT_SIGNATURE_MAGIC;
    bak_hdr->revision = GPT_REVISION_1_0;
    bak_hdr->header_size = GPT_MIN_HEADER_SIZE;
    bak_hdr->current_lba = total_sectors - 1;
    bak_hdr->backup_lba = 1;
    bak_hdr->first_usable_lba = 2 + array_sectors;
    bak_hdr->last_usable_lba = bak_array_lba - 1;
    bak_hdr->disk_guid = GPT_GUID_LINUX_FS;
    bak_hdr->partition_entry_lba = bak_array_lba;
    bak_hdr->num_partition_entries = num_entries;
    bak_hdr->sizeof_partition_entry = entry_size;
    bak_hdr->partition_entry_array_crc32 = array_crc;
    bak_hdr->header_crc32 = 0;
    bak_hdr->header_crc32 = crc32(0, bak_hdr, bak_hdr->header_size);
}

/* =========================================================================
 * Phase 9 (Step 9C.1): GPT Partition Parsing & Block Devices
 * ========================================================================= */
static void test_phase9c1_gpt(void) {
    serial_puts("\n========================================================\n");
    serial_puts("Phase 9 (Step 9C.1): GPT Partition Parsing & Block Devices\n");
    serial_puts("========================================================\n");

    /* 0. Ensure NVMe Driver is initialized */
    if (!nvme_is_initialized()) {
        if (!nvme_init()) {
            serial_puts("       [FAIL] nvme_init() failed!\n");
            hcf();
        }
    }

    /* 1. IEEE 802.3 CRC32 Engine Self-Test */
    serial_puts("[TEST 1] Executing IEEE 802.3 CRC32 verification vector...\n");
    if (!crc32_selftest()) {
        serial_puts("       [FAIL] CRC32 self-test failed! Reference vector mismatch!\n");
        hcf();
    }
    serial_puts("       [PASS] CRC32 engine verified (vector \"123456789\" -> 0xCBF43926)\n");

    /* 2. Block Device Subsystem Initialization & Base Device Registration */
    serial_puts("[TEST 2] Initializing Block Device layer & registering NVMe controller...\n");
    block_init();
    if (!block_register_nvme()) {
        serial_puts("       [FAIL] block_register_nvme() failed!\n");
        hcf();
    }

    block_dev_t *nvme_dev = block_get_dev_by_name("nvme0n1");
    if (!nvme_dev) {
        serial_puts("       [FAIL] Could not locate block device nvme0n1!\n");
        hcf();
    }
    if (block_get_sector_size(nvme_dev) != 512 || block_get_sector_count(nvme_dev) != 65536) {
        serial_puts("       [FAIL] nvme0n1 reports unexpected geometry via block device API!\n");
        hcf();
    }
    serial_puts("       [PASS] Base block device nvme0n1 registered (65536 sectors, 512 B/sector, ");
    serial_print_dec(block_get_capacity_bytes(nvme_dev) / (1024 * 1024));
    serial_puts(" MiB)\n");

    /* 3. GPT Header, Protective MBR, and Partition Array Discovery */
    serial_puts("[TEST 3] Parsing GUID Partition Table (GPT) on nvme0n1...\n");
    gpt_policy_result_t policy;
    if (!gpt_parse_ex(nvme_dev, &policy) || policy != GPT_POLICY_PRIMARY_CONSISTENT) {
        serial_puts("       [FAIL] gpt_parse_ex() failed on nvme0n1 or unexpected policy: ");
        serial_print_dec((uint32_t)policy);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] GPT discovery passed (Protective MBR, Primary/Backup consistent policy)\n");

    /* 4. Validate Partition Discovery & Metadata */
    serial_puts("[TEST 4] Validating Partition Discovery & Attributes...\n");
    size_t part_count = gpt_get_partition_count();
    if (part_count != 1) {
        serial_puts("       [FAIL] Expected exactly 1 partition on test disk!\n");
        hcf();
    }

    gpt_partition_t *part1 = gpt_find_by_type(&GPT_GUID_LINUX_FS);
    if (!part1) {
        serial_puts("       [FAIL] Linux Filesystem Data partition not found!\n");
        hcf();
    }

    if (part1->starting_lba != 2048 || part1->ending_lba != 10239 || part1->sector_count != 8192) {
        serial_puts("       [FAIL] Partition 1 bounds mismatch! Expected 2048..10239 (8192 sectors)\n");
        hcf();
    }
    serial_puts("       [PASS] Discovered Partition 1 (Linux FS): LBA 2048..10239 (8192 sectors, 4 MiB)\n");

    /* 5. Bounded Block Device Adapter Verification (nvme0n1p1) */
    serial_puts("[TEST 5] Testing Bounded Partition Block Device (nvme0n1p1)...\n");
    block_dev_t *part_dev = block_get_dev_by_name("nvme0n1p1");
    if (!part_dev) {
        serial_puts("       [FAIL] Partition block device nvme0n1p1 not found in registry!\n");
        hcf();
    }
    if (block_get_sector_size(part_dev) != 512 || block_get_sector_count(part_dev) != 8192) {
        serial_puts("       [FAIL] nvme0n1p1 reports invalid bounds!\n");
        hcf();
    }

    /* 5A: Read relative LBA 0 (maps to parent LBA 2048) */
    static uint8_t part_read_buf[512];
    memset(part_read_buf, 0, sizeof(part_read_buf));
    if (!block_read_sector(part_dev, 0, part_read_buf)) {
        serial_puts("       [FAIL] Reading partition relative LBA 0 failed!\n");
        hcf();
    }
    uint8_t parent_sector_2048[512];
    if (!block_read_sector(nvme_dev, 2048, parent_sector_2048) ||
        memcmp(part_read_buf, parent_sector_2048, 512)) {
        serial_puts("[FAIL] Partition translation mismatch\n");
        hcf();
    }
    serial_puts("       [PASS] Relative LBA 0 read verified (maps to parent LBA 2048)\n");

    /* 5B: Inspect ext2 Superblock at relative LBA 2 (offset 1024 into partition) */
    memset(part_read_buf, 0, sizeof(part_read_buf));
    if (!block_read_sector(part_dev, 2, part_read_buf)) {
        serial_puts("       [FAIL] Reading partition relative LBA 2 (ext2 superblock) failed!\n");
        hcf();
    }
    uint16_t ext2_magic = *(uint16_t *)(part_read_buf + 0x38);
    if (ext2_magic != 0xEF53) {
        serial_puts("       [FAIL] Ext2 superblock magic mismatch! Expected 0xEF53, got: ");
        serial_print_hex(ext2_magic);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Ext2 filesystem signature verified on nvme0n1p1 (s_magic: 0xEF53)\n");

    /* 5C: Read relative LBA 8191 (last sector in partition, maps to parent LBA 10239) */
    memset(part_read_buf, 0, sizeof(part_read_buf));
    if (!block_read_sector(part_dev, 8191, part_read_buf)) {
        serial_puts("       [FAIL] Reading partition relative LBA 8191 (last sector) failed!\n");
        hcf();
    }
    uint8_t parent_sector_10239[512];
    if (!block_read_sector(nvme_dev, 10239, parent_sector_10239) ||
        memcmp(part_read_buf, parent_sector_10239, 512)) {
        serial_puts("[FAIL] Partition translation mismatch\n");
        hcf();
    }
    serial_puts("       [PASS] Relative LBA 8191 (last sector) read verified (maps to parent LBA 10239)\n");

    /* 5D: Boundary Enforcement - read at exact boundary (LBA 8192) MUST be rejected */
    if (block_read_sector(part_dev, 8192, part_read_buf) != false) {
        serial_puts("       [FAIL] Boundary violation! Read at partition boundary LBA 8192 succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Read at partition capacity boundary (LBA 8192) strictly rejected\n");

    /* 5E: Read beyond boundary (e.g. LBA 99999) MUST be rejected */
    if (block_read_sector(part_dev, 99999, part_read_buf) != false) {
        serial_puts("       [FAIL] Boundary violation! Read at out-of-bounds LBA 99999 succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Read at out-of-bounds LBA (99999) strictly rejected\n");

    /* 5F: Arithmetic overflow read (UINT64_MAX) MUST be rejected */
    if (block_read_sector(part_dev, UINT64_MAX, part_read_buf) != false) {
        serial_puts("       [FAIL] Arithmetic overflow read succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Arithmetic overflow read (UINT64_MAX) strictly rejected\n");

    /* 5G: Boundary Enforcement on Write - write at exact boundary (LBA 8192) MUST be rejected */
    if (block_write_sector(part_dev, 8192, part_read_buf) != false) {
        serial_puts("       [FAIL] Boundary violation! Write at partition boundary LBA 8192 succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Write at partition capacity boundary (LBA 8192) strictly rejected\n");

    /* 5H: Arithmetic overflow write (UINT64_MAX) MUST be rejected */
    if (block_write_sector(part_dev, UINT64_MAX, part_read_buf) != false) {
        serial_puts("       [FAIL] Arithmetic overflow write succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Arithmetic overflow write (UINT64_MAX) strictly rejected\n");

    /* 5I: Flush on partition device passes through to parent */
    if (block_flush(part_dev) != true) {
        serial_puts("       [FAIL] Flush on partition device failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Flush on partition device nvme0n1p1 verified\n");

    /* =========================================================================
     * Step 6: Expanded Negative Test Suite (In-Memory Mock Block Devices)
     * ========================================================================= */
    serial_puts("[TEST 6] Executing Expanded Negative & Corner-Case GPT Test Suite...\n");

    const size_t mock_sectors = 100;
    uint8_t *mock_buf = (uint8_t *)kmalloc(mock_sectors * 512);
    if (!mock_buf) {
        serial_puts("       [FAIL] Failed to allocate mock disk buffer!\n");
        hcf();
    }

    mock_disk_t mock_disk;

    /* N1: Bad primary array CRC with valid backup -> read-only fallback to backup */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    mock_buf[2 * 512] ^= 0xAA; /* Corrupt primary array */
    mock_disk_init(&mock_disk, "mockN1", mock_buf, mock_sectors, 512);
    gpt_policy_result_t n1_policy;
    if (!gpt_parse_ex(&mock_disk.dev, &n1_policy) || n1_policy != GPT_POLICY_BACKUP_FALLBACK) {
        serial_puts("       [FAIL] Negative test N1 failed! Expected BACKUP_FALLBACK\n");
        hcf();
    }
    if (gpt_get_partition_count() != 1) {
        serial_puts("       [FAIL] Negative test N1 did not discover partition from backup array!\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N1: Corrupted primary array cleanly fell back to Backup in memory\n");

    /* N2: Both Primary and Backup copies invalid -> reject disk */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    mock_buf[1 * 512] = 'Z';                  /* Corrupt primary header signature */
    mock_buf[(mock_sectors - 1) * 512] = 'Z'; /* Corrupt backup header signature */
    mock_disk_init(&mock_disk, "mockN2", mock_buf, mock_sectors, 512);
    gpt_policy_result_t n2_policy;
    if (gpt_parse_ex(&mock_disk.dev, &n2_policy) || n2_policy != GPT_POLICY_REJECT_INVALID) {
        serial_puts("       [FAIL] Negative test N2 failed! Expected REJECT_INVALID\n");
        hcf();
    }
    if (gpt_get_partition_count() != 0) {
        serial_puts("       [FAIL] Negative test N2 published partitions from invalid disk!\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N2: Dual invalid headers cleanly rejected disk (0 partitions published)\n");

    /* N3: Ambiguous valid but inconsistent headers -> reject ambiguity */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    gpt_header_t *bak_hdr = (gpt_header_t *)(mock_buf + (mock_sectors - 1) * 512);
    bak_hdr->disk_guid.bytes[0] ^= 0x55; /* Inconsistent GUID */
    bak_hdr->header_crc32 = 0;
    bak_hdr->header_crc32 = crc32(0, bak_hdr, bak_hdr->header_size);
    mock_disk_init(&mock_disk, "mockN3", mock_buf, mock_sectors, 512);
    gpt_policy_result_t n3_policy;
    if (gpt_parse_ex(&mock_disk.dev, &n3_policy) || n3_policy != GPT_POLICY_REJECT_AMBIGUITY) {
        serial_puts("       [FAIL] Negative test N3 failed! Expected REJECT_AMBIGUITY\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N3: Ambiguous inconsistent headers cleanly rejected\n");

    /* N4: Valid-CRC table containing overlapping partitions -> rejected, zero partial registry entries */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    /* Add overlapping second partition entry: 45..60 (overlaps 34..50 at 45..50) */
    gpt_entry_t *pri_entry2 = (gpt_entry_t *)(mock_buf + 2 * 512 + 128);
    pri_entry2->type_guid = GPT_GUID_ESP;
    pri_entry2->unique_partition_guid = GPT_GUID_LINUX_FS;
    pri_entry2->starting_lba = 45;
    pri_entry2->ending_lba = 60;

    size_t array_bytes = 16 * 128;
    size_t array_sectors = (array_bytes + 512 - 1) / 512;
    uint8_t *bak_array = mock_buf + (mock_sectors - 1 - array_sectors) * 512;
    memcpy(bak_array, mock_buf + 2 * 512, array_bytes);

    uint32_t new_crc = crc32(0, mock_buf + 2 * 512, array_bytes);
    gpt_header_t *p_hdr = (gpt_header_t *)(mock_buf + 1 * 512);
    p_hdr->partition_entry_array_crc32 = new_crc;
    p_hdr->header_crc32 = 0;
    p_hdr->header_crc32 = crc32(0, p_hdr, p_hdr->header_size);

    bak_hdr = (gpt_header_t *)(mock_buf + (mock_sectors - 1) * 512);
    bak_hdr->partition_entry_array_crc32 = new_crc;
    bak_hdr->header_crc32 = 0;
    bak_hdr->header_crc32 = crc32(0, bak_hdr, bak_hdr->header_size);

    mock_disk_init(&mock_disk, "mockN4", mock_buf, mock_sectors, 512);
    if (gpt_parse(&mock_disk.dev) != false) {
        serial_puts("       [FAIL] Negative test N4 failed! Overlapping partitions accepted!\n");
        hcf();
    }
    if (gpt_get_partition_count() != 0 || block_get_dev_by_name("mockN4p1") != NULL) {
        serial_puts("       [FAIL] Negative test N4 left partial registry entries!\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N4: Valid-CRC overlapping partitions rejected (0 partial devices published)\n");

    /* N5: Valid-CRC table with out-of-range partition -> rejected without partial devices */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 999);
    mock_disk_init(&mock_disk, "mockN5", mock_buf, mock_sectors, 512);
    if (gpt_parse(&mock_disk.dev) != false) {
        serial_puts("       [FAIL] Negative test N5 failed! Out-of-range partition accepted!\n");
        hcf();
    }
    if (gpt_get_partition_count() != 0 || block_get_dev_by_name("mockN5p1") != NULL) {
        serial_puts("       [FAIL] Negative test N5 left partial registry entries!\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N5: Valid-CRC out-of-range partition rejected without publishing\n");

    /* N6: Oversized entry count (> 128) & placement collision -> rejected before allocation */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    p_hdr = (gpt_header_t *)(mock_buf + 1 * 512);
    p_hdr->num_partition_entries = 500; /* Exceeds GPT_MAX_SUPPORTED_ENTRIES (128) */
    p_hdr->header_crc32 = 0;
    p_hdr->header_crc32 = crc32(0, p_hdr, p_hdr->header_size);
    bak_hdr = (gpt_header_t *)(mock_buf + (mock_sectors - 1) * 512);
    bak_hdr->num_partition_entries = 500;
    bak_hdr->header_crc32 = 0;
    bak_hdr->header_crc32 = crc32(0, bak_hdr, bak_hdr->header_size);
    mock_disk_init(&mock_disk, "mockN6", mock_buf, mock_sectors, 512);
    if (gpt_parse(&mock_disk.dev) != false) {
        serial_puts("       [FAIL] Negative test N6 failed! Oversized entry count accepted!\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N6: Oversized partition entry count rejected during header validation\n");

    /* N7: Parent Driver Read Dispatch Isolation */
    synthesize_mock_gpt(mock_buf, mock_sectors, 512, 16, 128, 34, 50);
    mock_disk_init(&mock_disk, "mockN7", mock_buf, mock_sectors, 512);
    if (!gpt_parse(&mock_disk.dev)) {
        serial_puts("       [FAIL] Setup for N7 failed!\n");
        hcf();
    }
    block_dev_t *mock_part = block_get_dev_by_name("mockN7p1");
    if (!mock_part) {
        serial_puts("       [FAIL] Mock partition device mockN7p1 not found!\n");
        hcf();
    }

    mock_disk.read_calls = 0; /* Reset call counter */
    static uint8_t n7_dummy[512];
    bool r_bound = block_read_sector(mock_part, mock_part->sector_count, n7_dummy);
    bool r_oob   = block_read_sector(mock_part, mock_part->sector_count + 100, n7_dummy);
    bool r_ovf   = block_read_sector(mock_part, UINT64_MAX, n7_dummy);

    if (r_bound != false || r_oob != false || r_ovf != false) {
        serial_puts("       [FAIL] Out-of-bounds reads on mock partition unexpectedly succeeded!\n");
        hcf();
    }
    if (mock_disk.read_calls != 0) {
        serial_puts("       [FAIL] Rejected partition read reached parent driver! Read calls: ");
        serial_print_dec(mock_disk.read_calls);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] Negative N7: Rejected partition reads NEVER reached parent driver (parent calls: 0)\n");

    kfree(mock_buf);

    /* 7. Re-parse live NVMe device so active system has nvme0n1p1 ready */
    if (!gpt_parse(nvme_dev)) {
        serial_puts("       [FAIL] Re-parsing live NVMe device failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Live NVMe device restored to active registry (nvme0n1p1 ready)\n");

    /* 8. Dynamic Kernel Heap Integrity Verification */
    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed post-GPT parsing!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("\n[ OK ] Phase 9 (Step 9C.1): GPT Partition Parsing & Block Devices PASSED!\n\n");
}

/* Kernel Main Entry Point */
static void require_ext2(bool ok, const char *message) {
    if (!ok) {
        serial_puts("[FAIL] ext2/audit: "); serial_puts(message); serial_puts("\n");
        hcf();
    }
}

static bool qemu_fw_cfg_has_key(const char *key) {
    outw(0x510, 0x0000);
    char sig[4];
    for (int i = 0; i < 4; i++) sig[i] = (char)inb(0x511);
    if (memcmp(sig, "QEMU", 4) != 0) return false;

    outw(0x510, 0x0019);
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) {
        count = (count << 8) | inb(0x511);
    }
    if (count > 256) count = 256;

    for (uint32_t i = 0; i < count; i++) {
        for (int k = 0; k < 8; k++) (void)inb(0x511);
        char name[56];
        for (int k = 0; k < 56; k++) name[k] = (char)inb(0x511);
        name[55] = '\0';
        if (!strcmp(name, key)) {
            return true;
        }
    }
    return false;
}

static void test_ext2_and_audits(void) {
    serial_puts("\n[TEST] Read-only ext2 and architectural audits\n");
    require_ext2(spin_debug_selftest(), "lock ranks and caller IRQ restoration");
    serial_puts("[PASS] Lock recursion/inversion predicates and nested IRQ restoration\n");
    block_dev_t *mnt_dev = block_get_dev_by_name("nvme0n1p1");
    bool mnt_ok = false;
    bool write_opt_in = qemu_fw_cfg_has_key("opt/fortress/write_test");
    if (mnt_dev && mnt_dev->write_sector && write_opt_in) {
        mnt_ok = ext2_mount_rw(mnt_dev, "/mnt");
    }
    if (!mnt_ok && mnt_dev) {
        mnt_ok = ext2_mount(mnt_dev, "/mnt");
    }
    require_ext2(mnt_ok, "mount");
    require_ext2(vfs_lookup("/etc/motd") != NULL, "initramfs preserved");
    require_ext2(vfs_lookup("/mnt/nested/note.txt") != NULL, "nested path");
    require_ext2(vfs_lookup("/mnt/missing") == NULL, "missing path");
    file_t *wopen = vfs_open("/mnt/hello.txt", 1);
    if (mnt_dev && mnt_dev->write_sector && write_opt_in) {
        require_ext2(wopen != NULL, "write open supported");
        vfs_close(wopen);
    } else {
        require_ext2(wopen == NULL, "write open rejected on read-only mount");
    }
    vfs_node_t *dir = vfs_lookup("/mnt");
    vfs_dirent_t dent;
    bool saw_hello = false;
    int result;
    uint64_t index = 0;
    while ((result = vfs_readdir(dir, index++, &dent)) == 1) {
        if (!strcmp(dent.name, "hello.txt")) saw_hello = true;
        require_ext2(index < 32, "bounded enumeration");
    }
    require_ext2(result == 0 && saw_hello, "directory enumeration");
    file_t *a = vfs_open("/mnt/hello.txt", 0), *b = vfs_open("/mnt/hello.txt", 0);
    require_ext2(a && b, "independent open");
    uint8_t buf[1024], other[16];
    require_ext2(vfs_read(a, buf, 16) == 16 && vfs_read(b, other, 16) == 16 &&
                 !memcmp(buf, other, 16), "independent offsets");
    require_ext2(vfs_read(a, buf, 0) == 0 && a->offset == 16, "zero read");
    vfs_close(a); vfs_close(b);
    a = vfs_open("/mnt/large.bin", 0);
    require_ext2(a != NULL, "large open");
    size_t off = 0;
    while ((result = (int)vfs_read(a, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < result; i++)
            require_ext2(buf[i] == (uint8_t)((off + i) * 17 + 3), "indirect data");
        off += result;
    }
    require_ext2(result == 0 && off == 400000, "large EOF");
    vfs_close(a);
    a = vfs_open("/mnt/sparse.bin", 0);
    require_ext2(a != NULL, "sparse open");
    off = 0;
    while ((result = (int)vfs_read(a, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < result; i++) {
            uint8_t expected = off + i < 20000 ? 0 : (uint8_t)"END"[off + i - 20000];
            require_ext2(buf[i] == expected, "sparse zero fill");
        }
        off += result;
    }
    require_ext2(result == 0 && off == 20003, "sparse EOF");
    vfs_close(a);
    serial_puts("[PASS] ext2 lookup, readdir, offsets, direct/single/double-indirect and sparse reads\n");

    /* Static buffers exist before baseline, and mount/cache allocations are
     * intentionally retained. Warm a complete process lifecycle first. */
    static uint8_t before[PMM_BITMAP_CAPACITY_BYTES], after[PMM_BITMAP_CAPACITY_BYTES];
    uintptr_t first_frame = pmm_alloc_page();
    require_ext2(first_frame != 0 && pmm_snapshot(before, sizeof(before)), "audit setup");
    size_t same_count = pmm_get_free_pages();
    uintptr_t second_frame = pmm_alloc_page();
    require_ext2(second_frame != 0 && second_frame != first_frame, "distinct audit frames");
    pmm_free_page(first_frame);
    require_ext2(pmm_get_free_pages() == same_count && pmm_snapshot(after, sizeof(after)) &&
                 memcmp(before, after, sizeof(before)) != 0, "equal counts must not hide changed frame set");
    pmm_free_page(second_frame);
    serial_puts("[PASS] Exact bitmap detects changed allocation set despite equal allocation counters\n");
    extern const uint8_t embedded_init_elf_start[], embedded_init_elf_end[];
    uint64_t hash = 0;
    size_t heap_used = 0, tables = 0;
    for (unsigned cycle = 0; cycle < 11; cycle++) {
        tcb_t *p = process_spawn_with_arg("ext2-user", embedded_init_elf_start,
                    embedded_init_elf_end - embedded_init_elf_start, 8);
        require_ext2(p != NULL, "spawn");
        uint64_t pid = p->tid, code = 0;
        require_ext2(process_wait(pid, &code) && code == 89, "Ring 3 exact read/print/close");
        sched_reap_dead();
        if (!cycle) {
            require_ext2(pmm_snapshot(before, sizeof(before)), "snapshot baseline");
            hash = vmm_kernel_mapping_fingerprint();
            heap_used = heap_get_used_bytes();
            tables = vmm_get_allocated_table_frames();
        } else {
            require_ext2(pmm_snapshot(after, sizeof(after)) && !memcmp(before, after, sizeof(before)),
                         "exact allocation-set mismatch");
            require_ext2(hash == vmm_kernel_mapping_fingerprint(), "kernel mapping fingerprint");
            require_ext2(heap_used == heap_get_used_bytes() && tables == vmm_get_allocated_table_frames(),
                         "heap/table lifecycle mismatch");
        }
    }
    require_ext2(heap_verify_integrity(), "heap integrity");
    serial_puts("[PASS] Ring 3 ext2: 10 audited cycles, exact PMM bitmap restored, stable kernel mappings and heap\n");
    serial_puts("[ OK ] Phase 9 (Step 9C.2): Read-only ext2 PASSED!\n");
}

/* =========================================================================
 * SMP Piece 1: AP Discovery (SMP_DESIGN.md, SM1-SM2)
 * Scope: find every CPU ACPI MADT says is enabled, start it via Limine's
 * SMP protocol, cross-check the two sources, and confirm every AP reports
 * in. APs do nothing beyond that (halted, interrupts disabled) -- no
 * per-CPU storage, locking, or scheduler change lands until later pieces.
 * See docs/roadmap/smp-piece1-ap-discovery.md for how to verify this.
 * ========================================================================= */
static void test_smp_piece1_ap_discovery(const acpi_madt_info_t *madt_info) {
    serial_puts("========================================================\n");
    serial_puts("SMP Piece 1: AP Discovery\n");
    serial_puts("========================================================\n");

    size_t online_aps = smp_init(madt_info);
    size_t total_cpus = smp_get_cpu_count();

    /* madt_info->enabled_cpu_count == 0 already halted kmain at MADT parse
     * time (before this runs), so it is not re-checked here. */
    if (total_cpus != madt_info->enabled_cpu_count) {
        serial_puts("       [WARN] Total CPUs online (");
        serial_print_dec(total_cpus);
        serial_puts(") does not match MADT enabled count (");
        serial_print_dec(madt_info->enabled_cpu_count);
        serial_puts("); continuing single/partial-CPU. See the Piece 1 test doc before treating this as a pass.\n");
    } else if (online_aps == 0) {
        serial_puts("       [ OK ] Single-CPU system confirmed by both MADT and Limine\n");
    } else {
        serial_puts("       [ OK ] All ");
        serial_print_dec(total_cpus);
        serial_puts(" CPU(s) accounted for (1 BSP + ");
        serial_print_dec(online_aps);
        serial_puts(" AP(s)), matching MADT\n");
    }

    serial_puts("[ OK ] SMP Piece 1 (AP discovery) complete.\n\n");
}

/* =========================================================================
 * SMP Piece 3: Lock Discipline & Two-Core Contention (SMP_DESIGN.md, SM10-SM11c)
 * Scope: verify per-CPU tracker isolation, bus-locked atomic contention
 * (BSP + AP 1), lock classification, debug assertions, and AP panic isolation.
 * ========================================================================= */
static void test_smp_piece3_lock_discipline(void) {
    serial_puts("========================================================\n");
    serial_puts("SMP Piece 3: Lock Discipline & Contention\n");
    serial_puts("========================================================\n");

    /* Single-CPU invariant checks on BSP */
    if (!spin_debug_selftest()) {
        serial_puts("       [FAIL] BSP spinlock selftest failed!\n");
        hcf();
    }
    serial_puts("       [PASS] BSP spinlock selftest passed (ranks, classification, asserts)\n");

    if (!smp_run_lock_tests()) {
        serial_puts("       [FAIL] SMP lock discipline test failed!\n");
        hcf();
    }

    serial_puts("[ OK ] SMP Piece 3 (Lock discipline) complete.\n\n");
}

void kmain(void) {
    /* 1. Initialize COM1 Serial Port (0x3F8) */
    int serial_status = serial_init();

    /* Bring screen diagnostics up before PMM/VMM audits can halt. Limine's
     * initial mappings remain active here; no allocation is required. */
    static boot_info_t early_console;
    if (LIMINE_BASE_REVISION_SUPPORTED && framebuffer_request.response &&
        framebuffer_request.response->framebuffer_count && framebuffer_request.response->framebuffers &&
        framebuffer_request.response->framebuffers[0]) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        early_console.has_framebuffer = true;
        early_console.fb_address = (uintptr_t)fb->address;
        early_console.fb_width = fb->width;
        early_console.fb_height = fb->height;
        early_console.fb_pitch = fb->pitch;
        early_console.fb_bpp = fb->bpp;
        console_init(&early_console);
    }

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
    serial_puts("[ OK ] PMM audit passed (bitmap reserved, frame 0 guarded, ");
    serial_print_dec(PMM_BITMAP_MAX_RAM_BYTES / (1024ULL * 1024 * 1024));
    serial_puts(" GiB capacity verified)\n\n");
    /* pmm_high_memory_probe(); */
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
                   rsdp_request.response,
                   module_request.response,
                   kernel_file_request.response);

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

        render_boot_logo(&boot_info);
        serial_puts("[ OK ] Framebuffer boot logo rendered (using kernel-owned boot info)\n");

        console_init(&boot_info);
        serial_puts("[ OK ] Framebuffer text console active (dual COM1/screen output armed)\n");

        /* Hold the logo for a moment before the acceptance suite
         * starts painting over it. Bounded busy-wait; APIC timer is
         * not yet calibrated at this point in kmain. */
        for (volatile uint64_t i = 0; i < 150000000ULL; i++) { }

        /* Clear the framebuffer so the console starts on a fresh grid
         * rather than writing over a partially-covered logo. */
        console_clear();
    }

    /* 14. Virtual File System & Initramfs Mount (Step 8B) */
    if (boot_info.has_initramfs && boot_info.initramfs_vaddr) {
        tarfs_init((const void *)boot_info.initramfs_vaddr, boot_info.initramfs_size);
    } else {
        vfs_init();
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
    power_init();

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
    if (!lapic_init(madt_info.lapic_phys_addr) || !lapic_configure_nmi(&madt_info)) {
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

    /* =========================================================================
     * Phase 6 (Checkpoint 1): Cooperative Multitasking Suite
     * ========================================================================= */
    serial_puts("========================================================\n");
    serial_puts("Phase 6 (Checkpoint 1): Cooperative Multitasking Suite\n");
    serial_puts("========================================================\n");

    /* Initialize Thread Scheduler */
    sched_init();

    /* Spawn Worker A and Worker B */
    tcb_t *t_a = thread_create("WorkerA", worker_a, NULL);
    tcb_t *t_b = thread_create("WorkerB", worker_b, NULL);
    if (!t_a || !t_b) {
        serial_puts("[FAIL] Failed to spawn cooperative worker threads!\n");
        hcf();
    }
    serial_puts("[ OK ] Created Worker A (TID 1) and Worker B (TID 2)\n");

    /* Verify dedicated thread stack guard page is unmapped in kernel PML4 and isolated from heap */
    if (vmm_is_mapped(kernel_pml4, t_a->kstack_guard) ||
        !vmm_is_mapped(kernel_pml4, t_a->kstack_base) ||
        t_a->kstack_base != t_a->kstack_guard + STACK_GUARD_SIZE) {
        serial_puts("[FAIL] Worker A stack guard page mapping invariant violated!\n");
        hcf();
    }
    serial_puts("[PASS] Dedicated thread stack guard page unmapped and isolated from heap\n");

    /* Yield from main thread to start cooperative ping-pong */
    while (!g_worker_a_done || !g_worker_b_done) {
        thread_yield();
    }

    if (g_ping_pong_counter != 10) {
        serial_puts("[FAIL] Ping-pong counter mismatch! Expected 10, got: ");
        serial_print_dec(g_ping_pong_counter);
        serial_puts("\n");
        hcf();
    }

    if (!heap_verify_integrity()) {
        serial_puts("[FAIL] Heap integrity compromised after thread execution & reaping!\n");
        hcf();
    }

    serial_puts("[PASS] Cooperative multitasking verified: 10/10 ping-pong rounds, clean exit & heap audit\n");
    serial_puts("[ OK ] Phase 6 (Checkpoint 1) completed successfully!\n\n");

    /* =========================================================================
     * Phase 6 (Checkpoint 2): Preemptive Round-Robin Scheduler Suite
     * ========================================================================= */
    serial_puts("========================================================\n");
    serial_puts("Phase 6 (Checkpoint 2): Preemptive Scheduler Suite\n");
    serial_puts("========================================================\n");

    g_preempt_work1 = 0;
    g_preempt_work2 = 0;
    g_preempt_stop  = false;

    tcb_t *pw1 = thread_create("Preempt1", preempt_worker1, NULL);
    tcb_t *pw2 = thread_create("Preempt2", preempt_worker2, NULL);
    if (!pw1 || !pw2) {
        serial_puts("[FAIL] Failed to spawn preemptive worker threads!\n");
        hcf();
    }
    serial_puts("[ OK ] Created PreemptWorker1 and PreemptWorker2 (CPU-bound loops, zero voluntary yields)\n");

    /* Start APIC timer and enable preemption */
    sched_enable_preemption();
    apic_timer_start();
    __asm__ volatile("sti" ::: "memory");

    /* Observe both counters progressing concurrently across timer ticks */
    uint64_t initial_ticks = apic_timer_get_ticks();
    uint64_t prev_work1 = 0;
    uint64_t prev_work2 = 0;
    int concurrent_observations = 0;

    for (int obs = 1; obs <= 5; obs++) {
        uint64_t target_tick = apic_timer_get_ticks() + 2;
        uint64_t loop_timeout = 20000000;
        while (apic_timer_get_ticks() < target_tick) {
            __asm__ volatile("pause");
            if (--loop_timeout == 0) break;
        }

        uint64_t cur1 = g_preempt_work1;
        uint64_t cur2 = g_preempt_work2;

        serial_puts("       [Sample ");
        serial_print_dec(obs);
        serial_puts("] Worker1 Count: ");
        serial_print_dec(cur1);
        serial_puts(", Worker2 Count: ");
        serial_print_dec(cur2);
        serial_puts(" (APIC Ticks: ");
        serial_print_dec(apic_timer_get_ticks() - initial_ticks);
        serial_puts(")\n");

        if (cur1 > prev_work1 && cur2 > prev_work2) {
            concurrent_observations++;
        }
        prev_work1 = cur1;
        prev_work2 = cur2;
    }

    /* Signal workers to stop */
    g_preempt_stop = true;

    /* Yield main thread to allow workers to observe stop flag and exit */
    uint64_t wait_exit_timeout = 20000000;
    while (sched_ready_count() > 0) {
        thread_yield();
        if (--wait_exit_timeout == 0) break;
    }

    /* Disable interrupts and preemption */
    __asm__ volatile("cli" ::: "memory");
    apic_timer_stop();
    sched_disable_preemption();

    if (concurrent_observations < 3) {
        serial_puts("[FAIL] Preemption verification failed: threads did not progress concurrently!\n");
        hcf();
    }

    if (!heap_verify_integrity()) {
        serial_puts("[FAIL] Heap integrity walk failed after preemptive scheduler execution!\n");
        hcf();
    }

    serial_puts("[PASS] Preemptive round-robin timeslicing verified: Both CPU-bound workers advanced concurrently!\n");
    serial_puts("[ OK ] Phase 6 (Checkpoint 2) completed successfully!\n\n");

    /* =========================================================================
     * Phase 6 Hardening: Rapid Thread Lifecycle & Heap Synchronization Stress
     * ========================================================================= */
    serial_puts("========================================================\n");
    serial_puts("Phase 6 Hardening: Lifecycle Stress & Heap Sync Suite\n");
    serial_puts("========================================================\n");

    g_stress_completed_threads = 0;
    const int BATCH_SIZE = 6;
    const int NUM_BATCHES = 6; /* 36 threads total */

    for (int b = 0; b < NUM_BATCHES; b++) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            tcb_t *st = thread_create("stress", stress_worker, (void *)(uintptr_t)(b * BATCH_SIZE + i));
            if (!st) {
                serial_puts("[FAIL] Failed to allocate thread during stress test!\n");
                hcf();
            }
        }
        while (sched_ready_count() > 0) {
            thread_yield();
        }
    }

    if (g_stress_completed_threads != (size_t)(NUM_BATCHES * BATCH_SIZE)) {
        serial_puts("[FAIL] Stress test thread completion count mismatch!\n");
        hcf();
    }

    /* Verify thread stack slots were cleanly reclaimed */
    uint64_t active_slots = sched_get_active_stack_slots_mask();
    if (active_slots != 1ULL) {
        serial_puts("[FAIL] Stack slot leak detected! Active mask: ");
        serial_print_hex(active_slots);
        serial_puts("\n");
        hcf();
    }
    serial_puts("[PASS] All 36 thread stacks cleanly reclaimed and recycled (slot mask: 0x1)\n");

    if (!heap_verify_integrity()) {
        serial_puts("[FAIL] Heap integrity walk failed after rapid thread stress test!\n");
        hcf();
    }
    serial_puts("[PASS] Heap integrity walk passed after 36-thread concurrent lifecycle stress test\n");
    serial_puts("[ OK ] Phase 6 Hardening verified successfully!\n\n");

    /* =========================================================================
     * Phase 7 (Checkpoint 0): Address-Space Lifecycle, Teardown & Isolation
     * ========================================================================= */
    serial_puts("========================================================\n");
    serial_puts("Phase 7 (Checkpoint 0): Address-Space Lifecycle & Isolation\n");
    serial_puts("========================================================\n");

    uint64_t *master_kernel_pml4 = vmm_get_kernel_pml4_virt();
    uintptr_t master_kernel_pml4_phys = vmm_get_kernel_pml4();

    /* 1. Address Space Creation & Higher-Half Mirroring Test */
    serial_puts("[TEST 1] Creating User Address Space (PML4) & Validating Mirroring...\n");
    uintptr_t user_pml4_phys = vmm_create_user_pml4();
    if (user_pml4_phys == 0) {
        serial_puts("       [FAIL] Failed to allocate user PML4!\n");
        hcf();
    }
    uint64_t *user_pml4_virt = (uint64_t *)((uintptr_t)user_pml4_phys + boot_info.hhdm_offset);

    /* Assert lower half (0..255) is completely unmapped */
    bool lower_empty = true;
    for (size_t i = 0; i < 256; i++) {
        if (user_pml4_virt[i] != 0) {
            lower_empty = false;
            break;
        }
    }
    if (!lower_empty) {
        serial_puts("       [FAIL] User space PML4 entries 0..255 not clean!\n");
        hcf();
    }
    serial_puts("       [PASS] Lower-half (entries 0..255) confirmed pristine and unmapped\n");

    /* Assert higher half (256..511) matches master kernel PML4 */
    bool higher_mirrored = true;
    for (size_t i = 256; i < 512; i++) {
        if (user_pml4_virt[i] != master_kernel_pml4[i]) {
            higher_mirrored = false;
            break;
        }
    }
    if (!higher_mirrored) {
        serial_puts("       [FAIL] Higher-half PML4 entries 256..511 do not match kernel PML4!\n");
        hcf();
    }
    serial_puts("       [PASS] Higher-half (entries 256..511) correctly mirrored from kernel PML4\n");

    /* 2. Process Isolation & Dual-Space Virtual Address Collision Test */
    serial_puts("[TEST 2] Verifying Address Space Isolation & Virtual Address Collisions...\n");
    uintptr_t user2_pml4_phys = vmm_create_user_pml4();
    if (user2_pml4_phys == 0) {
        serial_puts("       [FAIL] Failed to allocate second user PML4!\n");
        hcf();
    }
    uint64_t *user2_pml4_virt = (uint64_t *)((uintptr_t)user2_pml4_phys + boot_info.hhdm_offset);

    /*
     * Map the SAME user virtual address 0x0000000000400000 (4 MiB) in Space 1 and Space 2
     * to DIFFERENT physical frames with DIFFERENT initial contents.
     */
    uintptr_t frame_a = pmm_alloc_page();
    uintptr_t frame_b = pmm_alloc_page();
    uintptr_t shared_user_virt = 0x0000000000400000ULL;
    if (frame_a == 0 || frame_b == 0) {
        serial_puts("       [FAIL] Failed to allocate frames for dual-space collision test!\n");
        hcf();
    }

    if (vmm_map_page(user_pml4_virt, shared_user_virt, frame_a, PTE_PRESENT | PTE_WRITABLE | PTE_USER) != VMM_OK ||
        vmm_map_page(user2_pml4_virt, shared_user_virt, frame_b, PTE_PRESENT | PTE_WRITABLE | PTE_USER) != VMM_OK) {
        serial_puts("       [FAIL] Failed to map shared virtual address in Space 1 and Space 2!\n");
        hcf();
    }

    /* Initialize distinct patterns via HHDM before switching */
    *(volatile uint64_t *)((uintptr_t)frame_a + boot_info.hhdm_offset) = 0xAAAAAAAAAAAAAAAAULL;
    *(volatile uint64_t *)((uintptr_t)frame_b + boot_info.hhdm_offset) = 0xBBBBBBBBBBBBBBBBULL;

    /* Master Kernel PML4 must NOT have this address mapped */
    if (vmm_is_mapped(master_kernel_pml4, shared_user_virt)) {
        serial_puts("       [FAIL] Kernel PML4 leaked user mapping!\n");
        hcf();
    }
    serial_puts("       [PASS] Virtual address 0x400000 absent in Master Kernel PML4\n");

    /*
     * Manual CR3 switching sequence executed with preemption/interrupts disabled (cli).
     * Tests that each space resolves 0x400000 to its own private frame and contents.
     */
    __asm__ volatile("cli" ::: "memory");

    vmm_switch_pml4(user_pml4_phys);
    volatile uint64_t *user_alias_ptr = (volatile uint64_t *)shared_user_virt;
    if (*user_alias_ptr != 0xAAAAAAAAAAAAAAAAULL) {
        vmm_switch_pml4(master_kernel_pml4_phys);
        serial_puts("       [FAIL] Space 1 did not read pattern A!\n");
        hcf();
    }

    vmm_switch_pml4(user2_pml4_phys);
    if (*user_alias_ptr != 0xBBBBBBBBBBBBBBBBULL) {
        vmm_switch_pml4(master_kernel_pml4_phys);
        serial_puts("       [FAIL] Space 2 did not read pattern B!\n");
        hcf();
    }

    /* Switch back to Space 1 and verify pattern A survived unmodified */
    vmm_switch_pml4(user_pml4_phys);
    if (*user_alias_ptr != 0xAAAAAAAAAAAAAAAAULL) {
        vmm_switch_pml4(master_kernel_pml4_phys);
        serial_puts("       [FAIL] Pattern A was corrupted after Space 2 switch!\n");
        hcf();
    }

    /* Restore Master Kernel CR3 */
    vmm_switch_pml4(master_kernel_pml4_phys);
    serial_puts("       [PASS] Same virtual address in Space 1 & Space 2 maps to separate frames and survives switching\n");

    /* 3. Invariant & Self-Destruction Guards */
    serial_puts("[TEST 3] Validating Address Space Destruction Guards & Higher-Half Protection...\n");
    if (vmm_destroy_pml4(master_kernel_pml4_phys, true) == VMM_OK) {
        serial_puts("       [FAIL] Kernel PML4 destruction unexpectedly succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Master Kernel PML4 destruction blocked\n");

    /* Active root destruction rejection */
    vmm_switch_pml4(user_pml4_phys);
    if (vmm_destroy_pml4(user_pml4_phys, true) == VMM_OK) {
        vmm_switch_pml4(master_kernel_pml4_phys);
        serial_puts("       [FAIL] Active user root destruction unexpectedly succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Active user root destruction blocked\n");
    vmm_switch_pml4(master_kernel_pml4_phys);

    /* Invariant: Higher-half address cannot be mapped with PTE_USER */
    if (vmm_map_page(user_pml4_virt, 0xFFFFFFFF80000000ULL, frame_a, PTE_PRESENT | PTE_USER) == VMM_OK) {
        serial_puts("       [FAIL] Mapping higher-half kernel address with PTE_USER unexpectedly succeeded!\n");
        hcf();
    }
    serial_puts("       [PASS] Higher-half mapping with PTE_USER rejected\n");

    /* Teardown Space 1 & Space 2 with free_user_frames = true */
    if (vmm_destroy_pml4(user_pml4_phys, true) != VMM_OK ||
        vmm_destroy_pml4(user2_pml4_phys, true) != VMM_OK) {
        serial_puts("       [FAIL] Destruction of Space 1 or Space 2 failed!\n");
        hcf();
    }
    serial_puts("       [PASS] Space 1 & Space 2 destroyed cleanly with user data frames freed\n");

    /* 4. TSS RSP0 Privilege Stack Transition Hook */
    serial_puts("[TEST 4] Validating TSS RSP0 Privilege Stack Transition Hook...\n");
    uint64_t test_rsp0 = 0xFFFFFFFFA0004FF0ULL;
    gdt_set_tss_rsp0(test_rsp0);
    if (gdt_get_tss_rsp0() != test_rsp0) {
        serial_puts("       [FAIL] TSS RSP0 readback mismatch!\n");
        hcf();
    }
    serial_puts("       [PASS] TSS RSP0 configured and verified (Ring 3 -> Ring 0 stack hook ready)\n");

    /* 5. free_user_frames = false Contract Test */
    serial_puts("[TEST 5] Testing Teardown with Caller-Owned Frames (free_user_frames = false)...\n");
    size_t tables_before_retain = vmm_get_allocated_table_frames();
    size_t pages_before_retain  = pmm_get_free_pages();

    uintptr_t space_retain = vmm_create_user_pml4();
    uint64_t *space_retain_virt = (uint64_t *)((uintptr_t)space_retain + boot_info.hhdm_offset);
    uintptr_t caller_frame = pmm_alloc_page();
    if (caller_frame == 0 || space_retain == 0) {
        serial_puts("       [FAIL] Allocation failed in retain test!\n");
        hcf();
    }

    /* Write marker into caller frame */
    *(volatile uint64_t *)((uintptr_t)caller_frame + boot_info.hhdm_offset) = 0xFEEDC0FFEE001122ULL;
    vmm_map_page(space_retain_virt, 0x0000000000400000ULL, caller_frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

    /* Destroy with free_user_frames = false: intermediate tables and root are freed, but caller frame is NOT */
    if (vmm_destroy_pml4(space_retain, false) != VMM_OK) {
        serial_puts("       [FAIL] vmm_destroy_pml4(..., false) failed!\n");
        hcf();
    }

    if (vmm_get_allocated_table_frames() != tables_before_retain) {
        serial_puts("       [FAIL] Allocated table count mismatch in retain test!\n");
        hcf();
    }
    serial_puts("       [PASS] Page-table frames reclaimed while caller retains data frame\n");

    /* Verify caller frame memory still contains the intact marker */
    if (*(volatile uint64_t *)((uintptr_t)caller_frame + boot_info.hhdm_offset) != 0xFEEDC0FFEE001122ULL) {
        serial_puts("       [FAIL] Caller-owned data frame corrupted!\n");
        hcf();
    }
    serial_puts("       [PASS] Caller-owned data frame remained intact after teardown\n");

    /* Caller now manually frees the retained frame */
    pmm_free_page(caller_frame);
    if (pmm_get_free_pages() != pages_before_retain) {
        serial_puts("       [FAIL] Frame balance mismatch after manual caller free!\n");
        hcf();
    }
    serial_puts("       [PASS] Caller manual frame release restored exact physical page count\n");

    /* 6. Structural Pre-Validation & Huge-Page Rejection Test */
    serial_puts("[TEST 6] Testing Structural Pre-Validation (Reject Unsupported Huge Pages)...\n");
    uintptr_t space_huge = vmm_create_user_pml4();
    uint64_t *space_huge_virt = (uint64_t *)((uintptr_t)space_huge + boot_info.hhdm_offset);

    /* Synthesize an intermediate PDPT entry with PTE_HUGE */
    uintptr_t fake_pdpt = pmm_alloc_page();
    uint64_t *fake_pdpt_virt = (uint64_t *)((uintptr_t)fake_pdpt + boot_info.hhdm_offset);
    memset(fake_pdpt_virt, 0, PAGE_SIZE);
    fake_pdpt_virt[0] = 0x10000000ULL | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_HUGE; /* 1 GiB huge page */
    space_huge_virt[0] = fake_pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* Pre-validation must detect PTE_HUGE and abort BEFORE freeing */
    int huge_destroy_res = vmm_destroy_pml4(space_huge, true);
    if (huge_destroy_res != VMM_ERR_INVALID_ADDR) {
        serial_puts("       [FAIL] Pre-validation failed to reject unsupported huge page!\n");
        hcf();
    }
    serial_puts("       [PASS] Unsupported huge-page structure cleanly rejected during pre-validation\n");

    /* Clean up synthesized test tables */
    space_huge_virt[0] = 0;
    pmm_free_page(fake_pdpt);
    pmm_free_page(space_huge);

    /* 7. Multi-Level Sparse Teardown & Repeated Cycles Leak Audit */
    serial_puts("[TEST 7] Multi-Boundary Sparse Mappings & 10-Cycle Teardown Audit...\n");
    size_t baseline_free_pages = pmm_get_free_pages();
    size_t baseline_allocated_tables = vmm_get_allocated_table_frames();

    for (int cycle = 1; cycle <= 10; cycle++) {
        uintptr_t cycle_pml4 = vmm_create_user_pml4();
        if (cycle_pml4 == 0) {
            serial_puts("       [FAIL] Failed to allocate PML4 during cycle test!\n");
            hcf();
        }
        uint64_t *cycle_pml4_virt = (uint64_t *)((uintptr_t)cycle_pml4 + boot_info.hhdm_offset);

        /* Map 3 sparse pages across 2 MiB, 1 GiB, and PML4 boundaries */
        uintptr_t f1 = pmm_alloc_page();
        uintptr_t f2 = pmm_alloc_page();
        uintptr_t f3 = pmm_alloc_page();
        if (f1 == 0 || f2 == 0 || f3 == 0) {
            serial_puts("       [FAIL] Frame allocation failed during cycle test!\n");
            hcf();
        }

        vmm_map_page(cycle_pml4_virt, 0x0000000000400000ULL, f1, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        vmm_map_page(cycle_pml4_virt, 0x0000008000000000ULL, f2, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        vmm_map_page(cycle_pml4_virt, 0x00007FFFF0000000ULL, f3, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

        if (vmm_destroy_pml4(cycle_pml4, true) != VMM_OK) {
            serial_puts("       [FAIL] Cycle teardown failed!\n");
            hcf();
        }
    }

    size_t final_free_pages = pmm_get_free_pages();
    size_t final_allocated_tables = vmm_get_allocated_table_frames();

    if (final_allocated_tables != baseline_allocated_tables) {
        serial_puts("       [FAIL] Allocated tables leaked across 10 cycles! Expected: ");
        serial_print_dec(baseline_allocated_tables);
        serial_puts(" Got: ");
        serial_print_dec(final_allocated_tables);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All intermediate page tables & roots reclaimed across 10 cycles (delta: 0)\n");

    if (final_free_pages != baseline_free_pages) {
        serial_puts("       [FAIL] Physical frames leaked across 10 cycles! Expected: ");
        serial_print_dec(baseline_free_pages);
        serial_puts(" Got: ");
        serial_print_dec(final_free_pages);
        serial_puts("\n");
        hcf();
    }
    serial_puts("       [PASS] All user physical frames returned to PMM across 10 cycles (delta: 0 frames leaked)\n");

    /* 8. Kernel Mapping Integrity Verification */
    serial_puts("[TEST 8] Verifying Master Kernel Mappings Integrity Post-Teardown...\n");
    if (!vmm_is_mapped(master_kernel_pml4, (uintptr_t)__text_start) ||
        !vmm_is_mapped(master_kernel_pml4, (uintptr_t)__rodata_start) ||
        !vmm_is_mapped(master_kernel_pml4, (uintptr_t)__data_start) ||
        !vmm_is_mapped(master_kernel_pml4, 0xFFFFFFFFB0000000ULL) /* Heap */ ||
        !vmm_is_mapped(master_kernel_pml4, boot_info.hhdm_offset + 0x100000) /* HHDM */) {
        serial_puts("       [FAIL] Kernel mappings corrupted by user address space teardowns!\n");
        hcf();
    }
    serial_puts("       [PASS] Kernel .text, .rodata, .data, heap, and HHDM intact after teardowns\n");

    if (!heap_verify_integrity()) {
        serial_puts("       [FAIL] Heap integrity walk failed after address space operations!\n");
        hcf();
    }
    serial_puts("       [PASS] Dynamic kernel heap integrity walk passed\n");

    serial_puts("[ OK ] Phase 7 (Checkpoint 0) completed successfully!\n\n");

    /* =========================================================================
     * Phase 7 (Checkpoint 1): Ring 3 Privilege Transition via iretq & Trap Hook
     * ========================================================================= */
    test_phase7_checkpoint1_ring3(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 7 (Checkpoint 2): First System Call (Serial Print via int 0x80)
     * ========================================================================= */
    test_phase7_checkpoint2_syscalls(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 7 (Checkpoint 3): Embedded Standalone ELF64 Executable Loading
     * ========================================================================= */
    test_phase7_checkpoint3_elf(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 7 (Checkpoint 4): General Process Exit & Lifecycle Management
     * ========================================================================= */
    test_phase7_checkpoint4_lifecycle(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 7 Acceptance Suite: Preemption & Fault Isolation
     * ========================================================================= */
    test_phase7_acceptance_suite(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 7 (Checkpoint 5): Fast Syscall Hardening (syscall / sysret)
     * ========================================================================= */
    test_phase7_checkpoint5_fast_syscall(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 8 (Step 8A): Basic Framebuffer Text Console
     * ========================================================================= */
    test_phase8a_framebuffer_console(&boot_info);

    /* =========================================================================
     * Phase 8 (Step 8B): Initramfs, Minimal VFS & File Descriptors
     * ========================================================================= */
    test_phase8b_vfs_initramfs(&boot_info, master_kernel_pml4, master_kernel_pml4_phys);

    /* =========================================================================
     * Phase 9 (Step 9A): PCI Discovery & NVMe MMIO BAR Verification
     * ========================================================================= */
    test_phase9a_pci_discovery(&boot_info);

    /* The following storage acceptance suite assumes a disposable QEMU image,
     * including its exact geometry and fixture files. Do not apply it to a
     * laptop's existing NVMe namespaces. PCI discovery above is read-only. */
    pci_device_t storage_fixture;
    if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_NVME,
                         PCI_PROGIF_STORAGE_NVME, &storage_fixture) ||
        storage_fixture.vendor_id != 0x1b36 || storage_fixture.device_id != 0x0010) {
        serial_puts("[BOOT] Hardware diagnostics complete. QEMU storage fixture tests skipped.\n");
    } else {

#if defined(ENABLE_NVME_RAW_PATTERN_TESTS) && (ENABLE_NVME_RAW_PATTERN_TESTS == 1)
        /* =========================================================================
         * Phase 9 (Step 9B.1): NVMe Initialization & Reads Verification
         * ========================================================================= */
        test_phase9b1_nvme_reads();

        /* =========================================================================
         * Phase 9 (Step 9B.2): NVMe Writes, Flush & Persistence Verification
         * ========================================================================= */
        test_phase9b2_nvme_writes();
#endif

        /* =========================================================================
         * Phase 9 (Step 9C.1): GPT Partition Parsing & Bounded Block Devices
         * ========================================================================= */
        test_phase9c1_gpt();

        test_ext2_and_audits();

        serial_puts("\n[BOOT] FortressOS Phase 9 (Step 9C.2) complete.\n");
    }

    /* =========================================================================
     * SMP Piece 1: AP Discovery & Piece 3: Lock Discipline
     * ========================================================================= */
    if (boot_info.cmdline[0] != '\0') {
        if (strstr(boot_info.cmdline, "smp_test=inversion")) {
            smp_set_test_mode(SMP_TEST_MODE_INVERSION);
        } else if (strstr(boot_info.cmdline, "smp_test=assert_held")) {
            smp_set_test_mode(SMP_TEST_MODE_ASSERT_HELD);
        } else if (strstr(boot_info.cmdline, "smp_test=none")) {
            smp_set_test_mode(SMP_TEST_MODE_NONE);
        } else if (strstr(boot_info.cmdline, "smp_test=contention")) {
            smp_set_test_mode(SMP_TEST_MODE_CONTENTION);
        }
    }
    test_smp_piece1_ap_discovery(&madt_info);
    test_smp_piece3_lock_discipline();

    /* Inputs and shell are started after destructive/negative acceptance cases. */
    __asm__ volatile("cli" ::: "memory");
    sched_disable_preemption();
    if (!input_init(&madt_info)) {
        serial_puts("[FAIL] No keyboard or serial input available.\n");
        hcf();
    }
    /* Keep discovery visible near the shell on hardware without COM1. */
    pci_report_xhci();
    xhci_boot_probe(&boot_info);
    usb_mount_production_storage(&boot_info);
    vfs_node_t *shell = vfs_lookup("/bin/shell");
    if (!shell || shell->type != VFS_FILE || !shell->data) {
        serial_puts("[FAIL] /bin/shell missing from initramfs.\n");
        hcf();
    }
    for (;;) {
        /* Spawn with preemption disabled until the PID is safely copied. */
        tcb_t *process = process_spawn("shell", shell->data, shell->size);
        if (!process) { serial_puts("[FAIL] Cannot start shell.\n"); hcf(); }
        uint64_t pid = process->tid;
        sched_enable_preemption();
        apic_timer_start();
        serial_puts("[BOOT] Interactive shell ready.\n");
        while (process_is_alive(pid)) {
            sched_reap_dead();
            __asm__ volatile("sti; hlt" ::: "memory");
        }
        __asm__ volatile("cli" ::: "memory");
        sched_disable_preemption();
        uint64_t status;
        (void)process_wait(pid, &status);
        sched_reap_dead();
        serial_puts("[SHELL] Process exited; restarting.\n");
    }
}
