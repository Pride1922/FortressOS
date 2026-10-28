#ifndef FORTRESS_NET_WIFI_OWNER_H
#define FORTRESS_NET_WIFI_OWNER_H
#include "handoff.h"

/* Internal kinds, disjoint from NETCTL and UDP/TCP. Four copied records and
 * one <=4 MiB firmware allocation; no user pointer reaches the owner. */
#define NET_OWNER_WIFI 32u
#define NET_OWNER_WIFI_LAST 36u
int64_t net_wifi_syscall(uint64_t cmd, uintptr_t arg, size_t size);
void net_wifi_step(handoff_token_t token, handoff_request_t request);
void net_wifi_maintenance(void);
#endif
