[bits 64]
default rel

section .rodata
stdout_data: db "STDOUT_DATA", 10
stdout_len equ $ - stdout_data
stderr_data: db "STDERR_DATA", 10
stderr_len equ $ - stderr_data

section .text
global _start
_start:
    test rsp, 0x0F
    jnz .bad_align

    mov eax, 1                  ; SYS_WRITE
    mov edi, 1
    lea rsi, [stdout_data]
    mov edx, stdout_len
    syscall
    cmp rax, stdout_len
    jne .write_failed

    mov eax, 1                  ; SYS_WRITE
    mov edi, 2
    lea rsi, [stderr_data]
    mov edx, stderr_len
    syscall
    cmp rax, -3                 ; SYSCALL_EBADF: closed stderr is intentional
    je .success
    cmp rax, stderr_len
    jne .write_failed
.success:
    xor edi, edi
    jmp .exit
.bad_align:
    mov edi, 2
    jmp .exit
.write_failed:
    mov edi, 1
.exit:
    xor eax, eax                ; SYS_EXIT
    syscall
    ud2
