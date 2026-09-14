#include "types.h"
#include "limine.h"
#include "serial.h"

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

    /* Draw test color bars at the top */
    uint32_t colors[6] = {
        0x00F7768E, /* Red */
        0x009ECE6A, /* Green */
        0x007AA2F7, /* Blue */
        0x00E0AF68, /* Yellow */
        0x00BB9AF7, /* Purple */
        0x007DCFFF  /* Cyan */
    };

    uint64_t bar_height = 24;
    uint64_t bar_width = width / 6;

    for (int c = 0; c < 6; c++) {
        uint64_t start_x = c * bar_width;
        uint64_t end_x = (c == 5) ? width : start_x + bar_width;

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
    serial_puts("[ OK ] Limine Base Revision 3 handshake verified\n");

    /* 3. Higher Half Direct Map (HHDM) */
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
        serial_puts(" MiB\n");
    }

    /* 5. Framebuffer Initialization & Test Pattern */
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
