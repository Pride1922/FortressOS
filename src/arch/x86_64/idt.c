#include "idt.h"
#include "serial.h"

extern void idtr_load(idt_ptr_t *ptr);
extern uint8_t isr_stub_table[];

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idtr;

static const char *exception_messages[32] = {
    "Divide-by-zero (#DE)",
    "Debug (#DB)",
    "Non-maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 Floating-Point Exception (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception (#HV)",
    "VMM Communication Exception (#VC)",
    "Security Exception (#SX)",
    "Reserved"
};

void idt_set_gate(uint8_t vector, void *handler, uint8_t ist, uint8_t type_attributes) {
    uint64_t addr = (uint64_t)handler;

    idt[vector].offset_low       = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector         = 0x08; /* Kernel Code Segment */
    idt[vector].ist              = (uint8_t)(ist & 0x07);
    idt[vector].type_attributes  = type_attributes;
    idt[vector].offset_mid       = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high      = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved         = 0;
}

void idt_init(void) {
    /* 1. Clear IDT */
    uint8_t *idt_bytes = (uint8_t *)idt;
    for (size_t i = 0; i < sizeof(idt); i++) {
        idt_bytes[i] = 0;
    }

    /* 2. Populate all 256 vectors with distinct 16-byte aligned assembly stubs */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        uint8_t ist = 0;

        /* Stack Switching: Double Fault (#DF, Vector 8) MUST use IST1 */
        if (i == 8) {
            ist = 1;
        }

        void *handler = (void *)((uintptr_t)isr_stub_table + (i * 16));
        idt_set_gate((uint8_t)i, handler, ist, IDT_GATE_INTERRUPT);
    }

    /* 3. Load IDTR */
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)&idt;
    idtr_load(&idtr);

    serial_puts("[ OK ] IDT loaded (all 256 gates populated with vector preservation, #DF bound to IST1)\n");
}

void isr_exception_handler(interrupt_frame_t *frame) {
    /* Breakpoint Trap (#BP, vector 3) is a non-fatal debugging trap */
    if (frame->vector == 3) {
        serial_puts("[TRAP] Exception 0x03 (Breakpoint Trap) at RIP: ");
        serial_print_hex(frame->rip);
        serial_puts(" - Resuming execution\n");
        return;
    }

    /* Unexpected hardware or software interrupts (vectors >= 32) */
    if (frame->vector >= 32) {
        serial_puts("[WARN] Unexpected interrupt received (Vector ");
        serial_print_dec(frame->vector);
        serial_puts(")\n");
        return;
    }

    /* Fatal Exception Panic */
    serial_puts("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    serial_puts("               CPU EXCEPTION KERNEL PANIC               \n");
    serial_puts("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

    serial_puts("Exception:   ");
    if (frame->vector < 32) {
        serial_puts(exception_messages[frame->vector]);
    } else {
        serial_puts("User / Unknown Vector");
    }
    serial_puts(" (Vector ");
    serial_print_dec(frame->vector);
    serial_puts(")\n");

    serial_puts("Error Code:  ");
    serial_print_hex(frame->error_code);
    serial_puts("\n");

    serial_puts("Instruction: RIP = ");
    serial_print_hex(frame->rip);
    serial_puts("  CS = ");
    serial_print_hex(frame->cs);
    serial_puts("  RFLAGS = ");
    serial_print_hex(frame->rflags);
    serial_puts("\n");

    serial_puts("Stack:       RSP = ");
    serial_print_hex(frame->rsp);
    serial_puts("  SS = ");
    serial_print_hex(frame->ss);
    serial_puts("\n");

    /* Special diagnostic decoding for Page Fault (#PF, vector 14) */
    if (frame->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        serial_puts("\n[#PF DIAGNOSTICS]\n");
        serial_puts("Faulting Linear Address (CR2): ");
        serial_print_hex(cr2);
        serial_puts("\nCause: ");

        if (!(frame->error_code & (1 << 0))) {
            serial_puts("[Page Not Present] ");
        } else {
            serial_puts("[Protection Violation] ");
        }

        if (frame->error_code & (1 << 1)) {
            serial_puts("[Write Access] ");
        } else {
            serial_puts("[Read Access] ");
        }

        if (frame->error_code & (1 << 2)) {
            serial_puts("[User Mode] ");
        } else {
            serial_puts("[Kernel Mode] ");
        }

        if (frame->error_code & (1 << 3)) {
            serial_puts("[Reserved Bit Violation] ");
        }

        if (frame->error_code & (1 << 4)) {
            serial_puts("[Instruction Fetch] ");
        }

        serial_puts("\n");
    }

    serial_puts("\nRegisters:\n");
    serial_puts("  RAX: "); serial_print_hex(frame->rax);
    serial_puts("  RBX: "); serial_print_hex(frame->rbx);
    serial_puts("  RCX: "); serial_print_hex(frame->rcx);
    serial_puts("  RDX: "); serial_print_hex(frame->rdx);
    serial_puts("\n");
    serial_puts("  RSI: "); serial_print_hex(frame->rsi);
    serial_puts("  RDI: "); serial_print_hex(frame->rdi);
    serial_puts("  RBP: "); serial_print_hex(frame->rbp);
    serial_puts("\n");
    serial_puts("  R8:  "); serial_print_hex(frame->r8);
    serial_puts("  R9:  "); serial_print_hex(frame->r9);
    serial_puts("  R10: "); serial_print_hex(frame->r10);
    serial_puts("  R11: "); serial_print_hex(frame->r11);
    serial_puts("\n");
    serial_puts("  R12: "); serial_print_hex(frame->r12);
    serial_puts("  R13: "); serial_print_hex(frame->r13);
    serial_puts("  R14: "); serial_print_hex(frame->r14);
    serial_puts("  R15: "); serial_print_hex(frame->r15);
    serial_puts("\n\nSystem halted.\n");

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
