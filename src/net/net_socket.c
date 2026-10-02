#include "net_socket.h"
#include "net_ipv4.h"
#include "syscall_abi.h"
#include "spinlock.h"
#include "thread.h"
#include "apic.h"
#include "heap.h"
#include "string.h"
#include "net_tcp.h"

typedef struct {
    net_sockaddr_in_t source;
    size_t len;
    uint8_t data[NET_UDP_DATA_MAX];
} datagram_t;
typedef struct {
    uint64_t token, done, expire, deadline;
    int64_t result;
    bool published;
} operation_t;
typedef struct {
    bool used, bound, wake, stream, staged_stream;
    uint16_t port; /* Host order; wildcard/local binds share a port namespace. */
    unsigned head, count;
    datagram_t rx[NET_SOCKET_RX_MAX], staged;
    uint8_t tx_data[NET_UDP_DATA_MAX];
    size_t tx_len;
    net_sockaddr_in_t dest;
    operation_t tx, receive;
    uint64_t dropped;
    char channel;
} socket_t;
static socket_t s_slots[NET_SOCKET_MAX];
static spinlock_t s_lock=SPINLOCK_RANKED(1,"socket_table");
static uint64_t s_generation;
static uint16_t s_ephemeral=49152;
static uint32_t s_local, s_mtu;
static bool s_enabled, s_online;
/* Single worker-owned snapshot; never a caller's user or DMA pointer. */
static uint8_t s_worker_data[NET_UDP_DATA_MAX];
static unsigned s_cursor;
static uint64_t s_unbound_dropped;

static uint64_t generation(void) { if (++s_generation==0) ++s_generation; return s_generation; }
static operation_t *operation(socket_t *s, bool receive) { return receive ? &s->receive : &s->tx; }
static void invalidate(operation_t *op) {
    op->published=false;
    __atomic_store_n(&op->token,0,__ATOMIC_RELEASE);
    __atomic_store_n(&op->done,0,__ATOMIC_RELEASE);
}
static int64_t unsupported_read(vfs_node_t *n, uint64_t o, void *b, size_t l) {
    (void)n; (void)o; (void)b; (void)l; return -VFS_EOPNOTSUPP;
}
static int64_t unsupported_write(vfs_node_t *n, uint64_t *o, bool a, const void *b, size_t l) {
    (void)n; (void)o; (void)a; (void)b; (void)l; return -VFS_EOPNOTSUPP;
}
static void socket_close(vfs_node_t *node) {
    socket_t *s=node->fs_private;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    /* Capture kind before freeing the common slot: an AP final-close may
     * overlap a BSP constructor attempting to reuse it after unlock. */
    bool stream=s->stream, staged=s->staged_stream;
    s->used=false; s->bound=false; s->count=0;
    s->wake=true;
    invalidate(&s->tx); invalidate(&s->receive);
    spin_unlock_irqrestore(&s_lock,irq);
    if (stream) {
        if (staged) net_tcp_unstage((unsigned)(s-s_slots));
        else net_tcp_close((unsigned)(s-s_slots));
    }
    /* Static channel/backing outlives every waiter. BSP worker wakes on next
     * pass; callback is also safe from an AP reaper, without scheduler calls. */
    kfree(node);
}
bool net_socket_file(file_t *f) { return f && f->node && f->node->close==socket_close; }
static socket_t *slot(file_t *f) { return net_socket_file(f) ? f->node->fs_private : NULL; }
bool net_socket_stream(file_t *f) { socket_t *s=slot(f); return s && s->stream; }
unsigned net_socket_index(file_t *f) { return (unsigned)(slot(f)-s_slots); }
void net_socket_init(net_dev_t *dev, const net_config_t *cfg) {
    memset(s_slots,0,sizeof(s_slots)); s_generation=0; s_ephemeral=49152;
    net_tcp_init(dev,cfg);
    s_local=cfg->local_ip; s_mtu=dev ? dev->mtu : 0; s_cursor=0;
    s_unbound_dropped=0;
    __atomic_store_n(&s_enabled,false,__ATOMIC_RELEASE);
    __atomic_store_n(&s_online,false,__ATOMIC_RELEASE);
}
void net_socket_enable(void) {
    __atomic_store_n(&s_online,true,__ATOMIC_RELEASE);
    __atomic_store_n(&s_enabled,true,__ATOMIC_RELEASE);
}
bool net_socket_available(void) {
    return __atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s_online,__ATOMIC_ACQUIRE) && apic_timer_get_frequency()!=0;
}
static int64_t create(file_t **out, bool stream, bool staged) {
    *out=NULL;
    if (!net_socket_available()) return SYSCALL_EIO;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    socket_t *s=NULL;
    for (unsigned i=0; i<NET_SOCKET_MAX; ++i) if (!s_slots[i].used) { s=&s_slots[i]; break; }
    if (!s) { spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_ENOSPC; }
    s->used=true; s->bound=false; s->head=s->count=0; s->dropped=0;
    s->stream=stream;
    s->staged_stream=staged;
    invalidate(&s->tx); invalidate(&s->receive);
    spin_unlock_irqrestore(&s_lock,irq);
    if (stream) {
        int64_t result=staged ? net_tcp_stage((unsigned)(s-s_slots)) : net_tcp_create((unsigned)(s-s_slots));
        if (result) {
            irq=spin_lock_irqsave(&s_lock); s->used=false; spin_unlock_irqrestore(&s_lock,irq);
            return result;
        }
    }
    vfs_node_t *node=kmalloc(sizeof(*node)); file_t *file=kmalloc(sizeof(*file));
    if (!node || !file) {
        kfree(node); kfree(file);
        if (stream) {
            if (staged) net_tcp_unstage((unsigned)(s-s_slots));
            else net_tcp_close((unsigned)(s-s_slots));
        }
        irq=spin_lock_irqsave(&s_lock); s->used=false; spin_unlock_irqrestore(&s_lock,irq);
        return SYSCALL_ENOMEM;
    }
    *node=(vfs_node_t){.type=VFS_STREAM,.is_stream=true,.fs_private=s,
        .read=unsupported_read,.write=unsupported_write,.close=socket_close};
    *file=(file_t){.node=node,.flags=VFS_O_RDWR,.ref_count=1};
    *out=file; return 0;
}
int64_t net_socket_create(file_t **out) { return create(out,false,false); }
int64_t net_socket_create_stream(file_t **out) { return create(out,true,false); }
int64_t net_socket_stage_stream(file_t **out) { return create(out,true,true); }
void net_socket_finish_adopt(file_t *file) {
    uint64_t irq=spin_lock_irqsave(&s_lock);
    socket_t *s=slot(file);
    if (!s || !s->staged_stream) __builtin_trap();
    s->staged_stream=false; spin_unlock_irqrestore(&s_lock,irq);
}
static bool occupied(uint16_t port) {
    for (unsigned i=0; i<NET_SOCKET_MAX; ++i)
        if (s_slots[i].used && !s_slots[i].stream && s_slots[i].bound && s_slots[i].port==port) return true;
    return false;
}
static int64_t bind_unlocked(socket_t *s, uint16_t port) {
    if (s->bound) return SYSCALL_EEXIST;
    if (port) { if (occupied(port)) return SYSCALL_EEXIST; }
    else {
        unsigned i;
        for (i=0; i<16384; ++i) {
            port=s_ephemeral; s_ephemeral=port==65535 ? 49152 : port+1;
            if (!occupied(port)) break;
        }
        if (i==16384) return SYSCALL_ENOSPC;
    }
    s->port=port; s->bound=true; return 0;
}
int64_t net_socket_bind(file_t *file, const net_sockaddr_in_t *a) {
    socket_t *s=slot(file);
    if (!s) return SYSCALL_EBADF;
    if (a->address && a->address!=s_local) return SYSCALL_EINVAL;
    if (!net_socket_available()) return SYSCALL_EIO;
    if (s->stream) return net_tcp_bind((unsigned)(s-s_slots),ntohs(a->port));
    uint64_t irq=spin_lock_irqsave(&s_lock);
    int64_t ret=bind_unlocked(s,ntohs(a->port));
    spin_unlock_irqrestore(&s_lock,irq); return ret;
}
static void reserve(operation_t *op, net_socket_wait_t *w, socket_t *s, bool receive) {
    uint64_t now=apic_timer_get_bsp_ticks(), hz=apic_timer_get_frequency();
    uint64_t token=generation(); op->expire=now+(receive ? 7 : 5)*hz;
    op->deadline=now+5*hz; op->published=false; op->result=0;
    __atomic_store_n(&op->done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&op->token,token,__ATOMIC_RELEASE);
    *w=(net_socket_wait_t){(unsigned)(s-s_slots),token,receive};
}
int64_t net_socket_send(file_t *file, const net_sockaddr_in_t *dest,
                        const void *data, size_t len, net_socket_wait_t *w) {
    socket_t *s=slot(file);
    if (!s) return SYSCALL_EBADF;
    if (!net_socket_available() || s_mtu<28) return SYSCALL_EIO;
    if (len>NET_UDP_DATA_MAX || len>s_mtu-28) return SYSCALL_EINVAL;
    uint32_t hop;
    if (!net_ipv4_route(dest->address,&hop)) return SYSCALL_EIO;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (s->tx.token) { spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_EAGAIN; }
    int64_t ret=s->bound ? 0 : bind_unlocked(s,0);
    if (ret) { spin_unlock_irqrestore(&s_lock,irq); return ret; }
    reserve(&s->tx,w,s,false); s->dest=*dest; s->tx_len=len;
    spin_unlock_irqrestore(&s_lock,irq);
    /* Called from BSP syscall with IF clear. Exclusive reservation, no user
     * copy under socket lock; worker cannot see data until published. */
    if (len) memcpy(s->tx_data,data,len);
    irq=spin_lock_irqsave(&s_lock);
    if (s->tx.token!=w->token) ret=SYSCALL_EINTR;
    else s->tx.published=true;
    spin_unlock_irqrestore(&s_lock,irq); return ret;
}
int64_t net_socket_receive(file_t *file, bool nonblocking, net_socket_wait_t *w) {
    socket_t *s=slot(file);
    if (!s) return SYSCALL_EBADF;
    if (!net_socket_available()) return SYSCALL_EIO;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (s->receive.token) { spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_EAGAIN; }
    int64_t ret=s->bound ? 0 : bind_unlocked(s,0);
    if (ret) { spin_unlock_irqrestore(&s_lock,irq); return ret; }
    if (nonblocking && !s->count) { spin_unlock_irqrestore(&s_lock,irq); return SYSCALL_EAGAIN; }
    reserve(&s->receive,w,s,true);
    if (s->count) __atomic_store_n(&s->receive.done,w->token,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_lock,irq); return 0;
}
const void *net_socket_channel(const net_socket_wait_t *w) { return &s_slots[w->slot].channel; }
bool net_socket_ready(void *arg) {
    const net_socket_wait_t *w=arg;
    operation_t *op=operation(&s_slots[w->slot],w->receive);
    return __atomic_load_n(&op->token,__ATOMIC_ACQUIRE)!=w->token ||
        __atomic_load_n(&op->done,__ATOMIC_ACQUIRE)==w->token;
}
bool net_socket_live(const net_socket_wait_t *w) {
    return __atomic_load_n(&operation(&s_slots[w->slot],w->receive)->token,__ATOMIC_ACQUIRE)==w->token;
}
int64_t net_socket_result(const net_socket_wait_t *w, const uint8_t **data,
                          net_sockaddr_in_t *source) {
    socket_t *s=&s_slots[w->slot]; operation_t *op=operation(s,w->receive);
    uint64_t irq=spin_lock_irqsave(&s_lock);
    int64_t ret;
    if (!s->used || op->token!=w->token) ret=SYSCALL_EINTR;
    else if (!net_socket_available() && !(op->done==w->token && !w->receive)) ret=SYSCALL_EIO;
    else if (op->done!=w->token) ret=SYSCALL_EAGAIN;
    else if (op->result<0 || !w->receive) ret=op->result;
    else if (!s->count) ret=SYSCALL_EAGAIN;
    else {
        s->staged=s->rx[s->head];
        *source=s->staged.source; *data=s->staged.data; ret=(int64_t)s->staged.len;
    }
    spin_unlock_irqrestore(&s_lock,irq); return ret;
}
void net_socket_finish(const net_socket_wait_t *w, bool consume) {
    socket_t *s=&s_slots[w->slot]; operation_t *op=operation(s,w->receive);
    uint64_t irq=spin_lock_irqsave(&s_lock);
    if (op->token==w->token) {
        if (w->receive && consume && s->count) { s->head=(s->head+1)%NET_SOCKET_RX_MAX; --s->count; }
        invalidate(op);
    }
    spin_unlock_irqrestore(&s_lock,irq);
}
void net_socket_input(uint32_t source, uint16_t sport, uint16_t dport,
                       const uint8_t *data, size_t len) {
    if (!dport || len>NET_UDP_DATA_MAX) return;
    uint64_t irq=spin_lock_irqsave(&s_lock);
    bool matched=false;
    for (unsigned i=0; i<NET_SOCKET_MAX; ++i) {
        socket_t *s=&s_slots[i];
        if (!s->used || s->stream || !s->bound || s->port!=dport) continue;
        matched=true;
        if (s->count==NET_SOCKET_RX_MAX) { ++s->dropped; break; }
        datagram_t *d=&s->rx[(s->head+s->count)%NET_SOCKET_RX_MAX];
        d->source=(net_sockaddr_in_t){.family=NET_AF_INET,.port=htons(sport),.address=source};
        d->len=len; if (len) memcpy(d->data,data,len); ++s->count; break;
    }
    if (!matched) ++s_unbound_dropped;
    spin_unlock_irqrestore(&s_lock,irq);
}
void net_socket_worker_tick(uint64_t now, bool online) {
    __atomic_store_n(&s_online,online,__ATOMIC_RELEASE);
    for (unsigned n=0; n<NET_SOCKET_MAX; ++n) {
        unsigned i=(s_cursor+n)%NET_SOCKET_MAX;
        socket_t *s=&s_slots[i];
        uint64_t irq=spin_lock_irqsave(&s_lock);
        bool wake=s->wake; s->wake=false;
        if (s->tx.token && now>=s->tx.expire) { invalidate(&s->tx); wake=true; }
        if (s->receive.token && now>=s->receive.expire) { invalidate(&s->receive); wake=true; }
        uint64_t token=s->used ? s->tx.token : 0;
        bool submit=token && s->tx.published;
        net_sockaddr_in_t dest=s->dest; size_t len=s->tx_len; uint16_t port=s->port;
        if (submit) { memcpy(s_worker_data,s->tx_data,len); s->tx.published=false; }
        if (s->receive.token && s->receive.done!=s->receive.token) {
            int64_t result=online ? (now>=s->receive.deadline ? SYSCALL_EAGAIN : 0) : SYSCALL_EIO;
            if (result || s->count) {
                s->receive.result=result;
                __atomic_store_n(&s->receive.done,s->receive.token,__ATOMIC_RELEASE);
                wake=true;
            }
        }
        bool completed=token && s->tx.done==token;
        spin_unlock_irqrestore(&s_lock,irq);
        /* Cancellation is generation-based, including final close/reuse. */
        net_ipv4_udp_sync(i,token);
        int64_t result=0; bool done=false;
        if (submit && !completed) {
            if (!online || !net_ipv4_udp_start(i,token,dest.address,port,ntohs(dest.port),
                                              s_worker_data,len,now)) { result=SYSCALL_EIO; done=true; }
        }
        if (token && !completed && !done) done=net_ipv4_udp_take(i,token,&result);
        if (token && !completed && !online) { result=SYSCALL_EIO; done=true; net_ipv4_udp_sync(i,0); }
        if (done) {
            irq=spin_lock_irqsave(&s_lock);
            if (s->used && s->tx.token==token && s->tx.done!=token) {
                s->tx.result=result; __atomic_store_n(&s->tx.done,token,__ATOMIC_RELEASE);
                wake=true;
            }
            spin_unlock_irqrestore(&s_lock,irq);
        }
        /* Static address; also wakes stale continuations after AP final-close. */
        if (wake) sched_wake_all(&s->channel);
    }
    s_cursor=(s_cursor+1)%NET_SOCKET_MAX;
}
