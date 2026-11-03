.intel_syntax noprefix
.text
.global _start
.extern ssh_main
_start:
    and rsp, -16
    xor ebp, ebp
    call ssh_main
    mov edi, eax
    xor eax, eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
