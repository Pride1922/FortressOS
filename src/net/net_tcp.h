#ifndef FORTRESS_NET_TCP_H
#define FORTRESS_NET_TCP_H
#include "net.h"
#include "tcp_tcb.h"
#include "socket_abi.h"
typedef struct { unsigned slot; uint64_t generation, event, deadline_ticks, clock_floor; bool timed; } net_tcp_wait_t;
#define NET_TCP_IO_HORIZON_SECONDS 60U
/* Slots come only from a live common socket handle (0..NET_SOCKET_MAX-1).
 * Client queue/peek/consume/shutdown/snapshot require own BSP fd continuation
 * with IF clear. No caller buffer/pointer is retained. Protocol input/tick/
 * idle are sole-worker APIs. Close alone is AP-safe deferred publication. */
void net_tcp_init(net_dev_t *dev, const net_config_t *cfg);
void net_tcp_set_local_ip(uint32_t new_ip);
int64_t net_tcp_create(unsigned slot);
void net_tcp_close(unsigned slot); /* AP-safe publication, no protocol work. */
#define NET_TCP_BACKLOG_MAX 4U
typedef struct { int block; uint64_t generation; } net_tcp_child_t;
int64_t net_tcp_stage(unsigned slot); /* No transport block; BSP IF-clear. */
void net_tcp_unstage(unsigned slot);
int64_t net_tcp_listen(unsigned slot, unsigned backlog);
bool net_tcp_listener(unsigned slot);
int64_t net_tcp_accept_peek(unsigned slot, net_tcp_child_t *child, net_sockaddr_in_t *peer);
void net_tcp_accept_commit(unsigned listener, unsigned target, net_tcp_child_t child);
int64_t net_tcp_bind(unsigned slot, uint16_t port);
int64_t net_tcp_connect(unsigned slot, uint32_t ip, uint16_t port);
int64_t net_tcp_connect_until(unsigned slot, uint32_t ip, uint16_t port, uint64_t deadline);
int64_t net_tcp_connected(unsigned slot);
void net_tcp_cancel_connect(unsigned slot, uint64_t generation);
int64_t net_tcp_send(unsigned slot, const void *data, size_t len);
int64_t net_tcp_peek(unsigned slot, void *data, size_t capacity);
int64_t net_tcp_consume(unsigned slot, size_t len);
int64_t net_tcp_shutdown(unsigned slot);
bool net_tcp_snapshot(unsigned slot, net_tcp_wait_t *wait);
bool net_tcp_ready(void *wait); /* Atomic only; scheduler-lock safe. */
bool net_tcp_deadline_register(const net_tcp_wait_t *wait);
bool net_tcp_deadline_clock_ok(void); /* Atomic, fail closed on BSP clock reset/wrap. */
const void *net_tcp_channel(const net_tcp_wait_t *wait);
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len);
void net_tcp_tick(uint64_t ticks, bool online);
bool net_tcp_idle(void);
/* Sole BSP worker only; receive-capable connection present, including local
 * send half-close while the peer response is still arriving. */
bool net_tcp_receiving(void);
bool net_tcp_isn_crypto_guaranteed(void);
#endif
