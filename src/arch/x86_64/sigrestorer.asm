; FortressOS signal restorer stub
; Position-independent, no relocations, fits in one 4 KiB page.
;
; Entry state (set by handler's ret instruction):
;   RSP = F (frame base, 0 mod 16)
;   [RSP] = signal_frame_v1_t (224 bytes)
;
; The restorer passes RSP (= F = pointer to the frame) in RDI and issues
; SYS_SIGRETURN. On success the kernel restores the interrupted context and
; never returns here. On error the stub terminates via SYS_EXIT.
;
; This file is linked into the kernel image and the kernel copies the
; position-independent code to each process's private page at
; USER_SIGRESTORER_VIRT. The page is read-only/executable from user mode.

[bits 64]
default rel

%include "build/signal_frame_asm.inc"

; SYS_EXIT = 0 (from syscall_abi.h)
%define SYS_EXIT 0

section .sigrestorer_text progbits alloc exec nowrite

global sigrestorer_start
global sigrestorer_end

sigrestorer_start:
    ; RSP = F (0 mod 16). Pass F in RDI as the frame pointer.
    mov    rdi, rsp
    mov    eax, SYS_SIGRETURN
    syscall

    ; If sigreturn failed (negative RAX), terminate.
    ; RDI = exit code derived from the error (non-zero).
    mov    rdi, rax          ; negative error code — non-zero exit
    mov    eax, SYS_EXIT
    syscall

    ; Unreachable: but if somehow execution reaches here, spin/halt.
    ; MUST NOT execute ret into the frame header.
.halt:
    hlt
    jmp    .halt

sigrestorer_end:

; Assert the stub fits in one 4 KiB page.
; NASM evaluates this at assemble time.
%if (sigrestorer_end - sigrestorer_start) > 4096
%error "Restorer stub exceeds 4 KiB page"
%endif
