; FortressOS GDT & TSS Flush Routine
; System V AMD64 ABI:
;   rdi = pointer to gdt_ptr_t
;   rsi = kernel code segment selector (0x08)
;   rdx = kernel data segment selector (0x10)
;   rcx = TSS selector (0x28)

[bits 64]
default rel

global gdt_flush

section .text
gdt_flush:
    ; 1. Load GDT descriptor
    lgdt [rdi]

    ; 2. Reload data segment registers
    mov ds, dx
    mov es, dx
    mov ss, dx
    mov fs, dx
    mov gs, dx

    ; 3. Far return to reload Code Segment (CS)
    push rsi                    ; push 64-bit CS selector
    lea rax, [rel .reload_cs]   ; push target instruction pointer
    push rax
    retfq

.reload_cs:
    ; 4. Load Task Register with TSS selector
    ltr cx
    ret
