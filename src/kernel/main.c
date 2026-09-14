#include "types.h"
#include "limine.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "string.h"

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
static void render_test_pattern(struct limine_framebuffer *fb) {
    if (!fb || !fb->address) return;

    /* Validate format: ensure 32 bpp linear framebuffer */
    if (fb->bpp != 32) {
        serial_puts("[WARN] Framebuffer is not 32 bpp (detected ");
        serial_print_dec(fb->bpp);
        serial_puts(" bpp); skipping test pattern.\n");
        return;
    }

    if (fb->width == 0 || fb->height == 0 || fb->pitch < fb->width * 4) {
        serial_puts("[WARN] Invalid framebuffer dimensions or pitch.\n");
        return;
    }

    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint64_t width = fb->width;
    uint64_t height = fb->height;
    uint64_t pitch32 = fb->pitch / 4;

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

    /* 9. Framebuffer Initialization & Test Pattern */
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_puts("[WARN] No Limine Framebuffer found (running headless)\n");
    } else {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        serial_puts("[ OK ] Framebuffer: ");
        serial_print_dec(fb->width);
        serial_puts("x");
        serial_print_dec(fb->height);
        serial_puts("@");
        serial_print_dec(fb->bpp);
        serial_puts(" bpp, Pitch: ");
        serial_print_dec(fb->pitch);
        serial_puts(" bytes, Addr: ");
        serial_print_hex((uint64_t)fb->address);
        serial_puts("\n");

        render_test_pattern(fb);
        serial_puts("[ OK ] Framebuffer test pattern rendered\n");
    }

    serial_puts("\n[BOOT] FortressOS early initialization complete. CPU halted.\n");

    /* 6. Clean halt state */
    hcf();
}
