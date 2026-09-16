[bits 64]
default rel

section .rodata
msg: db "Hello from standalone ELF64 user process!", 10
msg_len equ $ - msg

section .data
g_magic_val: dq 0xCAFEBABE12345678

section .bss
g_bss_val: resq 1

section .text
global _start

_start:
    ; 1. Verify .data initialized value
    mov rax, [g_magic_val]
    mov rbx, 0xCAFEBABE12345678
    cmp rax, rbx
    jne .fail_data

    ; 2. Verify .bss was zeroed by the loader
    mov rax, [g_bss_val]
    test rax, rax
    jnz .fail_bss

    ; Increment .bss value to test writeability of .bss
    inc qword [g_bss_val]
    mov rax, [g_bss_val]
    cmp rax, 1
    jne .fail_bss_write

    ; 3. Print message via SYS_WRITE
    mov rax, 1          ; SYS_WRITE
    mov rdi, 1          ; stdout
    lea rsi, [msg]      ; buffer
    mov rdx, msg_len    ; count
    int 0x80
    cmp rax, msg_len
    jne .fail_write

    ; 4. Test user stack push/pop
    push qword 0x55AA
    pop rcx
    cmp rcx, 0x55AA
    jne .fail_stack

    ; 5. Preemption compute loop (executes in Ring 3, allowing timer ticks to preempt)
    mov rcx, 10000000
.compute_loop:
    dec rcx
    jnz .compute_loop

    ; 6. All tests passed! Call SYS_EXIT with code 77
    mov rax, 0          ; SYS_EXIT
    mov rdi, 77         ; exit code
    int 0x80
    hlt

.fail_data:
    mov rdi, 1
    jmp .do_exit

.fail_bss:
    mov rdi, 2
    jmp .do_exit

.fail_bss_write:
    mov rdi, 3
    jmp .do_exit

.fail_write:
    mov rdi, 4
    jmp .do_exit

.fail_stack:
    mov rdi, 5
    jmp .do_exit

.do_exit:
    mov rax, 0          ; SYS_EXIT
    int 0x80
    hlt
