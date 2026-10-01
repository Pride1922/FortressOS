#ifndef FORTRESS_NET_IPV4_STACK_H
#define FORTRESS_NET_IPV4_STACK_H
#include "net.h"
#include "ping_abi.h"
/* Worker-only entry points. Every buffer is copied before return. */
void net_ipv4_init(net_dev_t *dev, const net_config_t *config);
void net_ipv4_input(const uint8_t *packet, size_t len);
void net_ipv4_tick(uint64_t now);
bool net_ipv4_unicast(uint32_t ip);
bool net_ipv4_ping_start(const net_ping_v1_t *request, uint64_t token, uint64_t now);
bool net_ipv4_ping_take(net_ping_v1_t *result);
void net_ipv4_ping_cancel(uint64_t token);
#endif
