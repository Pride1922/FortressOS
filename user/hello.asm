[bits 64]
default rel

section .rodata
msg: db "Hello from /bin/hello! VFS file execution verified.", 10
msg_len equ $ - msg

section .text
global _start

_start:
    ; SYS_WRITE (nr 1)
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    lea rsi, [msg]
    mov edx, msg_len
    syscall

    ; SYS_EXIT (nr 0)
    mov eax, 0          ; SYS_EXIT
    mov edi, 0          ; exit code 0
    syscall

.hang:
    hlt
    jmp .hang
