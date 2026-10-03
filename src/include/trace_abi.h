#ifndef FORTRESS_TRACE_ABI_H
#define FORTRESS_TRACE_ABI_H
#include "types.h"
#define NETCTL_TRACE_PROBE 4u
#define NETTRACE_REPLY 0u
#define NETTRACE_HOP_EXPIRED 1u
#define NETTRACE_UNREACHABLE 2u
#define NETTRACE_PROBE_TIMEOUT 3u
#define NETTRACE_ARP_TIMEOUT 4u
#define NETTRACE_TX_FAILED 5u
#define NETTRACE_HORIZON_SECONDS 120u
typedef struct {
    uint32_t version, destination, ttl, timeout_seconds, sequence;
    uint32_t outcome, responder;
    uint8_t icmp_type, icmp_code;
    uint16_t reserved0;
    uint64_t rtt_ticks, tick_hz, deadline_ticks, reserved1;
} net_trace_v1_t;
_Static_assert(sizeof(net_trace_v1_t)==64,"trace size");
#define TRACE_OFFSET(f,n) _Static_assert(__builtin_offsetof(net_trace_v1_t,f)==n,"trace offset " #f)
TRACE_OFFSET(version,0); TRACE_OFFSET(destination,4); TRACE_OFFSET(ttl,8);
TRACE_OFFSET(timeout_seconds,12); TRACE_OFFSET(sequence,16); TRACE_OFFSET(outcome,20);
TRACE_OFFSET(responder,24); TRACE_OFFSET(icmp_type,28); TRACE_OFFSET(icmp_code,29);
TRACE_OFFSET(reserved0,30); TRACE_OFFSET(rtt_ticks,32); TRACE_OFFSET(tick_hz,40);
TRACE_OFFSET(deadline_ticks,48); TRACE_OFFSET(reserved1,56);
#undef TRACE_OFFSET
#endif
