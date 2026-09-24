[bits 64]
default rel
section .text
global smp_stack_enter
extern smp_ap_local_entry
; RDI=kernel CR3, RSI=kernel stack top, RDX=CPU index.
smp_stack_enter:
    cli
    mov r8, rdx
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 11
    wrmsr
    mov cr3, rdi
    mov rsp, rsi
    xor ebp, ebp
    mov rdi, r8
    call smp_ap_local_entry
.halt:
    cli
    hlt
    jmp .halt

global smp_fault_probe
; Save normal stack + recovery before provoking a real #PF -> #DF.
; RDI points just above an unmapped guard; the push faults in the guard,
; and #PF frame delivery on that same stack faults again, invoking IST1.
smp_fault_probe:
    mov [rsi], rsp
    lea rax, [rel .recovered]
    mov [rdx], rax
    mov rsp, rdi
    push rax
    ud2
.recovered:
    ret
