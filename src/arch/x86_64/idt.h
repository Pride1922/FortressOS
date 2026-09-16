#ifndef FORTRESS_IDT_H
#define FORTRESS_IDT_H

#include "types.h"

#define IDT_ENTRIES 256

/* Gate Types */
#define IDT_GATE_INTERRUPT 0x8E /* Present, Ring 0, 64-bit Interrupt Gate */
#define IDT_GATE_USER      0xEE /* Present, Ring 3, 64-bit Interrupt Gate */

/* 16-byte IDT Descriptor */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

/* IDT Pointer passed to lidt */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

/* Standardized Interrupt & Exception Stack Frame */
typedef struct {
    /* Saved General Purpose Registers (pushed by common stub) */
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    /* Pushed by individual ISR stub */
    uint64_t vector;
    uint64_t error_code;

    /* Pushed automatically by CPU on interrupt/exception */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

void idt_init(void);
void idt_set_gate(uint8_t vector, void *handler, uint8_t ist, uint8_t type_attributes);
void isr_exception_handler(interrupt_frame_t *frame);

/* Dynamic IRQ Handler Registration */
typedef void (*irq_handler_t)(interrupt_frame_t *frame);
void idt_register_handler(uint8_t vector, irq_handler_t handler);
void idt_register_hardware_handler(uint8_t vector, irq_handler_t handler);

/* Diagnostic / Test Hooks for Expected Page Faults */
void idt_set_expected_page_fault(uintptr_t recovery_rip);
void idt_clear_expected_page_fault(void);
bool idt_was_page_fault_caught(uint64_t *out_cr2, uint64_t *out_error);
void test_nx_exec_helper(uintptr_t target_addr);

/* Ring 3 Transition & Trap Test Hooks */
void enter_user_mode(uintptr_t entry_point, uintptr_t user_stack_top);
bool test_user_mode_helper(uintptr_t entry_point, uintptr_t user_stack_top);
void idt_set_user_trap_handler(uintptr_t recovery_rip, uintptr_t recovery_rsp);
void idt_clear_user_trap_handler(void);
bool idt_was_user_trap_caught(uint64_t *out_cs, uint64_t *out_ss, uint64_t *out_rax, uint64_t *out_rsp);

#endif /* FORTRESS_IDT_H */
