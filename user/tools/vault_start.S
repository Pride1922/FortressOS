.intel_syntax noprefix
.text
.global _start
.extern vault_main
_start:
    and rsp, -16
    xor ebp, ebp
    call vault_main
    mov edi, eax
    xor eax, eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
