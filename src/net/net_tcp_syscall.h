#ifndef FORTRESS_NET_TCP_SYSCALL_H
#define FORTRESS_NET_TCP_SYSCALL_H
#include "idt.h"
int64_t net_tcp_syscall(interrupt_frame_t *frame);
#endif
