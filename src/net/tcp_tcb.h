#ifndef FORTRESS_TCP_TCB_H
#define FORTRESS_TCP_TCB_H
#include "tcp.h"

#define TCP_CB_MAX 8U
#define TCP_TIMEWAIT_MAX 16U
#define TCP_BUFFER_SIZE 8192U
#define TCP_RETX_MAX 32U
#define TCP_MSS_MAX 1460U
#define TCP_TIMEWAIT_MS 240000U /* 2 * explicitly chosen 120-second MSL. */
#define TCP_HANDSHAKE_MS 30000U
#define TCP_ORPHAN_MS 120000U
#define TCP_FINWAIT2_MS 60000U

typedef enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING,
    TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;
/* Internal engine results, NOT syscall errno numbers. */
enum { TCP_OK=0, TCP_WOULD_BLOCK=-1, TCP_INVALID=-2, TCP_RESET=-3,
       TCP_TIMEOUT=-4, TCP_NOT_CONNECTED=-5, TCP_WRITE_CLOSED=-6, TCP_NO_SPACE=-7 };
typedef struct {
    uint32_t local_ip, remote_ip; /* Network order. */
    uint16_t local_port, remote_port; /* Host order. */
} tcp_tuple_t;
typedef struct {
    uint64_t sent_ms;
    uint32_t sequence;
    uint16_t length; /* Sequence-space length; SYN/FIN are each one. */
    uint8_t flags;
    bool retransmitted;
} tcp_retx_t;
typedef enum { TCP_ACTION_ACK, TCP_ACTION_NEW, TCP_ACTION_RETX,
               TCP_ACTION_PROBE, TCP_ACTION_RESET } tcp_action_kind_t;
typedef struct {
    uint64_t generation, revision;
    tcp_header_t header;
    uint16_t data_len;
    uint8_t kind;
    bool timeout;
} tcp_action_t;
typedef struct {
    tcp_tuple_t tuple;
    uint64_t generation, revision, now_ms;
    uint64_t rto_deadline, persist_deadline, handshake_deadline;
    uint64_t user_deadline, finwait2_deadline, timewait_deadline;
    tcp_state_t state;
    int error;
    uint32_t iss, irs, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint32_t rcv_nxt, rx_sequence, tx_sequence;
    uint32_t cwnd, ssthresh, ca_acked, srtt_ms, rttvar_ms, rto_ms;
    uint32_t fin_sequence, remote_fin_sequence;
    uint16_t local_mss, peer_mss, mss, tx_head, tx_count, rx_head, rx_count;
    uint8_t retx_count, retries, dupacks, persist_backoff;
    bool syn_sent, syn_ack, ack_pending, retransmit_pending, retransmit_timeout;
    bool want_fin, fin_sent, fin_acked, remote_fin_pending, eof;
    bool fast_recovery, have_rtt, orphan, reset_pending, action_pending;
    tcp_header_t reset_header;
    tcp_action_t action;
    tcp_retx_t retx[TCP_RETX_MAX];
    uint8_t tx[TCP_BUFFER_SIZE], rx[TCP_BUFFER_SIZE];
    /* Byte-indexed OOO occupancy shares the RX buffer, no extra payload pool. */
    uint8_t rx_valid[TCP_BUFFER_SIZE/8];
} tcp_conn_t;
typedef struct {
    tcp_tuple_t tuple;
    uint64_t generation, expires_ms;
    uint32_t snd_nxt, rcv_nxt;
    bool used, active;
} tcp_timewait_t;
typedef struct {
    tcp_conn_t blocks[TCP_CB_MAX];
    tcp_timewait_t timewait[TCP_TIMEWAIT_MAX];
    uint64_t generation;
    bool used[TCP_CB_MAX];
    uint8_t tw_slot[TCP_CB_MAX];
} tcp_pool_t;

/* Pure caller-owned engine. Serialized by its owner, never internally locked.
 * Clock is monotonic milliseconds; caller supplies generation and ISN.
 * Passive mode is a single tuple-bound child, not a listening backlog.
 * Owner demuxes the full IP/port tuple before input (header has no IPs).
 * Header/data input already passed codec validation; no pointers retained.
 * Non-aliasing caller buffers for queue/peek/prepare (not inside the CB). */
bool tcp_seq_before(uint32_t a, uint32_t b); /* Half-space ambiguous => false. */
int tcp_conn_init(tcp_conn_t *cb, tcp_tuple_t tuple, uint64_t generation,
                uint32_t isn, uint16_t mtu, bool active, uint64_t now_ms);
void tcp_conn_input(tcp_conn_t *cb, const tcp_header_t *h, const uint8_t *data,
                   size_t len, uint64_t now_ms);
void tcp_conn_tick(tcp_conn_t *cb, uint64_t now_ms);
int tcp_conn_queue(tcp_conn_t *cb, const void *data, size_t len);
int tcp_conn_peek(const tcp_conn_t *cb, void *data, size_t capacity);
int tcp_conn_consume(tcp_conn_t *cb, size_t len);
int tcp_conn_shutdown(tcp_conn_t *cb);
void tcp_conn_detach(tcp_conn_t *cb, uint64_t now_ms);
/* Prepare copies bytes into caller scratch, never commits sequence/timers.
 * One outstanding action. Commit(true) only after successful local submission.
 * Commit(false) abandons it without a network-loss/congestion charge.
 * Any intervening mutation makes its revision stale. Owner must resolve an
 * action before protocol RX/next NIC submission; stale actions must not be sent. */
int tcp_conn_prepare(tcp_conn_t *cb, tcp_action_t *action, void *data, size_t capacity);
bool tcp_conn_commit(tcp_conn_t *cb, const tcp_action_t *action, bool submitted);

/* Optional bounded lifecycle manager. Reserves a TIME_WAIT record at open,
 * keeps detached transports until CLOSED/TIME_WAIT, then releases big buffers.
 * Buffered data/error on an attached endpoint keeps its CB alive. Closed-state
 * reset output is best effort for at most one second, then orphan can be freed.
 * No wildcard listeners/fds/user pointers. Not allocated in live kernel yet. */
void tcp_pool_init(tcp_pool_t *pool);
int tcp_pool_open(tcp_pool_t *pool, tcp_tuple_t tuple, uint32_t isn,
                  uint16_t mtu, bool active, uint64_t now_ms);
void tcp_pool_tick(tcp_pool_t *pool, uint64_t now_ms);
/* Handles duplicate FIN in exported TIME_WAIT. Returns an ACK header, or false.
 * Matching RST is ignored; duplicate FIN restarts 2MSL. */
bool tcp_pool_timewait_input(tcp_pool_t *pool, tcp_tuple_t tuple,
                              const tcp_header_t *h, size_t len, uint64_t now_ms,
                              tcp_header_t *ack);
#endif
