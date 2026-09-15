#include "gdt.h"
#include "serial.h"

#define GDT_ENTRY_COUNT 7

/* Assembly helper defined in gdt_flush.asm */
extern void gdt_flush(gdt_ptr_t *ptr, uint16_t cs, uint16_t ds, uint16_t tss_sel);

/* Unified IST1 emergency stack layout: 4096-byte guard page strictly placed directly below 16 KiB stack */
struct ist1_layout {
    uint8_t guard[4096];
    uint8_t stack[16384];
} __attribute__((aligned(4096)));

static struct ist1_layout ist1_memory;

_Static_assert(sizeof(ist1_memory.guard) == 4096, "ist1 guard size must be 4096 bytes");
_Static_assert(sizeof(ist1_memory.stack) == 16384, "ist1 stack size must be 16384 bytes");
_Static_assert(__builtin_offsetof(struct ist1_layout, stack) == 4096, "ist1 stack must immediately follow guard page");
_Static_assert(_Alignof(struct ist1_layout) == 4096, "ist1 layout must be page aligned");

uintptr_t gdt_get_ist1_guard(void) {
    return (uintptr_t)ist1_memory.guard;
}

uintptr_t gdt_get_ist1_stack_top(void) {
    return (uintptr_t)ist1_memory.stack + sizeof(ist1_memory.stack);
}

/* Static TSS instance */
static tss_t tss __attribute__((aligned(16)));

/* Static GDT table (7 entries: Null, Kernel Code, Kernel Data, User Data, User Code, TSS Low, TSS High) */
static struct {
    gdt_entry_t entries[5];
    tss_descriptor_t tss;
} __attribute__((packed)) gdt_table;

static gdt_ptr_t gdt_descriptor;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    gdt_table.entries[index].base_low         = (uint16_t)(base & 0xFFFF);
    gdt_table.entries[index].base_middle      = (uint8_t)((base >> 16) & 0xFF);
    gdt_table.entries[index].base_high        = (uint8_t)((base >> 24) & 0xFF);
    gdt_table.entries[index].limit_low        = (uint16_t)(limit & 0xFFFF);
    gdt_table.entries[index].flags_limit_high = (uint8_t)((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt_table.entries[index].access           = access;
}

static void gdt_set_tss(uint64_t base, uint32_t limit) {
    gdt_table.tss.length        = (uint16_t)(limit & 0xFFFF);
    gdt_table.tss.base_low      = (uint16_t)(base & 0xFFFF);
    gdt_table.tss.base_mid      = (uint8_t)((base >> 16) & 0xFF);
    gdt_table.tss.flags1        = 0x89; /* Present, Ring 0, 64-bit TSS (Available) */
    gdt_table.tss.flags2        = (uint8_t)((limit >> 16) & 0x0F);
    gdt_table.tss.base_high_mid = (uint8_t)((base >> 24) & 0xFF);
    gdt_table.tss.base_high     = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    gdt_table.tss.reserved      = 0;
}

void gdt_init(void) {
    /* 1. Clear TSS */
    uint8_t *tss_bytes = (uint8_t *)&tss;
    for (size_t i = 0; i < sizeof(tss_t); i++) {
        tss_bytes[i] = 0;
    }

    /* 2. Configure IST1 emergency stack for Double Fault (#DF) */
    tss.ist[0] = gdt_get_ist1_stack_top();
    tss.iopb_offset = (uint16_t)sizeof(tss_t); /* Disable I/O bitmap */

    /* 3. Populate GDT */
    /* Entry 0: Null Descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Entry 1: 64-bit Kernel Code (0x08) - Executable, Readable, Long Mode */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0x20);

    /* Entry 2: 64-bit Kernel Data (0x10) - Writable */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0x00);

    /* Entry 3: 64-bit User Data (0x18) - Ring 3, Writable */
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0x00);

    /* Entry 4: 64-bit User Code (0x20) - Ring 3, Executable, Long Mode */
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0x20);

    /* Entry 5 & 6: 16-byte 64-bit TSS Descriptor (0x28) */
    gdt_set_tss((uint64_t)&tss, sizeof(tss_t) - 1);

    /* 4. Prepare GDT Pointer */
    gdt_descriptor.limit = (uint16_t)(sizeof(gdt_table) - 1);
    gdt_descriptor.base  = (uint64_t)&gdt_table;

    /* 5. Load GDT, reload segments, and load Task Register */
    gdt_flush(&gdt_descriptor, GDT_KERNEL_CODE, GDT_KERNEL_DATA, GDT_TSS);

    serial_puts("[ OK ] GDT loaded (Kernel CS: 0x08, Kernel DS: 0x10)\n");
    serial_puts("[ OK ] TSS loaded (Selector: 0x28, IST1 top: ");
    serial_print_hex(tss.ist[0]);
    serial_puts(")\n");
}
