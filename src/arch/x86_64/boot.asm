; FortressOS 64-bit Kernel Entry Stub (crt0)
; Target: x86_64 UEFI (Limine Boot Protocol)

[bits 64]
default rel

global _start
global kernel_stack_guard
global stack_bottom
global stack_top
extern kmain

section .text
_start:
    ; Clear Direction Flag (required by System V AMD64 ABI)
    cld

    ; Initialize a dedicated 16-byte aligned kernel stack
    lea rsp, [stack_top]

    ; Call the C kernel entry point
    call kmain

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 4096
kernel_stack_guard:
    resb 4096 ; 4 KiB dedicated unmapped guard page directly below stack
stack_bottom:
    resb 16384 ; 16 KiB early kernel stack
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits

