#include "net_tcp.h"
#include "net.h"
#include "net_ipv4.h"
#include "socket_abi.h"
#include "syscall_abi.h"
#include "spinlock.h"
#include "thread.h"
#include "apic.h"
#include "ipv4.h"
#include "string.h"

typedef struct {
    uint64_t generation, event, observed;
    uint64_t wake_deadline_ticks;
    bool wake_deadline_set;
    int block;
    int64_t error;
    uint16_t port;
    bool used, started, detach, cancel, wake;
    bool listening, passive;
    unsigned backlog;
    net_tcp_child_t pending[NET_TCP_BACKLOG_MAX];
    char channel;
} endpoint_t;
static endpoint_t endpoints[NET_SOCKET_MAX];
static tcp_pool_t pool;
/* Passive children retain full tuples, not exclusive listener port binds. */
static bool passive_tw[TCP_TIMEWAIT_MAX];
static spinlock_t lock=SPINLOCK_RANKED(1,"tcp_endpoints");
static net_dev_t *device;
static uint32_t local_ip;
static uint16_t ephemeral;
static uint64_t clock_ms, arp_next[TCP_CB_MAX];
static unsigned cursor;
static uint8_t scratch[TCP_MSS_MAX], frame[ETH_MAX_FRAME_LEN], mac[6];
static tcp_action_t action;
static tcp_tuple_t tw_tuple;
static tcp_header_t tw_ack;
static bool tw_pending;
static bool action_inflight;
static uint64_t deadline_clock_last;
static bool deadline_clock_failed;

static uint64_t milliseconds(uint64_t ticks) {
    uint64_t hz=apic_timer_get_frequency();
    if (!hz) return clock_ms;
    uint64_t seconds=ticks/hz;
    if (seconds>UINT64_MAX/1000) return UINT64_MAX;
    return seconds*1000+(ticks%hz)*1000/hz;
}
static uint64_t irq_off(void) {
#ifdef TEST_SMP_MEMORY
    return 0;
#else
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli":"=r"(f)::"memory"); return f;
#endif
}
static void irq_restore(uint64_t f) {
#ifdef TEST_SMP_MEMORY
    (void)f;
#else
    __asm__ volatile("push %0; popfq"::"r"(f):"memory","cc");
#endif
}
static void publish(endpoint_t *e) {
    if (__atomic_load_n(&e->event,__ATOMIC_RELAXED)==UINT64_MAX) {
        e->wake_deadline_set=false;
        e->error=SYSCALL_EIO;
        __atomic_store_n(&e->generation,0,__ATOMIC_RELEASE); e->wake=true; return;
    }
    __atomic_add_fetch(&e->event,1,__ATOMIC_RELEASE); e->wake=true;
}
static void deadline_expire(endpoint_t *e) {
    /* Runtime fence: expiry/event publication must stay outside TX transactions. */
    if (action_inflight) __builtin_trap();
    if (e->wake_deadline_set && (deadline_clock_failed || apic_timer_get_bsp_ticks()>=e->wake_deadline_ticks)) {
        e->wake_deadline_set=false; publish(e);
    }
}
#ifdef TEST_SMP_MEMORY
void net_tcp_test_expiry_guard(void) {
    action_inflight=true; deadline_expire(&endpoints[0]);
}
void net_tcp_test_event_exhaustion(unsigned slot) {
    uint64_t flags=spin_lock_irqsave(&lock);
    __atomic_store_n(&endpoints[slot].event,UINT64_MAX,__ATOMIC_RELEASE);
    publish(&endpoints[slot]);
    spin_unlock_irqrestore(&lock,flags);
}
#endif
static tcp_conn_t *connection(endpoint_t *e) {
    return e->block>=0 && pool.used[e->block] ? &pool.blocks[e->block] : NULL;
}
static int64_t error_code(tcp_conn_t *c, bool connecting) {
    if (!c) return SYSCALL_ENOTCONN;
    if (c->error==TCP_RESET) return connecting ? SYSCALL_ECONNREFUSED : SYSCALL_ECONNRESET;
    if (c->error==TCP_TIMEOUT) return SYSCALL_ETIMEDOUT;
    return SYSCALL_ENOTCONN;
}
static bool tuple_equal(tcp_tuple_t a, tcp_tuple_t b) {
    return a.local_ip==b.local_ip && a.remote_ip==b.remote_ip &&
        a.local_port==b.local_port && a.remote_port==b.remote_port;
}
static bool port_used(uint16_t port, endpoint_t *self) {
    for (unsigned i=0; i<NET_SOCKET_MAX; ++i)
        if (&endpoints[i]!=self && endpoints[i].used && !endpoints[i].passive && endpoints[i].port==port) return true;
    /* Orphans and TIME_WAIT also retain local ports: conservative no sharing. */
    for (unsigned i=0; i<TCP_TIMEWAIT_MAX; ++i)
        if (pool.timewait[i].used && !passive_tw[i] && pool.timewait[i].tuple.remote_ip &&
            pool.timewait[i].tuple.local_port==port &&
            (!self || !connection(self) || pool.timewait[i].generation!=self->generation)) return true;
    return false;
}
static uint8_t boot_secret[32];
static bool isn_crypto_guaranteed = false;

bool net_tcp_isn_crypto_guaranteed(void) {
    return isn_crypto_guaranteed;
}

static inline uint64_t read_tsc(void) {
#ifdef TEST_SMP_MEMORY
    return (uint64_t)apic_timer_get_bsp_ticks() * 1000000ULL;
#else
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#endif
}

static uint32_t ror32(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_hash(const void *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t offset = 0;
    const uint8_t *p = (const uint8_t *)data;

    while (len - offset >= 64) {
        memcpy(block, p + offset, 64);
        sha256_block(state, block);
        offset += 64;
    }

    size_t rem = len - offset;
    memcpy(block, p + offset, rem);
    block[rem++] = 0x80;
    if (rem > 56) {
        memset(block + rem, 0, 64 - rem);
        sha256_block(state, block);
        rem = 0;
    }
    memset(block + rem, 0, 56 - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (unsigned i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    }
    sha256_block(state, block);

    for (unsigned i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

static uint32_t initial_sequence(tcp_tuple_t tuple, uint64_t now) {
    (void)now;
    /* RFC 6528: ISN = M + H(secret || src_ip || src_port || dst_ip || dst_port || counter)
     * Using TSC as both monotonic M and snapshot counter. */
    uint64_t tsc_now = read_tsc();
    uint32_t m = (uint32_t)tsc_now;

    struct {
        uint8_t secret[32];
        uint32_t local_ip;
        uint16_t local_port;
        uint32_t remote_ip;
        uint16_t remote_port;
        uint64_t counter;
    } input;

    memcpy(input.secret, boot_secret, 32);
    input.local_ip = tuple.local_ip;
    input.local_port = tuple.local_port;
    input.remote_ip = tuple.remote_ip;
    input.remote_port = tuple.remote_port;
    input.counter = tsc_now;

    uint8_t digest[32];
    sha256_hash(&input, sizeof(input), digest);

    uint32_t h = ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
                 ((uint32_t)digest[2] << 8)  | (uint32_t)digest[3];
    return m + h;
}

void net_tcp_init(net_dev_t *dev, const net_config_t *cfg) {
    action_inflight = false;
    deadline_clock_last = apic_timer_get_bsp_ticks();
    __atomic_store_n(&deadline_clock_failed, false, __ATOMIC_RELEASE);
    device = dev; local_ip = cfg->local_ip; ephemeral = 49152; cursor = 0;
    clock_ms = milliseconds(apic_timer_get_bsp_ticks());
    tw_pending = false;

    /* Harvest boot entropy: RDRAND/RDSEED primary when supported by CPU.
     * Note: Boot entropy without RDRAND is not a cryptographic guarantee. */
    struct {
        uint64_t hw_random[4];
        uint64_t tsc_sample;
        uint64_t apic_ticks;
        uint32_t local_ip;
        uint8_t  mac[6];
        uint8_t  cmos_rtc[6];
    } entropy_pool;
    memset(&entropy_pool, 0, sizeof(entropy_pool));

    entropy_pool.tsc_sample = read_tsc();
    entropy_pool.apic_ticks = apic_timer_get_bsp_ticks();
    entropy_pool.local_ip   = local_ip;
    if (device) memcpy(entropy_pool.mac, device->mac_addr, 6);

#ifndef TEST_SMP_MEMORY
    /* Sample CMOS RTC date/time registers */
    for (uint8_t reg = 0; reg < 6; reg++) {
        __asm__ volatile("outb %0, $0x70" : : "a"(reg) : "memory");
        uint8_t val;
        __asm__ volatile("inb $0x71, %0" : "=a"(val) : : "memory");
        entropy_pool.cmos_rtc[reg] = val;
    }

    /* CPUID check for RDRAND (leaf 1, ECX bit 30) and RDSEED (leaf 7, EBX bit 18) */
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    bool has_rdrand = (ecx & (1u << 30)) != 0;

    eax = 7; ecx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    bool has_rdseed = (ebx & (1u << 18)) != 0;

    unsigned collected = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint64_t val = 0;
        unsigned char ok = 0;
        if (has_rdseed) {
            __asm__ volatile("rdseed %0; setc %1" : "=r"(val), "=qm"(ok));
        }
        if (!ok && has_rdrand) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        }
        if (ok) {
            entropy_pool.hw_random[i] = val;
            collected++;
        }
    }
    isn_crypto_guaranteed = (collected == 4);
#endif

    sha256_hash(&entropy_pool, sizeof(entropy_pool), boot_secret);

    memset(endpoints, 0, sizeof(endpoints)); memset(arp_next, 0, sizeof(arp_next)); tcp_pool_init(&pool);
    memset(passive_tw, 0, sizeof(passive_tw));
    for (unsigned i = 0; i < NET_SOCKET_MAX; ++i) {
        endpoints[i].block = -1;
        for (unsigned j = 0; j < NET_TCP_BACKLOG_MAX; ++j) endpoints[i].pending[j].block = -1;
    }
}
void net_tcp_set_local_ip(uint32_t new_ip) {
    local_ip = new_ip;
}
int64_t net_tcp_create(unsigned slot) {
    if (slot>=NET_SOCKET_MAX || !device) return SYSCALL_EIO;
    uint64_t flags=spin_lock_irqsave(&lock);
    endpoint_t *e=&endpoints[slot];
    if (e->used || e->detach) { spin_unlock_irqrestore(&lock,flags); return SYSCALL_ENOSPC; }
    /* Unique placeholder reserves a real block and TIME_WAIT capacity at socket
     * creation. It is never demuxed or submitted before CONNECT initializes it. */
    tcp_tuple_t tuple={local_ip,0,(uint16_t)(slot+1),1};
    int block=tcp_pool_open(&pool,tuple,0,(uint16_t)device->mtu,false,clock_ms);
    if (block<0) { spin_unlock_irqrestore(&lock,flags); return SYSCALL_ENOSPC; }
    pool.blocks[block].state=TCP_CLOSED;
    passive_tw[pool.tw_slot[block]]=false;
    e->block=block; e->used=true; e->started=e->cancel=e->detach=false;
    e->port=0; e->error=0; e->observed=UINT64_MAX;
    e->wake_deadline_set=false; e->wake_deadline_ticks=0;
    e->listening=e->passive=false; e->backlog=0;
    __atomic_store_n(&e->event,0,__ATOMIC_RELEASE);
    __atomic_store_n(&e->generation,pool.blocks[block].generation,__ATOMIC_RELEASE);
    publish(e); spin_unlock_irqrestore(&lock,flags); return 0;
}
int64_t net_tcp_stage(unsigned slot) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    int64_t result=0;
    if (e->used || e->detach) result=SYSCALL_ENOSPC;
    else {
        e->used=true; e->block=-1; e->started=e->listening=e->wake=e->passive=false;
        e->port=0; e->error=0; e->backlog=0;
        e->wake_deadline_set=false; e->wake_deadline_ticks=0;
    }
    spin_unlock_irqrestore(&lock,flags); return result;
}
void net_tcp_unstage(unsigned slot) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    if (!e->used || e->block!=-1 || e->started || e->generation) __builtin_trap();
    e->used=false; spin_unlock_irqrestore(&lock,flags);
}
bool net_tcp_listener(unsigned slot) { return endpoints[slot].used && endpoints[slot].listening; }
int64_t net_tcp_listen(unsigned slot, unsigned backlog) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    tcp_conn_t *c=connection(e); int64_t result=0;
    if (!backlog || backlog>NET_TCP_BACKLOG_MAX || !e->port || e->started || !c) result=SYSCALL_EINVAL;
    else {
        e->started=e->listening=true; e->backlog=backlog;
        c->tuple.local_port=e->port; c->state=TCP_LISTEN;
        publish(e);
    }
    spin_unlock_irqrestore(&lock,flags); return result;
}
static tcp_conn_t *pending_connection(net_tcp_child_t child) {
    return child.block>=0 && child.block<(int)TCP_CB_MAX && pool.used[child.block] &&
        pool.blocks[child.block].generation==child.generation ? &pool.blocks[child.block] : NULL;
}
int64_t net_tcp_accept_peek(unsigned slot, net_tcp_child_t *child, net_sockaddr_in_t *peer) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    int64_t result=e->error ? e->error : SYSCALL_EAGAIN;
    if (!net_tcp_listener(slot)) result=SYSCALL_EINVAL;
    else if (!e->error) for (unsigned i=0; i<e->backlog; ++i) {
        tcp_conn_t *c=pending_connection(e->pending[i]);
        if (c && (c->state==TCP_ESTABLISHED || c->state==TCP_CLOSE_WAIT)) {
            *child=e->pending[i];
            *peer=(net_sockaddr_in_t){.family=NET_AF_INET,.port=htons(c->tuple.remote_port),.address=c->tuple.remote_ip};
            result=0; break;
        }
    }
    spin_unlock_irqrestore(&lock,flags); return result;
}
void net_tcp_accept_commit(unsigned listener, unsigned target, net_tcp_child_t child) {
    uint64_t flags=spin_lock_irqsave(&lock);
    endpoint_t *e=&endpoints[listener], *accepted=&endpoints[target];
    tcp_conn_t *c=pending_connection(child); unsigned i=0;
    for (; i<e->backlog; ++i) if (e->pending[i].block==child.block && e->pending[i].generation==child.generation) break;
    if (!net_tcp_listener(listener) || i==e->backlog || !c ||
        !accepted->used || accepted->block!=-1 || accepted->generation ||
        (c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT)) __builtin_trap();
    /* Ownership linearization: fd is inserted already, but BSP IF stays clear.
     * No allocation/copy/signal check/sleep/tick can interrupt this transfer. */
    accepted->block=child.block; accepted->port=c->tuple.local_port;
    accepted->passive=true;
    accepted->started=true; accepted->observed=UINT64_MAX;
    accepted->wake_deadline_set=false; accepted->wake_deadline_ticks=0;
    __atomic_store_n(&accepted->event,0,__ATOMIC_RELEASE);
    __atomic_store_n(&accepted->generation,child.generation,__ATOMIC_RELEASE);
    e->pending[i].block=-1; publish(e); publish(accepted);
    spin_unlock_irqrestore(&lock,flags);
}
void net_tcp_close(unsigned slot) {
    if (slot>=NET_SOCKET_MAX) return;
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    e->used=false; e->detach=true;
    e->wake_deadline_set=false; e->wake_deadline_ticks=0;
    __atomic_store_n(&e->generation,0,__ATOMIC_RELEASE); publish(e);
    spin_unlock_irqrestore(&lock,flags);
}
int64_t net_tcp_bind(unsigned slot, uint16_t port) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    int64_t result=0;
    if (e->port || e->started) result=SYSCALL_EINVAL;
    else if (port && port_used(port,e)) result=SYSCALL_EADDRINUSE;
    else {
        if (!port) {
            unsigned n=0;
            do { port=ephemeral; ephemeral=port==65535 ? 49152 : port+1; }
            while (port_used(port,e) && ++n<16384);
            if (n==16384) result=SYSCALL_ENOSPC;
        }
        if (!result) e->port=port;
    }
    spin_unlock_irqrestore(&lock,flags); return result;
}
static int64_t connect_start(unsigned slot, uint32_t ip, uint16_t port, bool timed, uint64_t deadline) {
    uint32_t hop;
    if (!net_ipv4_route(ip,&hop)) return SYSCALL_EINVAL;
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    tcp_conn_t *c=connection(e); int64_t result=0;
    if (!e->used || !c) result=SYSCALL_ENOTCONN;
    else if (e->started) result=SYSCALL_EISCONN;
    spin_unlock_irqrestore(&lock,flags);
    if (result) return result;
    if (timed && apic_timer_get_bsp_ticks()>=deadline) return SYSCALL_ETIMEDOUT;
    if (!e->port && (result=net_tcp_bind(slot,0))) return result;
    flags=spin_lock_irqsave(&lock);
    tcp_tuple_t tuple={local_ip,ip,e->port,port};
    unsigned tw=pool.tw_slot[e->block];
    for (unsigned i=0; i<TCP_TIMEWAIT_MAX; ++i)
        if (i!=tw && pool.timewait[i].used && tuple_equal(pool.timewait[i].tuple,tuple)) result=SYSCALL_EADDRINUSE;
    if (!result) {
        uint64_t now=milliseconds(apic_timer_get_bsp_ticks());
        result=tcp_conn_init(c,tuple,e->generation,initial_sequence(tuple,now),
            (uint16_t)device->mtu,true,now);
        if (!result) { pool.timewait[tw].tuple=tuple; e->started=true; publish(e); }
    }
    spin_unlock_irqrestore(&lock,flags);
    if (!result) net_request_poll();
    return result;
}
int64_t net_tcp_connect(unsigned slot, uint32_t ip, uint16_t port) {
    return connect_start(slot,ip,port,false,0);
}
int64_t net_tcp_connect_until(unsigned slot, uint32_t ip, uint16_t port, uint64_t deadline) {
    return connect_start(slot,ip,port,true,deadline);
}
int64_t net_tcp_connected(unsigned slot) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    tcp_conn_t *c=connection(e); int64_t result;
    if (e->error) result=e->error;
    else if (!e->started || !c) result=SYSCALL_ENOTCONN;
    else if (c->state==TCP_SYN_SENT || c->state==TCP_SYN_RCVD) result=SYSCALL_EAGAIN;
    else if (c->state==TCP_CLOSED) result=error_code(c,true);
    else result=0;
    spin_unlock_irqrestore(&lock,flags); return result;
}
void net_tcp_cancel_connect(unsigned slot, uint64_t generation) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[slot];
    if (e->used && e->generation==generation) { e->cancel=true; e->error=SYSCALL_ENOTCONN; publish(e); }
    spin_unlock_irqrestore(&lock,flags);
}
int64_t net_tcp_send(unsigned slot, const void *data, size_t len) {
    /* BSP IF-clear continuation; no protocol mutation during unlocked copy. */
    endpoint_t *e=&endpoints[slot]; tcp_conn_t *c=connection(e);
    if (e->error) return e->error;
    if (!e->started || !c || c->state==TCP_SYN_SENT || c->state==TCP_SYN_RCVD) return SYSCALL_ENOTCONN;
    if (c->want_fin) return SYSCALL_EPIPE;
    if (c->state==TCP_CLOSED) return error_code(c,false);
    int result=tcp_conn_queue(c,data,len);
    if (result==TCP_WOULD_BLOCK) return SYSCALL_EAGAIN;
    if (result<0) return SYSCALL_ENOTCONN;
    uint64_t flags=spin_lock_irqsave(&lock);
    /* A later worker batch may restore the same count sampled before this
     * user operation. Invalidate the sample so its readiness event is fresh. */
    e->observed=UINT64_MAX; publish(e); spin_unlock_irqrestore(&lock,flags);
    if (result>0) net_request_poll();
    return result;
}
int64_t net_tcp_peek(unsigned slot, void *data, size_t capacity) {
    endpoint_t *e=&endpoints[slot]; tcp_conn_t *c=connection(e);
    if (e->error) return e->error;
    if (!e->started || !c || c->state==TCP_SYN_SENT || c->state==TCP_SYN_RCVD) return SYSCALL_ENOTCONN;
    int result=tcp_conn_peek(c,data,capacity);
    if (result==TCP_WOULD_BLOCK) return SYSCALL_EAGAIN;
    if (result<0) return error_code(c,false);
    return result;
}
int64_t net_tcp_consume(unsigned slot, size_t len) {
    tcp_conn_t *c=connection(&endpoints[slot]);
    int result=tcp_conn_consume(c,len);
    uint64_t flags=spin_lock_irqsave(&lock);
    /* Do not miss equal-sized RX batches arriving after this consume but before
     * the worker has sampled the intervening empty queue. */
    endpoints[slot].observed=UINT64_MAX; publish(&endpoints[slot]); spin_unlock_irqrestore(&lock,flags);
    if (!result && len) net_request_poll();
    return result;
}
int64_t net_tcp_shutdown(unsigned slot) {
    tcp_conn_t *c=connection(&endpoints[slot]);
    int result=tcp_conn_shutdown(c);
    if (!result) net_request_poll();
    return result ? SYSCALL_ENOTCONN : 0;
}
bool net_tcp_snapshot(unsigned slot, net_tcp_wait_t *w) {
    endpoint_t *e=&endpoints[slot];
    w->timed=false; w->deadline_ticks=0; w->clock_floor=0;
    w->slot=slot; w->generation=__atomic_load_n(&e->generation,__ATOMIC_ACQUIRE);
    w->event=__atomic_load_n(&e->event,__ATOMIC_ACQUIRE); return w->generation!=0;
}
bool net_tcp_ready(void *arg) {
    const net_tcp_wait_t *w=arg; endpoint_t *e=&endpoints[w->slot];
    return __atomic_load_n(&e->generation,__ATOMIC_ACQUIRE)!=w->generation ||
        __atomic_load_n(&e->event,__ATOMIC_ACQUIRE)!=w->event ||
        (w->timed && (!net_tcp_deadline_clock_ok() || apic_timer_get_bsp_ticks()<w->clock_floor ||
            apic_timer_get_bsp_ticks()>=w->deadline_ticks));
}
bool net_tcp_deadline_clock_ok(void) { return !__atomic_load_n(&deadline_clock_failed,__ATOMIC_ACQUIRE); }
bool net_tcp_deadline_register(const net_tcp_wait_t *w) {
    uint64_t flags=spin_lock_irqsave(&lock); endpoint_t *e=&endpoints[w->slot];
    bool valid=e->used && e->generation==w->generation && w->timed;
    if(valid && (!e->wake_deadline_set || w->deadline_ticks<e->wake_deadline_ticks)) {
        e->wake_deadline_ticks=w->deadline_ticks; e->wake_deadline_set=true;
    }
    spin_unlock_irqrestore(&lock,flags); return valid;
}
const void *net_tcp_channel(const net_tcp_wait_t *w) { return &endpoints[w->slot].channel; }
static bool submit(tcp_tuple_t tuple, const tcp_header_t *h, const void *data, size_t len) {
    uint32_t hop;
    if (!device || !net_ipv4_route(tuple.remote_ip,&hop) || net_arp_lookup(hop,mac)) return false;
    memset(frame,0,sizeof(frame));
    size_t header=h->has_mss ? 24 : 20;
    if (tcp_encode(frame+34,sizeof(frame)-34,tuple.local_ip,tuple.remote_ip,h,data,len) ||
        ipv4_encode(frame+14,sizeof(frame)-14,tuple.local_ip,tuple.remote_ip,6,(uint16_t)(header+len),64,NULL) ||
        eth_encode(frame,sizeof(frame),mac,device->mac_addr,ETHERTYPE_IPV4,NULL)) return false;
    size_t n=34+header+len; if (n<60) n=60;
    return device->send_packet(device,frame,n)==0;
}
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len) {
    tcp_header_t h; const uint8_t *payload; size_t n;
    if (tcp_decode(data,len,source,local_ip,&h,&payload,&n)) return;
    uint64_t outer=irq_off();
    tcp_tuple_t tuple={local_ip,source,h.destination,h.source};
    for (unsigned i=0; i<TCP_CB_MAX; ++i) if (pool.used[i] &&
        pool.blocks[i].tuple.remote_ip && tuple_equal(pool.blocks[i].tuple,tuple)) {
        tcp_conn_input(&pool.blocks[i],&h,payload,n,milliseconds(apic_timer_get_bsp_ticks()));
        irq_restore(outer); return;
    }
    tcp_header_t ack;
    if (!tw_pending && tcp_pool_timewait_input(&pool,tuple,&h,n,milliseconds(apic_timer_get_bsp_ticks()),&ack)) {
        tw_tuple=tuple; tw_ack=ack; tw_pending=true;
        irq_restore(outer); return;
    }
    /* A peer may offer ECN on SYN; ordinary SYN/ACK declines negotiation. */
    if ((h.flags&~(TCP_ECE|TCP_CWR))==TCP_SYN && !n && net_ipv4_unicast(source) && h.source && h.destination) {
        uint64_t flags=spin_lock_irqsave(&lock);
        for (unsigned i=0; i<NET_SOCKET_MAX; ++i) {
            endpoint_t *e=&endpoints[i];
            if (!e->used || !e->listening || e->error || e->port!=h.destination) continue;
            unsigned j=0;
            for (; j<e->backlog; ++j) if (e->pending[j].block<0) break;
            if (j<e->backlog) {
                uint64_t now=milliseconds(apic_timer_get_bsp_ticks());
                int block=tcp_pool_open(&pool,tuple,initial_sequence(tuple,now),(uint16_t)device->mtu,false,now);
                if (block>=0) {
                    passive_tw[pool.tw_slot[block]]=true;
                    arp_next[block]=0;
                    e->pending[j]=(net_tcp_child_t){block,pool.blocks[block].generation};
                    tcp_conn_input(&pool.blocks[block],&h,payload,n,now); publish(e);
                }
            }
            break; /* Queue/pool overflow silently drops the new SYN. */
        }
        spin_unlock_irqrestore(&lock,flags);
    }
    irq_restore(outer);
}
void net_tcp_tick(uint64_t ticks, bool online) {
    if (action_inflight) __builtin_trap();
    uint64_t fresh=apic_timer_get_bsp_ticks();
    bool clock_backwards=fresh<deadline_clock_last;
    deadline_clock_last=fresh;
    uint64_t now=milliseconds(ticks); if (now>clock_ms) clock_ms=now;
    for (unsigned n=0; n<NET_SOCKET_MAX; ++n) {
        unsigned slot=(cursor+n)%NET_SOCKET_MAX; endpoint_t *e=&endpoints[slot];
        uint64_t outer=irq_off(), flags=spin_lock_irqsave(&lock);
        /* Boot resets uptime before shell start. Only an outstanding timed
         * hint depends on the previous epoch. Read the hint under its manager
         * lock (AP final close may clear it); each continuation also checks
         * its own entry clock floor on retry. */
        if (clock_backwards && e->wake_deadline_set)
            __atomic_store_n(&deadline_clock_failed,true,__ATOMIC_RELEASE);
        tcp_conn_t *c=connection(e);
        if (e->detach || e->cancel) {
            for (unsigned j=0; j<e->backlog; ++j) {
                tcp_conn_t *pending=pending_connection(e->pending[j]);
                if (pending) tcp_conn_detach(pending,clock_ms);
                e->pending[j].block=-1;
            }
            if (c) tcp_conn_detach(c,clock_ms);
            e->listening=false; e->backlog=0;
            e->block=-1;
            e->detach=e->cancel=false; publish(e); c=NULL;
        }
        if (c && e->started) {
            unsigned ready=0;
            for (unsigned j=0; j<e->backlog; ++j) {
                tcp_conn_t *pending=pending_connection(e->pending[j]);
                if (!pending) { e->pending[j].block=-1; continue; }
                tcp_conn_tick(pending,clock_ms);
                if (pending->state==TCP_CLOSED) {
                    pending->orphan=true; e->pending[j].block=-1; publish(e);
                } else if (pending->state==TCP_ESTABLISHED || pending->state==TCP_CLOSE_WAIT) ready|=1U<<j;
            }
            tcp_conn_tick(c,clock_ms);
            if (!online && !e->error) e->error=SYSCALL_EIO;
            uint64_t rx_token=(c->rx_count&0xffffU)^(c->rx_count>>16);
            uint64_t observed=rx_token | ((uint64_t)c->tx_count<<16) |
                ((uint64_t)c->state<<32) | ((uint64_t)(uint8_t)(-c->error)<<40) |
                ((uint64_t)c->eof<<48) | ((uint64_t)c->want_fin<<49) |
                ((uint64_t)(uint8_t)(-e->error)<<50) | ((uint64_t)ready<<58);
            if (observed!=e->observed) { e->observed=observed; publish(e); }
        }
        deadline_expire(e);
        bool wake=e->wake; e->wake=false;
        spin_unlock_irqrestore(&lock,flags); irq_restore(outer);
        if (wake) sched_wake_all(&e->channel);
    }
    /* Sweep every active/orphan block even when RX batches are full. One local
     * TX action per block per pass; no retained packet actions through ARP. */
    for (unsigned n=0; n<TCP_CB_MAX; ++n) {
        unsigned i=(cursor+n)%TCP_CB_MAX;
        if (!pool.used[i] || !pool.blocks[i].tuple.remote_ip) continue;
        uint64_t outer=irq_off();
        tcp_conn_t *c=&pool.blocks[i]; tcp_conn_tick(c,clock_ms);
        if (c->orphan && c->rx_count) (void)tcp_conn_consume(c,c->rx_count);
        if (!online) { irq_restore(outer); continue; }
        /* Load-bearing transaction: tick/input/consume invalidate prepared
         * actions. Keep BSP IF clear from prepare through commit (including
         * failed submission); no reentrant RX/tick or sleep in this interval.
         * AP close only publishes deferred detach. Pool maintenance below
         * must run after every transaction has committed. */
        if (!tcp_conn_prepare(c,&action,scratch,sizeof(scratch))) {
            action_inflight=true;
            bool sent=false; uint32_t hop;
            if (net_ipv4_route(c->tuple.remote_ip,&hop)) {
                if (net_arp_lookup(hop,mac)) {
                    if (clock_ms>=arp_next[i]) {
                        (void)arp_resolve(device,hop,mac); arp_next[i]=clock_ms+1000;
                    }
                } else sent=submit(c->tuple,&action.header,scratch,action.data_len);
            }
            /* Local send failure still commits with submitted=false. A false
             * commit means this transaction was invalidated/reordered, not
             * ordinary ARP/NIC backpressure: never silently lose the action. */
            if (!tcp_conn_commit(c,&action,sent)) __builtin_trap();
            action_inflight=false;
        }
        irq_restore(outer);
    }
    uint64_t outer=irq_off();
    if (tw_pending) { if (online) (void)submit(tw_tuple,&tw_ack,NULL,0); tw_pending=false; }
    /* All per-block prepare/commit transactions above are finished; this
     * sweep ticks connections again and may invalidate action revisions. */
    tcp_pool_tick(&pool,clock_ms); irq_restore(outer);
    cursor=(cursor+1)%NET_SOCKET_MAX;
}
bool net_tcp_idle(void) {
    for (unsigned i=0; i<TCP_CB_MAX; ++i) if (pool.used[i] && pool.blocks[i].tuple.remote_ip &&
        (pool.blocks[i].retx_count || pool.blocks[i].tx_count || pool.blocks[i].ack_pending)) return false;
    return true;
}
bool net_tcp_receiving(void) {
    for (unsigned i=0;i<TCP_CB_MAX;i++)
        if (pool.used[i] && !pool.blocks[i].orphan &&
            (pool.blocks[i].state==TCP_ESTABLISHED ||
             pool.blocks[i].state==TCP_FIN_WAIT_1 ||
             pool.blocks[i].state==TCP_FIN_WAIT_2)) return true;
    return false;
}
