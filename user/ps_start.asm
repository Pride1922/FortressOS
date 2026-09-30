[bits 64]
default rel
section .text
global _start
extern ps_main
_start:
    and rsp, -16
    xor ebp, ebp
    call ps_main
    mov edi, eax
    xor eax, eax
    syscall
    ud2
section .note.GNU-stack noalloc noexec nowrite progbits
