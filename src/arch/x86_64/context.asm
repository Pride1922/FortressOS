[bits 64]
default rel

section .text

global switch_context
global thread_trampoline
extern thread_exit

; =============================================================================
; void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
; System V AMD64 ABI:
;   RDI = &old_thread->rsp
;   RSI = new_thread->rsp
; =============================================================================
switch_context:
    ; 1. Save RFLAGS and System V callee-preserved registers
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; 2. Atomically exchange stack pointers with interrupts disabled
    cli
    mov [rdi], rsp
    mov rsp, rsi

    ; 3. Restore callee-preserved registers and RFLAGS
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq

    ; 4. Resume execution in next thread
    ret

; =============================================================================
; void thread_trampoline(void)
; Entry point for freshly initialized threads.
; On entry:
;   R12 = thread entry function pointer
;   R13 = thread argument pointer
;   RSP is 16-byte aligned (post 'ret' from switch_context)
; =============================================================================
thread_trampoline:
    ; RDI = first argument in System V ABI
    mov rdi, r13
    ; Call thread entry point: pushes 8-byte return address, so entry() sees RSP % 16 == 8
    call r12

    ; If thread entry returns, cleanly terminate and reschedule
    call thread_exit

.halt:
    hlt
    jmp .halt

; =============================================================================
; void user_process_trampoline(void)
; Entry point for freshly initialized user processes.
; On entry (restored by switch_context):
;   R12 = user entry point (RIP)
;   R13 = user stack top (RSP)
; =============================================================================
global user_process_trampoline
user_process_trampoline:
    ; Set user data segment selectors (DS, ES, FS, GS)
    mov ax, 0x1B    ; GDT_USER_DATA | 3 (RPL=3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; GS base remains CPU-local until SWAPGS below.

    ; Push iretq frame: SS, RSP, RFLAGS, CS, RIP
    push 0x1B       ; SS: User Data Segment
    push r13        ; RSP: User Stack Pointer
    push 0x202      ; RFLAGS: IF=1 (interrupts enabled), bit 1 reserved
    push 0x23       ; CS: User Code Segment (0x20 | 3)
    push r12        ; RIP: User Entry Point

    ; Pass System V ABI initial arguments:
    ;   RDI = argc (passed in R14)
    ;   RSI = argv pointer (passed in R15)
    ;   RDX = 0 (rtld shared object termination function, set by xor rdx, rdx)
    mov rdi, r14
    mov rsi, r15

    ; Clear general-purpose registers to prevent leaking kernel state into user space
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    swapgs
    iretq

