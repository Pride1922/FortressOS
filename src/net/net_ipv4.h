#ifndef FORTRESS_NET_IPV4_STACK_H
#define FORTRESS_NET_IPV4_STACK_H
#include "net.h"
#include "ping_abi.h"
/* Worker-only entry points. Every buffer is copied before return. */
void net_ipv4_init(net_dev_t *dev, const net_config_t *config);
void net_ipv4_input(const uint8_t *packet, size_t len);
void net_ipv4_tick(uint64_t now);
bool net_ipv4_unicast(uint32_t ip);
bool net_ipv4_route(uint32_t destination, uint32_t *hop); /* Immutable config only. */
uint32_t net_ipv4_local(void);
bool net_ipv4_idle(void); /* Worker-only, opt-in idle accounting. */
/* Sole worker, bounded independent UDP slots; token prevents stale completion. */
bool net_ipv4_udp_start(unsigned slot, uint64_t token, uint32_t destination,
                        uint16_t sport, uint16_t dport, const uint8_t *data,
                        size_t len, uint64_t now);
void net_ipv4_udp_sync(unsigned slot, uint64_t token);
bool net_ipv4_udp_take(unsigned slot, uint64_t token, int64_t *result);
bool net_ipv4_ping_start(const net_ping_v1_t *request, uint64_t token, uint64_t now);
bool net_ipv4_ping_take(net_ping_v1_t *result);
void net_ipv4_ping_cancel(uint64_t token);
#endif
