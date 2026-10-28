#ifndef FORTRESS_NET_CONTROL_H
#define FORTRESS_NET_CONTROL_H
#include "netctl_abi.h"
#include "handoff.h"
/* Fixed control/probe values; service also dispatches retained UDP/TCP and
 * copied Wi-Fi operations (net_wifi_owner.h).
 * Value storage belongs to the generic token until release.
 * No user address, caller stack, TCB, descriptor or callback crosses ownership. */
typedef union {
    netctl_ifget_t get;
    netctl_ifset_t set;
    net_ping_v1_t ping;
    net_trace_v1_t trace;
} net_control_value_t;
int64_t net_control_call(unsigned kind, net_control_value_t *value);
void net_control_start(void);
void net_control_register_owner(void); /* Once, at CPU-0 worker entry. */
void net_control_service(void); /* Registered CPU-0 worker only, bounded. */
void net_control_assert_owner(void);
#endif
