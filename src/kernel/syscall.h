#ifndef FORTRESS_SYSCALL_H
#define FORTRESS_SYSCALL_H

#include "types.h"
#include "idt.h"

/* System Call Numbers */
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_READ      4
#define SYS_STAT      5
#define SYS_READDIR   6
#define SYS_REBOOT    7
#define SYS_KBD_LAYOUT 8
#define SYS_SPAWN      9  /* (const char *path, const char *const argv[]) -> child PID */
#define SYS_WAIT       10 /* (uint64_t pid, int64_t *status or NULL) -> 0 */
#define SYS_MKDIR      11 /* (const char *path, uint64_t mode) -> 0 */
#define SYS_UNLINK     12 /* (const char *path) -> 0 */
#define SYS_RENAME     13 /* (const char *oldpath, const char *newpath) -> 0 */
#define SYS_SYNC       14 /* () -> 0; flush the writable /mnt device */
#define SYS_DMESG      15 /* (char *buf, uint64_t cap) -> bytes written */
#define DMESG_SIZE     (64 * 1024)

/* System Call Error Codes */
#define SYSCALL_SUCCESS   0
#define SYSCALL_EINVAL   -1  /* Invalid argument / oversized length */
#define SYSCALL_EFAULT   -2  /* Bad address / inaccessible user memory */
#define SYSCALL_EBADF    -3  /* Invalid file descriptor */
#define SYSCALL_ENOSYS   -4  /* Unknown system call number */
#define SYSCALL_ENOENT   -5  /* No such file or directory */
#define SYSCALL_EMFILE   -6  /* Too many open files */
#define SYSCALL_EISDIR   -7  /* Is a directory */
#define SYSCALL_ENOTDIR  -8  /* Not a directory */
#define SYSCALL_EIO      -9  /* I/O error / tainted filesystem */
#define SYSCALL_ENOMEM   -10 /* Out of memory */
#define SYSCALL_EROFS    -11 /* Read-only filesystem */
#define SYSCALL_EFBIG    -12 /* File too large / unsupported indirection */
#define SYSCALL_ENOSPC   -13 /* No space left on device */
#define SYSCALL_EOPNOTSUPP -14 /* Operation not supported */
#define SYSCALL_EEXIST   -15 /* File already exists */
#define SYSCALL_ECHILD   -16 /* Not an uncollected child of this process */
#define SYSCALL_ENOEXEC  -17 /* Invalid or unsupported executable */
#define SYSCALL_E2BIG    -18 /* Argument list or string too long */
#define SYSCALL_ENOTEMPTY -19 /* Directory not empty */

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
