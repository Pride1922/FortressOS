# NET-2 step 2 — transport layout and simulator contract

2026-10-01, layout measured before implementation; now implemented and
host-simulator verified. Public layout is
src/net/tcp_tcb.h. Caller-owned serialized pure engine; no live allocation,
worker, socket, scheduler, syscall or driver changes in this step.

## Actual x86_64 sizeof budget

| Object | Size | Bound | Total |
| --- | ---: | ---: | ---: |
| tcp_cb_t | 18200 | 8 | 145600 |
| tcp_retx_t (included in CB) | 16 | 32 per CB | 4096 included |
| TX/RX bytes (included) | 8192 each | 8 CBs | 131072 included |
| RX occupancy bitmap (included) | 1024 | 8 | 8192 included |
| Remaining CB metadata (included) | 280 | 8 | 2240 included |
| tcp_timewait_t | 48 | 16 | 768 |
| Pool bookkeeping | 24 | 1 | 24 |
| tcp_pool_t | 146392 | 1 | 146392 |
| tcp_action_t | 48 | 1 worker scratch, later | 48 additional |
| Maximum worker data scratch | 1460 | 1, later | 1460 additional |

Host test allocation is static BSS. Kernel links the pure code but allocates
no CB pool until step 3. Existing UDP storage and capacity remain unchanged.
ABI operation storage, frame construction and syscall snapshots are step 3's
additional budget; the figures above are not a complete future socket budget.

RX uses one 8192-byte circular buffer with byte-valid bits. Out-of-order bytes
occupy their sequence positions, never a separate unbounded packet list. First
accepted bytes win conflicting overlaps. The advertised right edge is bounded
by unread data plus available circular positions, so gaps cannot overwrite
unread bytes. FIN is remembered separately and delivered only at rcv_nxt.
Retransmission descriptors retain sequence ranges into TX storage; partial ACK
trims ranges without duplicating bytes. 32 descriptors also bound flight when
the peer advertises an unusually tiny MSS. One send action per prepare/commit.

## API and transaction contract

Explicit monotonic millisecond clock, injected ISN and nonzero generation.
No production ISN/entropy claim. Active open and tuple-bound passive child
handshake; real listening/backlog and fd semantics belong to step 4/3.
Pure transport errors are not syscall errno values. Queue returns bytes accepted
and never promises submission. Peek is non-consuming; consume is a separate
owner-serialized operation. Buffer arguments cannot alias CB memory.

Prepare snapshots a header and copies bytes into caller scratch. Commit after
successful local submission advances sequence space and starts timers. Failed
local submission does neither. Revision/generation protects stale completion.
The sole owner must resolve the action before feeding protocol RX or submitting
another packet; a mutated/stale action must never be sent. Step 3 must preserve
this owner exclusion through its unlocked device call and validate generations.
Successful retransmission commit, not an attempted device call, changes retry,
RTO or congestion accounting. SYN/FIN consume one sequence number each.

## Time and close policy

Initial RTO 1000 ms; adaptive integer SRTT/RTTVAR with Karn exclusion, floor
1000 and cap 60000. SYN retransmission raises the subsequent data RTO to at
least 3000 ms. Eight retransmission attempts; handshake absolute deadline
30000 ms. Zero-window persist has a separate backed-off timer and does not
charge ordinary retry budget/congestion. Explicit user deadline is separate;
unset means no application data timeout. No keepalive in this profile.
Detached application has 120000 ms teardown budget; orphan FIN_WAIT_2 has
60000 ms. Live half-closed sockets do not acquire that orphan timer.
MSL is explicitly 120000 ms; TIME_WAIT is 240000 ms and restarts for duplicate
FIN. This is a conservative policy choice, not a measured LAN lifetime.

Pool reserves one of 16 tuple/generation TIME_WAIT records at every open.
An active block keeps that reservation until CLOSED or TIME_WAIT. On TIME_WAIT
export, large buffers are released while the tuple/sequence/deadline remain.
No tuple can reopen while reserved/active; pressure returns bounded no-space.
No half-implemented reuse shortcut or silent TIME_WAIT omission. The manager
uses no file/user/TCB/DMA pointers. Listener and accepted-child policies later.
Attached endpoints retain their buffered bytes/errors and their large CB until
the owner detaches. Only orphan TIME_WAIT is exported. Buffered bytes are read
before a pending reset error; no extra ACK is queued by reads on CLOSED/EOF.
Closed reset output is best effort for at most one second (the user-deadline
field is reused in CLOSED only), so failed local RST submission cannot pin an
orphan. The owner must demux the full IP tuple; input validates port fields.
Payload on SYN and URG application delivery are outside this initial profile;
they are not silently presented as ordinary stream bytes.

## Simulator gate

Real codec on every simulated packet, explicit fake time and finite queues.
Require ordered exactly-once application bytes through transfers much larger
than buffers, including both directions, drops/duplicates/reordering, partial
ACKs, wraparound, fast retransmit/recovery, timeout/backoff/Karn, zero-window
probe and lost window update. Cover SYN/FIN/ACK loss, simultaneous open/close,
invalid sequence/ACK/RST, half-close, FIN ordering, retransmission/pool bounds,
failed local submissions, stale actions, orphan timers and exported TIME_WAIT.
No simulator result substitutes for independent live-peer QEMU/hardware gates.

References: [TCP](https://www.rfc-editor.org/rfc/rfc9293.html),
[RTO](https://www.rfc-editor.org/rfc/rfc6298.html),
[Reno](https://www.rfc-editor.org/rfc/rfc5681.html).
