#ifndef FORTRESS_NET_SOCKET_H
#define FORTRESS_NET_SOCKET_H
#include "net.h"
#include "socket_abi.h"
#include "../fs/vfs.h"
typedef struct { unsigned slot; uint64_t token; bool receive; } net_socket_wait_t;
void net_socket_init(net_dev_t *dev, const net_config_t *cfg);
void net_socket_enable(void);
void net_socket_set_local(uint32_t local_ip);
void net_socket_set_dev(net_dev_t *dev);
bool net_socket_available(void);
int64_t net_socket_create(file_t **out);
/* Client constructor split: caller allocation, owner endpoint activation.
 * A NULL prepared file still checks capacity first (ENOSPC before ENOMEM).
 * Final UDP close publishes detach; slot reuse waits for owner acknowledgement.
 * UDP mutation APIs below require registered CPU-0 owner, no other locks. */
int64_t net_socket_prepare(file_t **out);
int64_t net_socket_activate(file_t *file);
int64_t net_socket_activate_stream(file_t *file);
int64_t net_socket_activate_staged(file_t *file); /* Owner: handle only, no TCB. */
uint64_t net_socket_identity(file_t *file);
bool net_socket_identity_live(file_t *file, uint64_t identity);
int64_t net_socket_create_stream(file_t **out);
int64_t net_socket_stage_stream(file_t **out); /* Owner/test constructor: no transport allocation. */
void net_socket_finish_adopt(file_t *file);
bool net_socket_stream(file_t *file);
unsigned net_socket_index(file_t *file);
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
/* Owner integration replaces the legacy abandoned-caller lease with typed
 * cancellation/copy leases. Finite receive keeps its original deadline. */
void net_socket_retain(const net_socket_wait_t *wait,uint64_t deadline);
/* Sole BSP protocol worker: copied datagrams, no retained pbufs. */
void net_socket_input(uint32_t source, uint16_t sport, uint16_t dport,
                       const uint8_t *data, size_t len);
void net_socket_worker_tick(uint64_t now, bool online);
#endif
