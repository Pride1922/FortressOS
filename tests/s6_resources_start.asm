[bits 64]
default rel
section .text
global _start
extern shell_main
_start:
    test rsp, 0x0f
    jnz .bad_align
    xor ebp, ebp
    call shell_main
    xor edi, edi
    jmp .exit
.bad_align:
    mov edi, 1
.exit:
    xor eax, eax
    syscall
    ud2

; Beyond dual_stream's text: its execution cannot hit this virtual breakpoint.
align 4096
global s6_resource_checkpoint
s6_resource_checkpoint:
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
