#ifndef FORTRESS_NET_SOCKET_H
#define FORTRESS_NET_SOCKET_H
#include "net.h"
#include "socket_abi.h"
#include "../fs/vfs.h"
typedef struct { unsigned slot; uint64_t token; bool receive; } net_socket_wait_t;
void net_socket_init(net_dev_t *dev, const net_config_t *cfg);
void net_socket_enable(void);
bool net_socket_available(void);
int64_t net_socket_create(file_t **out);
bool net_socket_file(file_t *file);
int64_t net_socket_bind(file_t *file, const net_sockaddr_in_t *address);
int64_t net_socket_send(file_t *file, const net_sockaddr_in_t *dest,
                        const void *data, size_t len, net_socket_wait_t *wait);
int64_t net_socket_receive(file_t *file, bool nonblocking, net_socket_wait_t *wait);
const void *net_socket_channel(const net_socket_wait_t *wait);
bool net_socket_ready(void *wait); /* Atomic only under scheduler lock. */
bool net_socket_live(const net_socket_wait_t *wait);
int64_t net_socket_result(const net_socket_wait_t *wait, const uint8_t **data,
                          net_sockaddr_in_t *source);
void net_socket_finish(const net_socket_wait_t *wait, bool consume);
/* Sole BSP protocol worker: copied datagrams, no retained pbufs. */
void net_socket_input(uint32_t source, uint16_t sport, uint16_t dport,
                       const uint8_t *data, size_t len);
void net_socket_worker_tick(uint64_t now, bool online);
#endif
