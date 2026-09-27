[bits 64]
default rel

section .text
global syscall_entry_stub
; Zero-byte probe symbols for deterministic external NMI injection tests.
global syscall_entry_kernel_gs
global syscall_exit_kernel_gs
global syscall_entry_rsp_saved
global syscall_entry_kernel_rsp
global syscall_exit_restore_rsp
global syscall_exit_user_rsp
; RETURN_SIGRETURN probe symbols (for NMI/test suite)
global syscall_sigreturn_iretq
global sigreturn_restore_regs
global sigreturn_before_swapgs
global sigreturn_after_swapgs
global sigreturn_before_iretq
extern syscall_dispatch

; GDT Selectors matching FortressOS layout
GDT_KERNEL_CODE equ 0x08
GDT_KERNEL_DATA equ 0x10
GDT_USER_DATA   equ 0x1B
GDT_USER_CODE   equ 0x23

; frame->vector marker written by syscall_dispatch when RETURN_SIGRETURN is set.
; Value 0x100 is above any real interrupt vector (0..255) so it cannot collide.
SIGRETURN_VECTOR_MARKER equ 0x100

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
    swapgs
syscall_entry_kernel_gs:
    mov [gs:8], rsp
syscall_entry_rsp_saved:
    mov rsp, [gs:16]
syscall_entry_kernel_rsp:

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
    push qword [gs:8] ; rsp (user RSP)
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

    ; Select the existing kernel-written disposition before touching GPRs.
    ; This implementation has no separate scratch area to discard.
    cmp qword [rsp + 120], SIGRETURN_VECTOR_MARKER
    je sigreturn_restore_regs

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

    ; 5. Check for RETURN_SIGRETURN disposition:
    ;    frame->vector is at [rsp] after GPR pops.
    ;    0x100 = SIGRETURN_VECTOR_MARKER set by syscall_dispatch/sys_sigreturn.
    ;    If set: skip to IRETQ directly (frame already fully committed).
    cmp qword [rsp], SIGRETURN_VECTOR_MARKER
    je  syscall_sigreturn_iretq

    add rsp, 16 ; skip vector and error_code

    ; 6. Check if returning to user space or test harness recovery
    ; Current stack layout:
    ;   [RSP + 0]:  rip
    ;   [RSP + 8]:  cs
    ;   [RSP + 16]: rflags
    ;   [RSP + 24]: rsp
    ;   [RSP + 32]: ss
    cmp qword [rsp + 8], GDT_USER_CODE
    jne syscall_return_iretq

    ; Fast return to user space via sysretq (o64 sysret):
    pop rcx     ; user RIP into RCX for sysret
    add rsp, 8  ; skip CS (sysret sets CS from STAR[63:48])
    pop r11     ; user RFLAGS into R11 for sysret
syscall_exit_restore_rsp:
    pop rsp     ; restore user RSP directly
syscall_exit_kernel_gs:
    swapgs
syscall_exit_user_rsp:
    ; sysretq atomically restores Ring 3, CS, SS, RIP from RCX, and RFLAGS from R11 (re-enabling IF)
    o64 sysret

; Separate restoration makes the first-pop boundary specific to sigreturn.
; Labels emit no bytes (in particular, do not insert DB 0 or a wait).
sigreturn_restore_regs:
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
syscall_sigreturn_iretq:
    ; RETURN_SIGRETURN path: the full IRET frame is already in the frame fields.
    ; Skip vector+error_code (16 bytes), then IRETQ restores RIP/CS/RFLAGS/RSP/SS.
    ; swapgs required: we entered with swapgs at the top, so we must undo it.
    ; At this point GS is in kernel mode (was swapped at entry).
    add rsp, 16   ; skip vector and error_code
sigreturn_before_swapgs:
    swapgs
sigreturn_after_swapgs:
sigreturn_before_iretq:
    iretq

syscall_return_iretq:
    ; Test harness recovery redirected CS to GDT_KERNEL_CODE; return via iretq.
    ; GS was swapped at entry; IRETQ returning to kernel mode does NOT need swapgs.
    iretq
