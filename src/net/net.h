#ifndef FORTRESS_NET_STACK_H
#define FORTRESS_NET_STACK_H

#include "../include/net.h"
#include "../include/netctl_abi.h"
#include "eth.h"

typedef struct {
    uint32_t local_ip, gateway; /* Network order. */
    uint8_t prefix;
    bool malformed, test_arp, test_rings, test_icmp, test_udp, test_tcp;
} net_config_t;

/* Bounded cmdline: at most len bytes; duplicate net= tokens are malformed. */
void net_parse_config(const char *cmdline, size_t len, net_config_t *out);
/* Single BSP owner, unlocked thread context; buffers/cache are BSS-static.
 * init precedes worker publication; input owns and recycles each RX exactly once.
 * These APIs are not cross-core or reentrant socket APIs. */
void net_init(net_dev_t *dev, const net_config_t *config);
void net_input(net_dev_t *dev, pbuf_t *packet);
void arp_input(net_dev_t *dev, const uint8_t *payload, size_t len);
/* 0: hit, 1: request submitted (retry later), -1: invalid/send failed. */
int arp_resolve(net_dev_t *dev, uint32_t ip, uint8_t out_mac[ETH_ALEN]);
int net_arp_lookup(uint32_t ip, uint8_t out_mac[ETH_ALEN]);
void net_start(const char *cmdline, size_t len);
void net_worker_main(void *arg);
/* Called only on BSP from the existing APIC timer path, after EOI. */
void net_timer_tick(void);
extern const char g_net_poll_channel;

/* Snapshot current interface and protocol configuration for SYS_NETCTL. */
int net_get_ifconfig(netctl_ifget_t *out);
/* Validate and atomically apply runtime IPv4 configuration for SYS_NETCTL. */
int net_validate_ifset(const netctl_ifset_t *set, net_config_t *out_cfg);
void net_set_config(const net_config_t *new_cfg);

#endif
