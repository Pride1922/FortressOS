[bits 64]
default rel

section .rodata
global user_syscall_test_start
global user_syscall_test_end

user_syscall_test_start:
    ; Test 1: Valid Serial Write (fd=1, "Hello from Ring 3 Syscall!\n", count=27)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x500000
    mov rdx, 27
    int 0x80
    cmp rax, 27
    jne .fail_1

    ; Test 2: Valid Buffer Crossing Across Two Mapped Pages (0x500FF8, count=18)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x500FF8
    mov rdx, 18
    int 0x80
    cmp rax, 18
    jne .fail_2

    ; Test 3: Invalid Pointer (Unmapped User Address 0x600000)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x600000
    mov rdx, 10
    int 0x80
    cmp rax, -2   ; -SYSCALL_EFAULT
    jne .fail_3

    ; Test 4: Buffer Crossing From Mapped Page into Unmapped Page (0x501FF8, count=16)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x501FF8
    mov rdx, 16
    int 0x80
    cmp rax, -2   ; -SYSCALL_EFAULT
    jne .fail_4

    ; Test 5: Kernel Space Pointer (0xFFFFFFFF80000000)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0xFFFFFFFF80000000
    mov rdx, 16
    int 0x80
    cmp rax, -2   ; -SYSCALL_EFAULT
    jne .fail_5

    ; Test 6: Oversized Length Bound (count=100000)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x500000
    mov rdx, 100000
    int 0x80
    cmp rax, -1   ; -SYSCALL_EINVAL
    jne .fail_6

    ; Test 7: Zero-Length Write (count=0)
    mov rax, 1
    mov rdi, 1
    mov rsi, 0x500000
    mov rdx, 0
    int 0x80
    test rax, rax
    jnz .fail_7

    ; Test 8: Invalid File Descriptor (fd=99)
    mov rax, 1
    mov rdi, 99
    mov rsi, 0x500000
    mov rdx, 10
    int 0x80
    cmp rax, -3   ; -SYSCALL_EBADF
    jne .fail_8

    ; All tests passed -> Exit with success code 42
    mov rax, 0   ; SYS_EXIT
    mov rdi, 42
    int 0x80
    hlt

.fail_1:
    mov rdi, 101
    jmp .do_exit
.fail_2:
    mov rdi, 102
    jmp .do_exit
.fail_3:
    mov rdi, 103
    jmp .do_exit
.fail_4:
    mov rdi, 104
    jmp .do_exit
.fail_5:
    mov rdi, 105
    jmp .do_exit
.fail_6:
    mov rdi, 106
    jmp .do_exit
.fail_7:
    mov rdi, 107
    jmp .do_exit
.fail_8:
    mov rdi, 108
    jmp .do_exit

.do_exit:
    mov rax, 0   ; SYS_EXIT
    int 0x80
    hlt

user_syscall_test_end:
