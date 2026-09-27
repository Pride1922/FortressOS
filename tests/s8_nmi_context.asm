[bits 64]
default rel
section .text
extern nmi_caught
global nmi_handler, nmi_self, nmi_self_resume
global nmi_timer, nmi_timer_loop, nmi_timer_end

; No prologue: the host inspects the original frame/return slot on entry.
nmi_handler:
    inc qword [nmi_caught]
    mov rax, 0xdead0001
    mov rcx, 0xdead0002
    mov r11, 0xdead0011
    ret                         ; the kernel-mapped restorer, never a local stub

%macro SAVE 0
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
%endmacro
%macro SEED 0
    mov eax, 0x101
    mov ebx, 0x102
    mov ecx, 0x103
    mov edx, 0x104
    mov esi, 0x105
    mov edi, 0x106
    mov ebp, 0x107
    mov r8d, 0x108
    mov r9d, 0x109
    mov r10d, 0x10a
    mov r11d, 0x10b
    mov r12d, 0x10c
    mov r13d, 0x10d
    mov r14d, 0x10e
    mov r15d, 0x10f
    std                         ; handler must clear DF; sigreturn restores it
%endmacro
%macro RESTORE 0
    cld
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
%endmacro
nmi_self:
    SAVE
    SEED
    mov eax, 27                 ; SYS_KILL
    xor edi, edi               ; own process group
    mov esi, 15                ; SIGTERM
    syscall
nmi_self_resume:
    RESTORE
nmi_timer:
    SAVE
    SEED
nmi_timer_loop:
    cmp qword [nmi_caught], 0
    je nmi_timer_loop           ; no syscall: only a real IRQ can deliver here
nmi_timer_end:
    RESTORE
section .note.GNU-stack noalloc noexec nowrite progbits
