#ifndef FORTRESS_POLL_H
#define FORTRESS_POLL_H

#include "types.h"
#include "syscall_abi.h"

void poll_wake_notify(void);
int64_t sys_poll(uintptr_t user_fds, uint64_t nfds, int64_t timeout_ms);

#endif
