#ifndef FORTRESS_PING_ABI_H
#define FORTRESS_PING_ABI_H
#include "types.h"
#define NETCTL_PING 1u
#define NETPING_REPLY 0u
#define NETPING_ARP_TIMEOUT 1u
#define NETPING_ECHO_TIMEOUT 2u
#define NETPING_TX_FAILED 3u
typedef struct {
    uint32_t version, destination, timeout_seconds, sequence;
    uint32_t outcome, echoed_bytes;
    uint64_t rtt_ticks, tick_hz;
    uint32_t start_delay_ms, reserved;
} net_ping_v1_t;
_Static_assert(sizeof(net_ping_v1_t)==48, "ping v1 size");
#define PING_OFFSET(field, n) _Static_assert(__builtin_offsetof(net_ping_v1_t,field)==n,"ping offset " #field)
PING_OFFSET(version,0); PING_OFFSET(destination,4); PING_OFFSET(timeout_seconds,8);
PING_OFFSET(sequence,12); PING_OFFSET(outcome,16); PING_OFFSET(echoed_bytes,20);
PING_OFFSET(rtt_ticks,24); PING_OFFSET(tick_hz,32); PING_OFFSET(start_delay_ms,40); PING_OFFSET(reserved,44);
#undef PING_OFFSET
#endif
