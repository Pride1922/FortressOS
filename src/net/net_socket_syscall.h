#ifndef FORTRESS_NET_SOCKET_SYSCALL_H
#define FORTRESS_NET_SOCKET_SYSCALL_H
#include "idt.h"
int64_t net_socket_syscall(interrupt_frame_t *frame);
#endif
