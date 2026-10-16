#ifndef FORTRESS_NET_UDP_OWNER_H
#define FORTRESS_NET_UDP_OWNER_H
#include "net_socket.h"
#include "handoff.h"
enum { NET_OWNER_UDP_CREATE=16, NET_OWNER_UDP_BIND, NET_OWNER_UDP_SEND, NET_OWNER_UDP_RECV,
    NET_OWNER_TCP_CREATE, NET_OWNER_TCP_BIND, NET_OWNER_TCP_CONNECT,
    NET_OWNER_TCP_SEND, NET_OWNER_TCP_RECV, NET_OWNER_TCP_SHUTDOWN };
/* Shared UDP/TCP retained records and eight 64 KiB payload credits.
 * Own caller continuation, IF clear, no locks. begin retains a checked file
 * reference; cancellation/death transfers cleanup to the CPU-0 owner. */
int64_t net_udp_begin(unsigned kind,file_t *file,const net_sockaddr_in_t *address,
                       size_t len,bool nonblocking);
int64_t net_tcp_owner_begin(unsigned kind,file_t *file,const net_sockaddr_in_t *address,
    size_t len,bool nonblocking,bool timed,uint64_t deadline,uint64_t floor);
void net_tcp_owner_cancel(int64_t status);
bool net_tcp_owner_connect_ack(void); /* Publish bounded completion acknowledgement. */
bool net_udp_ready(void *token); /* Stage or terminal result, atomic predicate. */
int64_t net_udp_collect(handoff_reply_t *reply);
bool net_udp_pin(const uint8_t **data,int64_t *result,net_sockaddr_in_t *source);
void net_udp_unpin(bool commit);
file_t *net_udp_created(void); /* Acquire fd reference after successful result. */
void net_udp_release(void);
void net_udp_abandon(void);
void net_udp_step(handoff_token_t token,handoff_request_t request);
void net_udp_maintenance(void); /* <=8 records, registered owner only. */
/* Atomic level predicate, safe under scheduler lock. Caller decisions and
 * eligible payload grants remain runnable until the owner consumes them. */
bool net_udp_owner_work(void);
void net_udp_refresh_work(void); /* Owner-only readiness publication after sweeps/service. */
#endif
