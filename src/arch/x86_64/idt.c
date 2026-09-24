#include "idt.h"
#include "serial.h"
#include "thread.h"
#include "percpu.h"
#include "gdt.h"

extern void idtr_load(idt_ptr_t *ptr);
extern uint8_t isr_stub_table[];

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idtr;

void idt_load_cpu(void) { idtr_load(&idtr); }

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

        /* Stack Switching: Double Fault (#DF, Vector 8) uses IST1; NMI (Vector 2) uses IST2 */
        if (i == 8) {
            ist = 1;
        } else if (i == 2) {
            ist = 2;
        }

        uint8_t flags = IDT_GATE_INTERRUPT;
        /* Vector 0x80 (System Call) and Vector 3 (Breakpoint Trap) are user-accessible (DPL 3) */
        if (i == 0x80 || i == 3) {
            flags = IDT_GATE_USER;
        }

        void *handler = (void *)((uintptr_t)isr_stub_table + (i * 16));
        idt_set_gate((uint8_t)i, handler, ist, flags);
    }

    /* 3. Load IDTR */
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)&idt;
    idtr_load(&idtr);

    serial_puts("[ OK ] IDT loaded (all 256 gates populated, #DF on IST1, NMI on IST2, int 0x80 configured with DPL 3)\n");
}

static volatile bool      g_expect_page_fault = false;
static volatile bool      g_page_fault_caught = false;
static volatile uint64_t  g_last_fault_cr2    = 0;
static volatile uint64_t  g_last_fault_error  = 0;
static volatile uintptr_t g_pf_recovery_rip   = 0;

void idt_set_expected_page_fault(uintptr_t recovery_rip) {
    g_expect_page_fault = true;
    g_page_fault_caught = false;
    g_last_fault_cr2    = 0;
    g_last_fault_error  = 0;
    g_pf_recovery_rip   = recovery_rip;
}

void idt_clear_expected_page_fault(void) {
    g_expect_page_fault = false;
    g_pf_recovery_rip   = 0;
}

bool idt_was_page_fault_caught(uint64_t *out_cr2, uint64_t *out_error) {
    if (out_cr2) *out_cr2 = g_last_fault_cr2;
    if (out_error) *out_error = g_last_fault_error;
    return g_page_fault_caught;
}

/* User Mode Software Interrupt (int 0x80) Test Hook State */
static volatile bool      g_user_trap_active       = false;
static volatile bool      g_user_trap_caught       = false;
static volatile uint64_t  g_user_trap_cs           = 0;
static volatile uint64_t  g_user_trap_ss           = 0;
static volatile uint64_t  g_user_trap_rax          = 0;
static volatile uint64_t  g_user_trap_rsp          = 0;
static volatile uintptr_t g_user_trap_recovery_rip = 0;
static volatile uintptr_t g_user_trap_recovery_rsp = 0;

void idt_set_user_trap_handler(uintptr_t recovery_rip, uintptr_t recovery_rsp) {
    g_user_trap_active       = true;
    g_user_trap_caught       = false;
    g_user_trap_cs           = 0;
    g_user_trap_ss           = 0;
    g_user_trap_rax          = 0;
    g_user_trap_rsp          = 0;
    g_user_trap_recovery_rip = recovery_rip;
    g_user_trap_recovery_rsp = recovery_rsp;
}

void idt_clear_user_trap_handler(void) {
    g_user_trap_active       = false;
    g_user_trap_recovery_rip = 0;
    g_user_trap_recovery_rsp = 0;
}

bool idt_was_user_trap_caught(uint64_t *out_cs, uint64_t *out_ss, uint64_t *out_rax, uint64_t *out_rsp) {
    if (out_cs)  *out_cs  = g_user_trap_cs;
    if (out_ss)  *out_ss  = g_user_trap_ss;
    if (out_rax) *out_rax = g_user_trap_rax;
    if (out_rsp) *out_rsp = g_user_trap_rsp;
    return g_user_trap_caught;
}

static irq_handler_t g_irq_handlers[IDT_ENTRIES];
static bool g_needs_eoi[IDT_ENTRIES];

void idt_register_hardware_handler(uint8_t vector, irq_handler_t handler) {
    if (vector < 32 || vector == 255) return;
    g_irq_handlers[vector] = handler;
    g_needs_eoi[vector] = true;
}

void idt_register_handler(uint8_t vector, irq_handler_t handler) {
    g_irq_handlers[vector] = handler;
    g_needs_eoi[vector] = false;
}

void isr_exception_handler(interrupt_frame_t *frame) {
    cpu_local_t *cpu = cpu_current();
    /* AP bring-up probes have no logging/locks/global recovery state. */
    if (cpu->id != 0 && cpu->probe == frame->vector &&
        (frame->vector == 2 || frame->vector == 8)) {
        uintptr_t guard = gdt_cpu_ist_guard(cpu->id, frame->vector == 8 ? 1 : 2);
        cpu->probe_rsp = (uintptr_t)frame;
        if ((uintptr_t)frame >= guard + 4096 &&
            (uintptr_t)frame + sizeof(*frame) <= guard + 20480) {
            if (frame->vector == 8) {
                frame->rip = cpu->recovery_rip;
                frame->rsp = cpu->recovery_rsp;
            }
            cpu->probe = 0;
            return;
        }
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Non-Maskable Interrupt (NMI, Vector 2):
     * Hardware delivers Vector 2 on the dedicated 16 KiB IST2 emergency stack.
     * Architectural Contract:
     * 1. Strictly reentrant and lockless.
     * 2. Must NEVER acquire subsystem spinlocks (g_sched_lock, g_heap_lock, g_vmm_lock, g_pmm_lock).
     * 3. Must NEVER invoke scheduler / context switch functions (thread_yield, process_exit).
     */
    if (frame->vector == 2) {
        cpu->nmi_rsp = (uintptr_t)frame;
        cpu->nmi_count++;
        serial_nmi_report(cpu, frame->rip, frame->rsp);
        return;
    }

    /* Hardware or software interrupts (vectors >= 32, excluding syscall 0x80) */
    if (frame->vector >= 32 && frame->vector != 0x80) {
        if (g_irq_handlers[frame->vector]) {
            g_irq_handlers[frame->vector](frame);
            /* Single-owner EOI: dispatcher acknowledges handled non-spurious interrupts */
            if (g_needs_eoi[frame->vector]) {
                extern void lapic_eoi(void);
                lapic_eoi();
            }
        } else {
            /* Unhandled interrupt: track without blindly acknowledging to prevent cascade */
            serial_puts("[FATAL] Unhandled interrupt vector: ");
            serial_print_dec(frame->vector);
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        return;
    }

    /* APs unexpected exception isolation: keep unexpected faults out of BSP test hooks. */
    if (cpu->id != 0) {
        cpu->fault_vector = frame->vector;
        cpu->fault_error = frame->error_code;
        cpu->fault_rip = frame->rip;
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Breakpoint Trap (#BP, vector 3) is a non-fatal debugging trap */
    if (frame->vector == 3) {
        serial_puts("[TRAP] Exception 0x03 (Breakpoint Trap) at RIP: ");
        serial_print_hex(frame->rip);
        serial_puts(" - Resuming execution\n");
        return;
    }

    /* Expected Page Fault handler for VMM validation */
    if (frame->vector == 14 && g_expect_page_fault) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        g_page_fault_caught = true;
        g_last_fault_cr2    = cr2;
        g_last_fault_error  = frame->error_code;

        serial_puts("       [CAUGHT] Expected #PF caught at CR2: ");
        serial_print_hex(cr2);
        serial_puts(" (Error Code: ");
        serial_print_hex(frame->error_code);
        serial_puts(")\n");

        if (g_pf_recovery_rip != 0) {
            frame->rip = g_pf_recovery_rip;
        }
        return;
    }

    /* Software Interrupt / System Call (int 0x80) from User Mode */
    if (frame->vector == 0x80) {
        if (g_user_trap_active) {
            g_user_trap_caught = true;
            g_user_trap_cs     = frame->cs;
            g_user_trap_ss     = frame->ss;
            g_user_trap_rax    = frame->rax;
            g_user_trap_rsp    = frame->rsp;

            if (g_user_trap_recovery_rip != 0) {
                /* Redirect execution back to kernel recovery context in Ring 0 */
                frame->rip    = g_user_trap_recovery_rip;
                frame->cs     = 0x08; /* GDT_KERNEL_CODE */
                frame->ss     = 0x10; /* GDT_KERNEL_DATA */
                frame->rsp    = g_user_trap_recovery_rsp;
                frame->rflags = 0x002; /* Kernel RFLAGS with IF=0 */
            }
            return;
        }

        /* General System Call Dispatch */
        extern int64_t syscall_dispatch(interrupt_frame_t *frame);
        syscall_dispatch(frame);
        return;
    }

    /* User-Space Exception Handling (CPL 3): fault isolation without kernel panic */
    if ((frame->cs & 3) == 3) {
        tcb_t *curr = thread_current();

        serial_puts("\n[PROCESS FAULT] User Process Exception in Ring 3!\n");
        serial_puts("       PID:         ");
        serial_print_dec(curr ? curr->tid : 0);
        serial_puts(" (");
        if (curr) serial_puts(curr->name);
        serial_puts(")\n       Exception:   ");
        if (frame->vector < 32) {
            serial_puts(exception_messages[frame->vector]);
        } else {
            serial_puts("Unknown");
        }
        serial_puts(" (Vector ");
        serial_print_dec(frame->vector);
        serial_puts(")\n       RIP:         ");
        serial_print_hex(frame->rip);
        serial_puts("\n       Error Code:  ");
        serial_print_hex(frame->error_code);
        serial_puts("\n");

        if (frame->vector == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_puts("       Linear (CR2):");
            serial_print_hex(cr2);
            serial_puts("\n");
        }

        serial_puts("       Action: Terminating faulting user process; kernel remains operational.\n");

        /* Clean termination: 128 + vector (e.g. 142 for #PF) */
        process_exit(128 + frame->vector);
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* Fatal Exception Panic - raw UART output to avoid lockups under g_console_lock */
    serial_raw_puts("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    serial_raw_puts("               CPU EXCEPTION KERNEL PANIC               \n");
    serial_raw_puts("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

    serial_raw_puts("Exception:   ");
    if (frame->vector < 32) {
        serial_raw_puts(exception_messages[frame->vector]);
    } else {
        serial_raw_puts("User / Unknown Vector");
    }
    serial_raw_puts(" (Vector ");
    serial_raw_print_dec(frame->vector);
    serial_raw_puts(")\n");

    serial_raw_puts("Error Code:  ");
    serial_raw_print_hex(frame->error_code);
    serial_raw_puts("\n");

    serial_raw_puts("Instruction: RIP = ");
    serial_raw_print_hex(frame->rip);
    serial_raw_puts("  CS = ");
    serial_raw_print_hex(frame->cs);
    serial_raw_puts("  RFLAGS = ");
    serial_raw_print_hex(frame->rflags);
    serial_raw_puts("\n");

    serial_raw_puts("Stack:       RSP = ");
    serial_raw_print_hex(frame->rsp);
    serial_raw_puts("  SS = ");
    serial_raw_print_hex(frame->ss);
    serial_raw_puts("\n");

    /* Special diagnostic decoding for Page Fault (#PF, vector 14) */
    if (frame->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        serial_raw_puts("\n[#PF DIAGNOSTICS]\n");
        serial_raw_puts("Faulting Linear Address (CR2): ");
        serial_raw_print_hex(cr2);
        serial_raw_puts("\nCause: ");

        if (!(frame->error_code & (1 << 0))) {
            serial_raw_puts("[Page Not Present] ");
        } else {
            serial_raw_puts("[Protection Violation] ");
        }

        if (frame->error_code & (1 << 1)) {
            serial_raw_puts("[Write Access] ");
        } else {
            serial_raw_puts("[Read Access] ");
        }

        if (frame->error_code & (1 << 2)) {
            serial_raw_puts("[User Mode] ");
        } else {
            serial_raw_puts("[Kernel Mode] ");
        }

        if (frame->error_code & (1 << 3)) {
            serial_raw_puts("[Reserved Bit Violation] ");
        }

        if (frame->error_code & (1 << 4)) {
            serial_raw_puts("[Instruction Fetch] ");
        }

        serial_raw_puts("\n");
    }

    serial_raw_puts("\nRegisters:\n");
    serial_raw_puts("  RAX: "); serial_raw_print_hex(frame->rax);
    serial_raw_puts("  RBX: "); serial_raw_print_hex(frame->rbx);
    serial_raw_puts("  RCX: "); serial_raw_print_hex(frame->rcx);
    serial_raw_puts("  RDX: "); serial_raw_print_hex(frame->rdx);
    serial_raw_puts("\n");
    serial_raw_puts("  RSI: "); serial_raw_print_hex(frame->rsi);
    serial_raw_puts("  RDI: "); serial_raw_print_hex(frame->rdi);
    serial_raw_puts("  RBP: "); serial_raw_print_hex(frame->rbp);
    serial_raw_puts("\n");
    serial_raw_puts("  R8:  "); serial_raw_print_hex(frame->r8);
    serial_raw_puts("  R9:  "); serial_raw_print_hex(frame->r9);
    serial_raw_puts("  R10: "); serial_raw_print_hex(frame->r10);
    serial_raw_puts("  R11: "); serial_raw_print_hex(frame->r11);
    serial_raw_puts("\n");
    serial_raw_puts("  R12: "); serial_raw_print_hex(frame->r12);
    serial_raw_puts("  R13: "); serial_raw_print_hex(frame->r13);
    serial_raw_puts("  R14: "); serial_raw_print_hex(frame->r14);
    serial_raw_puts("  R15: "); serial_raw_print_hex(frame->r15);
    serial_raw_puts("\n\nSystem halted.\n");

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
