#include "gdt.h"
#include "serial.h"
#include "percpu.h"

#define GDT_ENTRY_COUNT 7

/* Assembly helper defined in gdt_flush.asm */
extern void gdt_flush(gdt_ptr_t *ptr, uint16_t cs, uint16_t ds, uint16_t tss_sel);

/* Unified IST1 emergency stack layout: 4096-byte guard page strictly placed directly below 16 KiB stack */
struct ist1_layout {
    uint8_t guard[4096];
    uint8_t stack[16384];
} __attribute__((aligned(4096)));

static struct ist1_layout ist1_memory[MAX_DETECTED_CPUS];

_Static_assert(sizeof(ist1_memory[0].guard) == 4096, "ist1 guard size must be 4096 bytes");
_Static_assert(sizeof(ist1_memory[0].stack) == 16384, "ist1 stack size must be 16384 bytes");
_Static_assert(__builtin_offsetof(struct ist1_layout, stack) == 4096, "ist1 stack must immediately follow guard page");
_Static_assert(_Alignof(struct ist1_layout) == 4096, "ist1 layout must be page aligned");

uintptr_t gdt_get_ist1_guard(void) {
    return (uintptr_t)ist1_memory[cpu_current()->id].guard;
}

uintptr_t gdt_get_ist1_stack_top(void) {
    return (uintptr_t)ist1_memory[cpu_current()->id].stack + sizeof(ist1_memory[0].stack);
}

/* Unified IST2 emergency stack layout for Non-Maskable Interrupts (NMI):
 * Guarantees that NMIs firing during syscall entry/exit stack switches land safely on IST2. */
struct ist2_layout {
    uint8_t guard[4096];
    uint8_t stack[16384];
} __attribute__((aligned(4096)));

static struct ist2_layout ist2_memory[MAX_DETECTED_CPUS];

_Static_assert(sizeof(ist2_memory[0].guard) == 4096, "ist2 guard size must be 4096 bytes");
_Static_assert(sizeof(ist2_memory[0].stack) == 16384, "ist2 stack size must be 16384 bytes");
_Static_assert(__builtin_offsetof(struct ist2_layout, stack) == 4096, "ist2 stack must immediately follow guard page");
_Static_assert(_Alignof(struct ist2_layout) == 4096, "ist2 layout must be page aligned");

uintptr_t gdt_get_ist2_guard(void) {
    return (uintptr_t)ist2_memory[cpu_current()->id].guard;
}

uintptr_t gdt_get_ist2_stack_top(void) {
    return (uintptr_t)ist2_memory[cpu_current()->id].stack + sizeof(ist2_memory[0].stack);
}

/* Static TSS instance */
static tss_t cpu_tss[MAX_DETECTED_CPUS] __attribute__((aligned(16)));
#define local_tss (cpu_tss[cpu_current()->id])

/* Static GDT table (7 entries: Null, Kernel Code, Kernel Data, User Data, User Code, TSS Low, TSS High) */
static struct {
    gdt_entry_t entries[5];
    tss_descriptor_t tss;
} __attribute__((packed)) cpu_gdt[MAX_DETECTED_CPUS];
#define gdt_table (cpu_gdt[cpu_current()->id])

static gdt_ptr_t cpu_gdtr[MAX_DETECTED_CPUS];
#define gdt_descriptor (cpu_gdtr[cpu_current()->id])

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

void gdt_init_cpu(size_t id) {
    cpu_install(id);
    /* 1. Clear TSS */
    uint8_t *tss_bytes = (uint8_t *)&local_tss;
    for (size_t i = 0; i < sizeof(tss_t); i++) {
        tss_bytes[i] = 0;
    }

    /* 2. Configure IST1 emergency stack for Double Fault (#DF) and IST2 for NMI */
    local_tss.ist[0] = gdt_get_ist1_stack_top();
    local_tss.ist[1] = gdt_get_ist2_stack_top();
    local_tss.iopb_offset = (uint16_t)sizeof(tss_t); /* Disable I/O bitmap */

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
    gdt_set_tss((uint64_t)&local_tss, sizeof(tss_t) - 1);

    /* 4. Prepare GDT Pointer */
    gdt_descriptor.limit = (uint16_t)(sizeof(gdt_table) - 1);
    gdt_descriptor.base  = (uint64_t)&gdt_table;

    /* 5. Load GDT, reload segments, and load Task Register */
    gdt_flush(&gdt_descriptor, GDT_KERNEL_CODE, GDT_KERNEL_DATA, GDT_TSS);

    cpu_install(id); /* Loading GS selector in gdt_flush resets its base. */
    if (id != 0) return;

    serial_puts("[ OK ] GDT loaded (Kernel CS: 0x08, Kernel DS: 0x10)\n");
    serial_puts("[ OK ] TSS loaded (Selector: 0x28, IST1 top: ");
    serial_print_hex(local_tss.ist[0]);
    serial_puts(")\n");
}

void gdt_init(void) { gdt_init_cpu(0); }

void gdt_set_tss_rsp0(uint64_t rsp0) {
    local_tss.rsp[0] = rsp0;
    cpu_current()->rsp0 = rsp0;
}

uint64_t gdt_get_tss_rsp0(void) {
    return local_tss.rsp[0];
}


/* BSP prepares every AP guard before any AP can use the kernel CR3. */
uintptr_t gdt_cpu_ist_guard(size_t id, unsigned ist) {
    return ist == 1 ? (uintptr_t)ist1_memory[id].guard
                    : (uintptr_t)ist2_memory[id].guard;
}

bool gdt_cpu_is_local(void) {
    gdt_ptr_t actual;
    uint16_t selector;
    __asm__ volatile("sgdt %0" : "=m"(actual));
    __asm__ volatile("str %0" : "=r"(selector));
    return actual.base == (uintptr_t)&gdt_table &&
           actual.limit == sizeof(gdt_table) - 1 && selector == GDT_TSS &&
           gdt_table.tss.flags1 == 0x8b; /* CPU marked this TSS busy. */
}
