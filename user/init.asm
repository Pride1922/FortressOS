[bits 64]
default rel

section .rodata
msg: db "Hello from standalone ELF64 user process!", 10
msg_len equ $ - msg

msg_worker_1: db "Worker 1: Computing in User Space...", 10
msg_worker_1_len equ $ - msg_worker_1

msg_worker_2: db "Worker 2: Computing in User Space...", 10
msg_worker_2_len equ $ - msg_worker_2

msg_fast_syscall: db "Fast Syscall (syscall/sysret) operational in Ring 3!", 10
msg_fast_syscall_len equ $ - msg_fast_syscall

msg_dual_int80: db "Reference int 0x80 operational in Ring 3!", 10
msg_dual_int80_len equ $ - msg_dual_int80

msg_worker_fast1: db "1"
msg_worker_fast1_len equ $ - msg_worker_fast1

msg_worker_fast2: db "2"
msg_worker_fast2_len equ $ - msg_worker_fast2

path_motd: db "/etc/motd", 0
path_bad: db "/no/such/file", 0
msg_vfs_start: db "Starting Ring 3 VFS acceptance test...", 10
msg_vfs_start_len equ $ - msg_vfs_start

section .data
g_magic_val: dq 0xCAFEBABE12345678

section .bss
g_bss_val: resq 1
vfs_buf1: resb 1024
vfs_buf2: resb 1024

section .text
global _start

_start:
    ; RDI holds mode argument passed from kernel:
    ; 0 = Default initialization and self-tests (Checkpoint 3 & 4)
    ; 1 = CPU-Bound Worker 1 (computes across multiple timer ticks, exits 77)
    ; 2 = CPU-Bound Worker 2 (computes across multiple timer ticks, exits 88)
    ; 3 = Deliberate fault: attempts to read supervisor kernel higher-half memory
    ; 4 = Fast Syscall (syscall/sysret) & Dual-Interface Verification (exits 99)
    ; 5 = Preempted Fast Syscall Worker 1 (repeated fast syscalls, exits 91)
    ; 6 = Preempted Fast Syscall Worker 2 (repeated fast syscalls, exits 92)
    ; 7 = Ring 3 VFS & Initramfs Acceptance Test (exits 88)
    cmp rdi, 1
    je .mode_worker_1
    cmp rdi, 2
    je .mode_worker_2
    cmp rdi, 3
    je .mode_fault
    cmp rdi, 4
    je .mode_fast_syscall
    cmp rdi, 5
    je .mode_fast_worker_1
    cmp rdi, 6
    je .mode_fast_worker_2
    cmp rdi, 8
    je .mode_ext2_test
    cmp rdi, 7
    je .mode_vfs_test
    cmp rdi, 9
    je .mode_quick_exit

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

    ; -------------------------------------------------------------
    ; Mode 4: Fast Syscall (syscall/sysret) & Dual-Interface Suite
    ; -------------------------------------------------------------
.mode_fast_syscall:
    ; 1. SYS_WRITE via 'syscall' instruction
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    lea rsi, [msg_fast_syscall]    ; buffer
    mov rdx, msg_fast_syscall_len  ; count
    syscall
    cmp rax, msg_fast_syscall_len
    jne .fail_fast_write

    ; 2. SYS_WRITE via reference 'int 0x80' instruction
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    lea rsi, [msg_dual_int80]      ; buffer
    mov rdx, msg_dual_int80_len    ; count
    int 0x80
    cmp rax, msg_dual_int80_len
    jne .fail_fast_int80

    ; 3. Negative test via 'syscall': unmapped pointer (EFAULT)
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    mov rsi, 0x600000              ; unmapped virtual address
    mov rdx, 16
    syscall
    cmp rax, -2                    ; SYSCALL_EFAULT
    jne .fail_fast_efault

    ; 4. Negative test via 'syscall': oversized buffer (EINVAL)
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    lea rsi, [msg_fast_syscall]
    mov rdx, 100000                ; exceeds MAX_SYSCALL_WRITE_LEN (16384)
    syscall
    cmp rax, -1                    ; SYSCALL_EINVAL
    jne .fail_fast_einval

    ; 5. Negative test via 'syscall': invalid file descriptor (EBADF)
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 99                    ; invalid fd
    lea rsi, [msg_fast_syscall]
    mov rdx, 10
    syscall
    cmp rax, -3                    ; SYSCALL_EBADF
    jne .fail_fast_ebadf

    ; 6. Negative test via 'syscall': unknown syscall number (ENOSYS)
    mov rax, 999                   ; invalid syscall number
    syscall
    cmp rax, -4                    ; SYSCALL_ENOSYS
    jne .fail_fast_enosys

    ; 7. Test user stack push/pop across syscall
    push qword 0xAA55
    pop rcx
    cmp rcx, 0xAA55
    jne .fail_fast_stack

    ; 8. Clean exit via 'syscall' instruction with exit code 99!
    mov rax, 0                     ; SYS_EXIT
    mov rdi, 99                    ; exit code 99
    syscall
    hlt

    ; -------------------------------------------------------------
    ; Mode 5: Preempted Concurrent Fast Syscall Worker 1 (exits 91)
    ; Repeatedly issues 'syscall' interspersed with compute loops
    ; -------------------------------------------------------------
.mode_fast_worker_1:
    mov r12, 60                    ; 60 repeated syscall cycles
.loop_fast_worker_1:
    ; 1. CPU-bound compute loop
    mov rcx, 500000
.compute_fast_1:
    inc qword [g_bss_val]
    dec rcx
    jnz .compute_fast_1

    ; 2. Test user stack preservation across fast syscall
    push qword 0x5511
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    lea rsi, [msg_worker_fast1]    ; "1"
    mov rdx, msg_worker_fast1_len
    syscall
    pop rcx
    cmp rcx, 0x5511
    jne .fail_fast_stack
    cmp rax, msg_worker_fast1_len
    jne .fail_fast_write

    dec r12
    jnz .loop_fast_worker_1

    ; Clean exit via fast 'syscall' with exit code 91
    mov rax, 0                     ; SYS_EXIT
    mov rdi, 91                    ; exit code 91
    syscall
    hlt

    ; -------------------------------------------------------------
    ; Mode 6: Preempted Concurrent Fast Syscall Worker 2 (exits 92)
    ; Repeatedly issues 'syscall' interspersed with compute loops
    ; -------------------------------------------------------------
.mode_fast_worker_2:
    mov r12, 60                    ; 60 repeated syscall cycles
.loop_fast_worker_2:
    ; 1. CPU-bound compute loop
    mov rcx, 500000
.compute_fast_2:
    inc qword [g_bss_val]
    dec rcx
    jnz .compute_fast_2

    ; 2. Test user stack preservation across fast syscall
    push qword 0x5522
    mov rax, 1                     ; SYS_WRITE
    mov rdi, 1                     ; stdout
    lea rsi, [msg_worker_fast2]    ; "2"
    mov rdx, msg_worker_fast2_len
    syscall
    pop rcx
    cmp rcx, 0x5522
    jne .fail_fast_stack
    cmp rax, msg_worker_fast2_len
    jne .fail_fast_write

    dec r12
    jnz .loop_fast_worker_2

    ; Clean exit via fast 'syscall' with exit code 92
    mov rax, 0                     ; SYS_EXIT
    mov rdi, 92                    ; exit code 92
    syscall
    hlt

.fail_fast_write:
    mov rdi, 10
    jmp .do_exit

.fail_fast_int80:
    mov rdi, 11
    jmp .do_exit

.fail_fast_efault:
    mov rdi, 12
    jmp .do_exit

.fail_fast_einval:
    mov rdi, 13
    jmp .do_exit

.fail_fast_ebadf:
    mov rdi, 14
    jmp .do_exit

.fail_fast_enosys:
    mov rdi, 15
    jmp .do_exit

.fail_fast_stack:
    mov rdi, 16
    jmp .do_exit

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

    ; -------------------------------------------------------------
    ; Mode 7: Ring 3 VFS & Initramfs Acceptance Test
    ; -------------------------------------------------------------
.mode_ext2_test:
    mov eax, 2
    lea rdi, [ext2_path]
    xor esi, esi
    syscall
    test rax, rax
    js .ext2_fail
    mov r12, rax
    mov eax, 4
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 128
    syscall
    cmp rax, ext2_expected_len
    jne .ext2_fail
    lea rsi, [vfs_buf1]
    lea rdi, [ext2_expected]
    mov ecx, ext2_expected_len
    repe cmpsb
    jne .ext2_fail
    mov eax, 1
    mov edi, 1
    lea rsi, [vfs_buf1]
    mov edx, ext2_expected_len
    syscall
    cmp rax, ext2_expected_len
    jne .ext2_fail
    mov eax, 4
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 128
    syscall
    test rax, rax
    jnz .ext2_fail
    mov eax, 3
    mov rdi, r12
    syscall
    test rax, rax
    jnz .ext2_fail
    mov edi, 89
    jmp .do_exit
.ext2_fail:
    mov edi, 198
    jmp .do_exit

.mode_vfs_test:
    ; Announce start of test
    mov eax, 1          ; SYS_WRITE
    mov edi, 1          ; stdout
    lea rsi, [msg_vfs_start]
    mov edx, msg_vfs_start_len
    syscall

    ; 1. Open /etc/motd
    mov eax, 2          ; SYS_OPEN
    lea rdi, [path_motd]
    mov esi, 0          ; O_RDONLY
    syscall
    test rax, rax
    js .fail_vfs_open1
    mov r12, rax        ; r12 = fd1

    ; 2. Read first 32 bytes from fd1
    mov eax, 4          ; SYS_READ
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 32
    syscall
    cmp rax, 32
    jne .fail_vfs_read1

    ; 3. Print the read chunk to stdout
    mov eax, 1          ; SYS_WRITE
    mov edi, 1
    lea rsi, [vfs_buf1]
    mov edx, 32
    syscall

    ; 4. Open /etc/motd second time (independent fd)
    mov eax, 2          ; SYS_OPEN
    lea rdi, [path_motd]
    mov esi, 0          ; O_RDONLY
    syscall
    test rax, rax
    js .fail_vfs_open2
    mov r13, rax        ; r13 = fd2
    cmp r12, r13
    je .fail_vfs_fd_same

    ; 5. Read first 32 bytes from fd2 (must read from offset 0, independent of fd1!)
    mov eax, 4          ; SYS_READ
    mov rdi, r13
    lea rsi, [vfs_buf2]
    mov edx, 32
    syscall
    cmp rax, 32
    jne .fail_vfs_read2

    ; Verify vfs_buf2 matches vfs_buf1 (independent seek offsets!)
    mov rcx, 32
    lea rsi, [vfs_buf1]
    lea rdi, [vfs_buf2]
    repe cmpsb
    jne .fail_vfs_offset_mismatch

    ; 6. Read remainder from fd1 (short read test)
    mov eax, 4          ; SYS_READ
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 1024
    syscall
    test rax, rax
    jle .fail_vfs_short_read

    ; 7. Subsequent read from fd1 must return 0 (EOF)
    mov eax, 4          ; SYS_READ
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 1024
    syscall
    test rax, rax
    jnz .fail_vfs_eof

    ; 8. Negative Tests:
    ; 8a. Open non-existent file -> SYSCALL_ENOENT (-5)
    mov eax, 2          ; SYS_OPEN
    lea rdi, [path_bad]
    mov esi, 0
    syscall
    cmp rax, -5
    jne .fail_vfs_enoent

    ; 8b. Read with count=0 -> returns 0
    mov eax, 4          ; SYS_READ
    mov rdi, r12
    lea rsi, [vfs_buf1]
    xor edx, edx
    syscall
    test rax, rax
    jnz .fail_vfs_zero_len

    ; 8c. Read into read-only memory (path_motd is in .rodata) -> SYSCALL_EFAULT (-2)
    mov eax, 4          ; SYS_READ
    mov rdi, r13
    lea rsi, [path_motd]
    mov edx, 16
    syscall
    cmp rax, -2
    jne .fail_vfs_efault

    ; 9. Close both descriptors
    mov eax, 3          ; SYS_CLOSE
    mov rdi, r12
    syscall
    test rax, rax
    jnz .fail_vfs_close1

    mov eax, 3          ; SYS_CLOSE
    mov rdi, r13
    syscall
    test rax, rax
    jnz .fail_vfs_close2

    ; 10. Read from closed fd -> SYSCALL_EBADF (-3)
    mov eax, 4          ; SYS_READ
    mov rdi, r12
    lea rsi, [vfs_buf1]
    mov edx, 16
    syscall
    cmp rax, -3
    jne .fail_vfs_ebadf_closed

    ; All Ring 3 VFS acceptance assertions passed! Exit code 88
    mov rdi, 88
    jmp .do_exit

.fail_vfs_open1:
    mov rdi, 21
    jmp .do_exit
.fail_vfs_read1:
    mov rdi, 22
    jmp .do_exit
.fail_vfs_open2:
    mov rdi, 23
    jmp .do_exit
.fail_vfs_fd_same:
    mov rdi, 24
    jmp .do_exit
.fail_vfs_read2:
    mov rdi, 25
    jmp .do_exit
.fail_vfs_offset_mismatch:
    mov rdi, 26
    jmp .do_exit
.fail_vfs_short_read:
    mov rdi, 27
    jmp .do_exit
.fail_vfs_eof:
    mov rdi, 28
    jmp .do_exit
.fail_vfs_enoent:
    mov rdi, 29
    jmp .do_exit
.fail_vfs_zero_len:
    mov rdi, 30
    jmp .do_exit
.fail_vfs_efault:
    mov rdi, 31
    jmp .do_exit
.fail_vfs_close1:
    mov rdi, 32
    jmp .do_exit
.fail_vfs_close2:
    mov rdi, 33
    jmp .do_exit
.fail_vfs_ebadf_closed:
    mov rdi, 34
    jmp .do_exit

.fail_stack:
    mov rdi, 5
    jmp .do_exit

.mode_quick_exit:
    mov rax, 0          ; SYS_EXIT
    mov rdi, 42         ; exit code 42
    syscall
    hlt

.do_exit:
    mov rax, 0          ; SYS_EXIT
    int 0x80
    hlt


section .rodata
ext2_path: db "/mnt/hello.txt", 0
ext2_expected: db "Hello from FortressOS ext2 NVMe partition!", 10
ext2_expected_len equ $ - ext2_expected
