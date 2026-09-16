[bits 64]
default rel
section .text
global _start
extern shell_main
_start:
    and rsp, -16
    xor rbp, rbp
    call shell_main
    xor edi, edi
    xor eax, eax
    syscall
    ud2
section .note.GNU-stack noalloc noexec nowrite progbits
