; FortressOS 64-bit Interrupt Service Routine (ISR) Stubs
; Standardized uniform stack frame for all 32 x86 CPU exceptions

[bits 64]
default rel

extern isr_exception_handler

section .text

; Table of 256 ISR stubs, each aligned to exactly 16 bytes
align 16
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
align 16
global isr%+i
isr%+i:
%if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
    push qword i        ; Exception already pushed CPU error code
    jmp isr_common_stub
%else
    push qword 0        ; Push dummy error code
    push qword i        ; Push vector number
    jmp isr_common_stub
%endif
%assign i i+1
%endrep

; Common ISR entrance and exit stub
isr_common_stub:
    ; 1. Save all general purpose registers
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    ; Stack alignment calculation:
    ; CPU pushes 5 qwords (SS, RSP, RFLAGS, CS, RIP) = 40 bytes (RSP % 16 == 8)
    ; Stub pushes Error Code (8 bytes)               = 48 bytes (RSP % 16 == 0)
    ; Stub pushes Vector Number (8 bytes)            = 56 bytes (RSP % 16 == 8)
    ; Common stub pushes 15 GP registers (120 bytes) = 176 bytes (RSP % 16 == 0)
    ; Total frame size: 176 bytes (11 * 16 bytes, clean 16-byte alignment).
    ; System V ABI: RSP must be 16-byte aligned before 'call', which pushes 8-byte RIP.

    ; 2. Ensure direction flag is forward per ABI
    cld

    ; 3. Pass pointer to interrupt_frame_t as first parameter (RDI)
    mov rdi, rsp
    call isr_exception_handler

    ; 4. Restore general purpose registers
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; 5. Clean up error code and vector number pushed by stub
    add rsp, 16

    ; 6. Return from interrupt (pops RIP, CS, RFLAGS, RSP, SS)
    iretq

; Assembly helper to load IDTR
global idtr_load
idtr_load:
    lidt [rdi]
    ret

; Assembly helper to test execution of NX target
; Signature: void test_nx_exec_helper(uintptr_t target_addr);
; RDI = target_addr
global test_nx_exec_helper
extern idt_set_expected_page_fault
extern idt_clear_expected_page_fault

test_nx_exec_helper:
    ; Entry RSP is 8 mod 16; saving RBX aligns every call below.
    push rbx

    mov rbx, rdi

    ; Register recovery label with IDT
    lea rdi, [.nx_recovery]
    call idt_set_expected_page_fault

    ; Call target: pushes 8-byte return address, then CPU faults on instruction fetch
    call rbx

    ; In case target returned without faulting:
    jmp .done

.nx_recovery:
    ; IRET restored the target-entry RSP. Drop CALL's return address
    ; before calling C, restoring both the stack and ABI alignment.
    add rsp, 8
.done:
    call idt_clear_expected_page_fault
    pop rbx
    ret

; Assembly helper to enter user mode via iretq
; Signature: void enter_user_mode(uintptr_t entry_point, uintptr_t user_stack_top);
; RDI = entry_point
; RSI = user_stack_top
global enter_user_mode
enter_user_mode:
    ; 1. Load User Data selector (0x1B = 0x18 | 3) into DS and ES
    mov ax, 0x1B
    mov ds, ax
    mov es, ax

    ; 2. Build 64-bit iretq stack frame (pushed in reverse order):
    ;    [RSP + 32] SS:     0x1B (User Data Segment)
    ;    [RSP + 24] RSP:    user_stack_top
    ;    [RSP + 16] RFLAGS: 0x202 (IF = 1, bit 1 reserved = 1)
    ;    [RSP +  8] CS:     0x23 (User Code Segment)
    ;    [RSP +  0] RIP:    entry_point
    push qword 0x1B
    push rsi
    push qword 0x202
    push qword 0x23
    push rdi

    ; 3. Clear General Purpose Registers to prevent leaking kernel data
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
    xor rsi, rsi
    xor rdi, rdi

    ; 4. Atomic privilege switch into Ring 3
    iretq

; Assembly helper to test user mode execution and recovery
; Signature: bool test_user_mode_helper(uintptr_t entry_point, uintptr_t user_stack_top);
; RDI = entry_point
; RSI = user_stack_top
global test_user_mode_helper
extern idt_set_user_trap_handler
extern idt_clear_user_trap_handler

test_user_mode_helper:
    ; 1. Preserve callee-saved registers per System V ABI (6 * 8 = 48 bytes)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; 2. Preserve arguments in callee-saved registers
    mov r12, rdi   ; entry_point
    mov r13, rsi   ; user_stack_top

    ; 3. Stack alignment: entry RSP was 8 mod 16. Pushed 6 registers (48 bytes).
    ;    (8 - 48) = -40 = 8 mod 16. Subtract 8 to make RSP 0 mod 16.
    sub rsp, 8

    ; 4. Register recovery label with IDT user trap handler
    ;    Recovery RIP = .user_trap_recovery
    ;    Recovery RSP = rsp (points to our aligned stack frame)
    lea rdi, [.user_trap_recovery]
    mov rsi, rsp
    call idt_set_user_trap_handler

    ; 5. Transition into user mode
    mov rdi, r12
    mov rsi, r13
    call enter_user_mode

    ; enter_user_mode performs iretq; should never reach here directly
    jmp .failure

.user_trap_recovery:
    ; isr_common iretq landed here after handling int 0x80.
    ; Restore Kernel Data Segment into DS and ES
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Disarm trap handler
    call idt_clear_user_trap_handler

    ; Restore stack alignment and callee-saved registers
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    mov rax, 1
    ret

.failure:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call idt_clear_user_trap_handler
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    xor rax, rax
    ret

