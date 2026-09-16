#ifndef FORTRESS_SYSCALL_H
#define FORTRESS_SYSCALL_H

#include "types.h"
#include "idt.h"

/* System Call Numbers */
#define SYS_EXIT   0
#define SYS_WRITE  1

/* System Call Error Codes */
#define SYSCALL_SUCCESS   0
#define SYSCALL_EINVAL   -1  /* Invalid argument / oversized length */
#define SYSCALL_EFAULT   -2  /* Bad address / inaccessible user memory */
#define SYSCALL_EBADF    -3  /* Invalid file descriptor */
#define SYSCALL_ENOSYS   -4  /* Unknown system call number */

/* Constraints */
#define MAX_SYSCALL_WRITE_LEN  16384

/*
 * =============================================================================
 * FortressOS Fast System Call ABI (x86_64 Long Mode)
 * =============================================================================
 * Instruction: 'syscall' (fast entry) / 'sysretq' (fast return)
 * Calling Convention:
 *   RAX = System call number
 *   RDI = Argument 1
 *   RSI = Argument 2
 *   RDX = Argument 3
 *   R10 = Argument 4 (matches System V syscall convention)
 *   R8  = Argument 5
 *   R9  = Argument 6
 * Return:
 *   RAX = Return value / negative error code (0 on success, < 0 on failure)
 * Hardware Clobbers:
 *   RCX = Overwritten by hardware with user RIP upon 'syscall'
 *   R11 = Overwritten by hardware with user RFLAGS upon 'syscall'
 * Callee-Preserved:
 *   RBX, RBP, R12, R13, R14, R15, RSP
 * Stack Invariant:
 *   Entry stub performs NO pushes, calls, or writes on the user stack.
 *   Switches to TSS.RSP0 immediately with interrupts disabled via SFMASK.
 * =============================================================================
 */

/* Dispatcher & Test Lifecycle Hooks */
void    syscall_init(void);
void    syscall_init_msrs(void);
bool    syscall_verify_msrs(void);
bool    syscall_validate_return_state(interrupt_frame_t *frame);
int64_t syscall_dispatch(interrupt_frame_t *frame);

void    syscall_set_recovery(uintptr_t rip, uintptr_t rsp);
void    syscall_clear_recovery(void);
bool    syscall_was_exit_called(uint64_t *out_exit_code);

#endif /* FORTRESS_SYSCALL_H */
