#ifndef FORTRESS_SYSCALL_H
#define FORTRESS_SYSCALL_H

#include "types.h"
#include "idt.h"

#include "syscall_abi.h"

/* Dispatcher & Test Lifecycle Hooks */
void    syscall_init(void);
void    syscall_init_msrs(void);
bool    syscall_verify_msrs(void);
bool    syscall_validate_return_state(interrupt_frame_t *frame);
int64_t syscall_dispatch(interrupt_frame_t *frame);

int64_t syscall_from_vfs_error(int64_t vfs_err);

void    syscall_set_recovery(uintptr_t rip, uintptr_t rsp);
void    syscall_clear_recovery(void);
bool    syscall_was_exit_called(uint64_t *out_exit_code);

#endif /* FORTRESS_SYSCALL_H */
