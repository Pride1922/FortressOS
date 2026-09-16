[bits 64]
default rel

section .rodata
msg: db "Hello from standalone ELF64 user process!", 10
msg_len equ $ - msg

msg_worker_1: db "Worker 1: Computing in User Space...", 10
msg_worker_1_len equ $ - msg_worker_1

msg_worker_2: db "Worker 2: Computing in User Space...", 10
msg_worker_2_len equ $ - msg_worker_2

section .data
g_magic_val: dq 0xCAFEBABE12345678

section .bss
g_bss_val: resq 1

section .text
global _start

_start:
    ; RDI holds mode argument passed from kernel:
    ; 0 = Default initialization and self-tests (Checkpoint 3 & 4)
    ; 1 = CPU-Bound Worker 1 (computes across multiple timer ticks, exits 77)
    ; 2 = CPU-Bound Worker 2 (computes across multiple timer ticks, exits 88)
    ; 3 = Deliberate fault: attempts to read supervisor kernel higher-half memory
    cmp rdi, 1
    je .mode_worker_1
    cmp rdi, 2
    je .mode_worker_2
    cmp rdi, 3
    je .mode_fault

    ; -------------------------------------------------------------
    ; Mode 0: Default Init Executable Verification
    ; -------------------------------------------------------------
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

    ; -------------------------------------------------------------
    ; Mode 1: CPU-Bound Worker 1 (exits 77)
    ; -------------------------------------------------------------
.mode_worker_1:
    mov rax, 1
    mov rdi, 1
    lea rsi, [msg_worker_1]
    mov rdx, msg_worker_1_len
    int 0x80

    ; CPU-bound compute loop updating private .bss variable at 0x402000
    mov qword [g_bss_val], 0
    mov rcx, 50000000
.loop_worker_1:
    inc qword [g_bss_val]
    dec rcx
    jnz .loop_worker_1

    mov rax, 0
    mov rdi, 77
    int 0x80
    hlt

    ; -------------------------------------------------------------
    ; Mode 2: CPU-Bound Worker 2 (exits 88)
    ; -------------------------------------------------------------
.mode_worker_2:
    mov rax, 1
    mov rdi, 1
    lea rsi, [msg_worker_2]
    mov rdx, msg_worker_2_len
    int 0x80

    ; CPU-bound compute loop updating private .bss variable at 0x402000
    mov qword [g_bss_val], 0
    mov rcx, 50000000
.loop_worker_2:
    inc qword [g_bss_val]
    dec rcx
    jnz .loop_worker_2

    mov rax, 0
    mov rdi, 88
    int 0x80
    hlt

    ; -------------------------------------------------------------
    ; Mode 3: Deliberate Fault - Access Supervisor Kernel Memory!
    ; -------------------------------------------------------------
.mode_fault:
    mov rbx, 0xFFFFFFFF80000000
    mov rax, [rbx]      ; Triggers #PF (Vector 14) with Protection Violation in Ring 3
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
