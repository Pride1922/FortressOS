[bits 64]
default rel
section .text
global _start
extern TOOL_ENTRY
_start:
    and rsp, -16
    xor ebp, ebp
    call TOOL_ENTRY
    mov edi, eax
    xor eax, eax
    syscall
    ud2
section .note.GNU-stack noalloc noexec nowrite progbits
