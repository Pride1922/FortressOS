[bits 64]
default rel

section .bss
global g_syscall_scratch_rsp
g_syscall_scratch_rsp: resq 1

section .text
global syscall_entry_stub
extern g_tss_rsp0
extern syscall_dispatch

; GDT Selectors matching FortressOS layout
GDT_KERNEL_CODE equ 0x08
GDT_KERNEL_DATA equ 0x10
GDT_USER_DATA   equ 0x1B
GDT_USER_CODE   equ 0x23

; =============================================================================
; Fast System Call Entry Point (Invoked via 'syscall' instruction)
; Hardware state on entry:
;   RCX    = User RIP (saved by hardware)
;   R11    = User RFLAGS (saved by hardware)
;   RFLAGS = RFLAGS & ~SFMASK (IF=0, TF=0, DF=0, interrupts disabled)
;   CS     = STAR[47:32] & ~3 (0x08, Kernel Code)
;   SS     = (STAR[47:32] & ~3) + 8 (0x10, Kernel Data)
;   RSP    = User RSP (UNTOUCHED by hardware - untrusted!)
; =============================================================================
syscall_entry_stub:
    ; 1. Save user RSP atomically and switch to active thread kernel stack (TSS.RSP0)
    ; Interrupts are guaranteed disabled by hardware (SFMASK masks IF)
    mov [rel g_syscall_scratch_rsp], rsp
    mov rsp, [rel g_tss_rsp0]

    ; 2. Build interrupt_frame_t layout on kernel stack:
    ; struct interrupt_frame_t:
    ;   [offset 168]: ss
    ;   [offset 160]: rsp
    ;   [offset 152]: rflags
    ;   [offset 144]: cs
    ;   [offset 136]: rip
    ;   [offset 128]: error_code
    ;   [offset 120]: vector
    ;   [offset 0..112]: rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15
    push qword GDT_USER_DATA               ; ss
    push qword [rel g_syscall_scratch_rsp] ; rsp (user RSP)
    push r11                               ; rflags (user RFLAGS)
    push qword GDT_USER_CODE               ; cs
    push rcx                               ; rip (user RIP)
    push qword 0                           ; error_code
    push qword 0x80                        ; vector

    ; Push GPRs in reverse order matching interrupt_frame_t:
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

    ; 3. Invoke unified C syscall dispatcher:
    ; RDI = interrupt_frame_t *frame
    mov rdi, rsp
    cld
    call syscall_dispatch

    ; 4. Restore general purpose registers:
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

    add rsp, 16 ; skip vector and error_code

    ; 5. Check if returning to user space or test harness recovery
    ; Current stack layout:
    ;   [RSP + 0]:  rip
    ;   [RSP + 8]:  cs
    ;   [RSP + 16]: rflags
    ;   [RSP + 24]: rsp
    ;   [RSP + 32]: ss
    cmp qword [rsp + 8], GDT_USER_CODE
    jne .return_iretq

    ; Fast return to user space via sysretq (o64 sysret):
    pop rcx     ; user RIP into RCX for sysret
    add rsp, 8  ; skip CS (sysret sets CS from STAR[63:48])
    pop r11     ; user RFLAGS into R11 for sysret
    pop rsp     ; restore user RSP directly
    ; sysretq atomically restores Ring 3, CS, SS, RIP from RCX, and RFLAGS from R11 (re-enabling IF)
    o64 sysret

.return_iretq:
    ; Test harness recovery redirected CS to GDT_KERNEL_CODE; return via iretq
    iretq
