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
