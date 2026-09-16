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

/* Dispatcher & Test Lifecycle Hooks */
void    syscall_init(void);
int64_t syscall_dispatch(interrupt_frame_t *frame);

void    syscall_set_recovery(uintptr_t rip, uintptr_t rsp);
void    syscall_clear_recovery(void);
bool    syscall_was_exit_called(uint64_t *out_exit_code);

#endif /* FORTRESS_SYSCALL_H */
