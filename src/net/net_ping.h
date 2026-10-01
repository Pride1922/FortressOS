#ifndef FORTRESS_NET_PING_H
#define FORTRESS_NET_PING_H
#include "ping_abi.h"
extern const char g_net_ping_channel;
void net_ping_init(bool available);
int64_t net_ping_submit(const net_ping_v1_t *request, uint64_t *token);
bool net_ping_ready(void *token); /* Lock-free predicate under scheduler lock. */
int64_t net_ping_collect(uint64_t token, net_ping_v1_t *result);
void net_ping_cancel(uint64_t token);
void net_ping_worker_tick(uint64_t now);
#endif
