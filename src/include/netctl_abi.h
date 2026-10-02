#ifndef FORTRESS_NETCTL_ABI_H
#define FORTRESS_NETCTL_ABI_H

#include "types.h"
#include "ping_abi.h"

/* SYS_NETCTL = 42 command codes */
#ifndef NETCTL_PING
#define NETCTL_PING  1u
#endif
#define NETCTL_IFGET 2u
#define NETCTL_IFSET 3u

/* Stable link state ABI values */
#define NET_IF_LINK_UNINITIALIZED 0u
#define NET_IF_LINK_WAITING       1u
#define NET_IF_LINK_ONLINE        2u
#define NET_IF_LINK_DOWN          3u
#define NET_IF_LINK_FAILED        4u

typedef struct {
    uint32_t struct_version;   /*  0, 4: must be 1 on input */
    uint32_t reserved;         /*  4, 4: must be 0 on input */
    uint8_t  mac[6];           /*  8, 6: hardware MAC address */
    uint16_t reserved2;        /* 14, 2: 0 on output */
    uint32_t local_ipv4;       /* 16, 4: network byte order; 0 if unconfigured */
    uint32_t netmask_ipv4;     /* 20, 4: network byte order; 0 if unconfigured */
    uint32_t gateway_ipv4;     /* 24, 4: network byte order; 0 if unconfigured */
    uint32_t mtu;              /* 28, 4: interface MTU (typically 1500) */
    uint32_t link_state;       /* 32, 4: NET_IF_LINK_* stable ABI value */
    uint32_t reserved3;        /* 36, 4: padding for 8B alignment; 0 on output */
    uint64_t rx_packets;       /* 40, 8: driver packets received */
    uint64_t tx_packets;       /* 48, 8: driver packets transmitted */
    uint32_t reserved4[2];     /* 56, 8: reserved; 0 on output */
} netctl_ifget_t;

_Static_assert(sizeof(netctl_ifget_t) == 64, "netctl_ifget_t size must be 64");
_Static_assert(__builtin_offsetof(netctl_ifget_t, struct_version) == 0,  "offset version");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved)       == 4,  "offset reserved");
_Static_assert(__builtin_offsetof(netctl_ifget_t, mac)            == 8,  "offset mac");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved2)      == 14, "offset reserved2");
_Static_assert(__builtin_offsetof(netctl_ifget_t, local_ipv4)     == 16, "offset local_ip");
_Static_assert(__builtin_offsetof(netctl_ifget_t, netmask_ipv4)   == 20, "offset netmask");
_Static_assert(__builtin_offsetof(netctl_ifget_t, gateway_ipv4)   == 24, "offset gateway");
_Static_assert(__builtin_offsetof(netctl_ifget_t, mtu)            == 28, "offset mtu");
_Static_assert(__builtin_offsetof(netctl_ifget_t, link_state)     == 32, "offset link_state");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved3)      == 36, "offset reserved3");
_Static_assert(__builtin_offsetof(netctl_ifget_t, rx_packets)     == 40, "offset rx_packets");
_Static_assert(__builtin_offsetof(netctl_ifget_t, tx_packets)     == 48, "offset tx_packets");
_Static_assert(__builtin_offsetof(netctl_ifget_t, reserved4)      == 56, "offset reserved4");

typedef struct {
    uint32_t struct_version;   /*  0, 4: must be 1 */
    uint32_t reserved;         /*  4, 4: must be 0 */
    uint32_t local_ipv4;       /*  8, 4: network byte order */
    uint32_t netmask_ipv4;     /* 12, 4: network byte order */
    uint32_t gateway_ipv4;     /* 16, 4: network byte order; 0 = no gateway */
    uint32_t flags;            /* 20, 4: must be 0 in v1 */
    uint32_t reserved2[2];     /* 24, 8: must be 0 */
} netctl_ifset_t;

_Static_assert(sizeof(netctl_ifset_t) == 32, "netctl_ifset_t size must be 32");
_Static_assert(__builtin_offsetof(netctl_ifset_t, struct_version) == 0,  "offset version");
_Static_assert(__builtin_offsetof(netctl_ifset_t, reserved)       == 4,  "offset reserved");
_Static_assert(__builtin_offsetof(netctl_ifset_t, local_ipv4)     == 8,  "offset local_ip");
_Static_assert(__builtin_offsetof(netctl_ifset_t, netmask_ipv4)   == 12, "offset netmask");
_Static_assert(__builtin_offsetof(netctl_ifset_t, gateway_ipv4)   == 16, "offset gateway");
_Static_assert(__builtin_offsetof(netctl_ifset_t, flags)          == 20, "offset flags");
_Static_assert(__builtin_offsetof(netctl_ifset_t, reserved2)      == 24, "offset reserved2");

#endif /* FORTRESS_NETCTL_ABI_H */
