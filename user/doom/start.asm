[bits 64]
default rel
section .text
global _start
extern doom_main
_start:
    ; System V ABI passes rdi = argc, rsi = argv, rdx = envp
    mov rbp, rsp
    and rsp, -16
    call doom_main
    mov edi, eax
    xor eax, eax
    syscall
    ud2
section .note.GNU-stack noalloc noexec nowrite progbits
