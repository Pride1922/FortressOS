[bits 64]
default rel

section .rodata
msg: db "Hello from /bin/hello! VFS file execution verified.", 10
msg_len equ $ - msg

arg_prefix: db "Received argument: "
arg_prefix_len equ $ - arg_prefix

newline: db 10

section .text
global _start

_start:
    ; System V AMD64 ABI:
    ;   - RSP must be 16-byte aligned at _start entry (RSP % 16 == 0)
    ;   - RDI = argc (also at [rsp])
    ;   - RSI = argv (also at [rsp + 8])
    test rsp, 0x0F
    jnz .bad_rsp_alignment

    mov r12, rdi        ; Preserve argc
    mov r13, rsi        ; Preserve argv pointer

    ; SYS_WRITE (nr 1) - base message
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    lea rsi, [msg]
    mov edx, msg_len
    syscall

    ; If argc <= 1, no arguments were passed; exit cleanly with 0
    cmp r12, 1
    jle .exit_zero

    ; Print argument prefix: "Received argument: "
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    lea rsi, [arg_prefix]
    mov edx, arg_prefix_len
    syscall

    ; Load pointer to argv[1]
    mov r14, [r13 + 8]  ; argv[1]
    test r14, r14
    jz .exit_zero

    ; Compute string length of argv[1]
    xor ecx, ecx
.strlen:
    cmp byte [r14 + rcx], 0
    je .strlen_done
    inc rcx
    jmp .strlen

.strlen_done:
    ; Print argv[1]
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    mov rsi, r14
    mov edx, ecx
    syscall

    ; Print newline
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    lea rsi, [newline]
    mov edx, 1
    syscall

    ; Parse argv[1] as decimal integer: if fully numeric, return value as exit code; else return 0
    mov rsi, r14
    xor eax, eax        ; Accumulator
    xor ecx, ecx
.parse_loop:
    movzx edx, byte [rsi + rcx]
    test dl, dl
    jz .exit_with_code
    cmp dl, '0'
    jb .exit_zero
    cmp dl, '9'
    ja .exit_zero
    sub dl, '0'
    imul rax, rax, 10
    add rax, rdx
    inc rcx
    jmp .parse_loop

.exit_zero:
    xor eax, eax

.exit_with_code:
    ; SYS_EXIT (nr 0) - return status in RDI
    mov rdi, rax
    mov eax, 0
    syscall

.bad_rsp_alignment:
    ; SYS_EXIT (nr 0) with status 99 indicating RSP % 16 != 0 at _start
    mov rdi, 99
    mov eax, 0
    syscall

.hang:
    hlt
    jmp .hang
