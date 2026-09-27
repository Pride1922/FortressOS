; /bin/sh-builtin entry stub — identical structure to shell_start.asm.
; The C entry point receives argc/argv/envp from the kernel's ELF loader
; via registers: RDI=argc, RSI=argv, RDX=envp (System V freestanding ABI).
[bits 64]
default rel
section .text
global _start
extern sh_builtin_main
_start:
    and  rsp, -16       ; Align to 16 bytes before call (System V ABI)
    xor  rbp, rbp       ; Mark outermost frame
    call sh_builtin_main
    mov  edi, eax       ; Exit status in RDI
    xor  eax, eax       ; SYS_EXIT = 0
    syscall
    ud2                 ; Unreachable
section .note.GNU-stack noalloc noexec nowrite progbits
