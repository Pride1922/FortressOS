#ifndef FORTRESS_SOCKET_ABI_H
#define FORTRESS_SOCKET_ABI_H
#include "types.h"
#define NET_AF_INET 2u
#define NET_SOCK_DGRAM 2u
#define NET_SOCK_STREAM 1u
#define NET_SHUT_WR 1u
#define NET_SOCK_CLOEXEC 0x80000u
#define NET_MSG_DONTWAIT 0x40u
#define NET_SOCKET_MAX 16u
#define NET_SOCKET_RX_MAX 4u
#define NET_UDP_DATA_MAX 1472u
typedef struct {
    uint16_t family, port;
    uint32_t address;
    uint8_t reserved[8];
} net_sockaddr_in_t;
_Static_assert(sizeof(net_sockaddr_in_t)==16, "socket address size");
_Static_assert(_Alignof(net_sockaddr_in_t)==4, "socket address alignment");
_Static_assert(__builtin_offsetof(net_sockaddr_in_t,family)==0, "family offset");
_Static_assert(__builtin_offsetof(net_sockaddr_in_t,port)==2, "port offset");
_Static_assert(__builtin_offsetof(net_sockaddr_in_t,address)==4, "address offset");
_Static_assert(__builtin_offsetof(net_sockaddr_in_t,reserved)==8, "reserved offset");
#endif
