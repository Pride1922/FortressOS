# NET-2 step 2 — bounded transport and deterministic simulator

2026-10-01. **IMPLEMENTED; host-simulator verified.** No live TCP sockets or
worker delivery enabled. The [pre-implementation layout/budget](../plans/TCP_TRANSPORT_DESIGN.md)
and [seven-step plan](../plans/NET2_PLAN.md) record scope and integration gates.
Headers/code are the implemented API. No scheduler/driver/UDP/ping modification.

## Engine and ownership

tcp_tcb.h/c supply caller-owned `tcp_cb_t` and `tcp_pool_t`, monotonic injected
millisecond time and explicit ISN/generation. No allocator, lock, scheduler,
user pointer, TCB pointer, DMA backing, NIC callback or protocol callback.
Only the caller serializes access. Active open and tuple-bound passive child
support SYN/ACK retransmission, simultaneous open and bounded handshake waits;
real listen/accept/backlog remains step 4. No production ISN/entropy claim.

Queue accepts partial bytes into TX ownership, retaining them until ACK or
failure. Retransmission ranges are metadata over retained circular bytes;
partial ACKs trim the first range. RX uses byte-indexed valid bits over the
same bounded circular buffer, handles gaps and duplicates and keeps the first
accepted overlapping bytes. Peek does not consume; consume is a separate
serialized owner operation. Buffered ordered data precedes EOF or reset error.
MSS is peer/default-536 limited by local MTU and the 1460 profile ceiling.
Window is unscaled and backed by actual RX sequence positions.

Prepare copies output into caller scratch without advancing sequence space.
Commit after successful local submission advances it and starts timers. Failed
local submission does not charge loss/retries/congestion. Generation/revision
reject stale or altered actions. Owner must resolve an action before protocol
RX or another NIC submission; integration must preserve this exclusion through
the unlocked driver call. Pure code does not prove that future concurrency.

RTO uses integer SRTT/RTTVAR, Karn exclusion, 1-second initial/floor and
60-second cap with backoff. Retransmitted SYN forces subsequent RTO >=3 seconds.
Reno includes slow start, byte-counted congestion avoidance, triple-duplicate
fast retransmit/recovery and timeout response. Zero-window persist is separate
from retry/congestion accounting, including unacknowledged FIN and lost window
update recovery. RST validates receive sequence; invalid in-window RST gets ACK.
No window scaling, ECN negotiation, SACK, timestamps or urgent application API.
SYN payload is outside this profile. Codec parsing is not option negotiation.

Shutdown schedules FIN after accepted data. Half-close, duplicate/out-of-order
FIN, simultaneous close, LAST_ACK, CLOSING and TIME_WAIT are supported. Orphan
teardown and orphan FIN_WAIT_2 have separate bounded deadlines. Best-effort reset
output expires after one second even if local submissions fail. Attached
endpoints retain buffered data/error until detached. Orphan TIME_WAIT exports
to a pre-reserved lightweight record; tuple reuse stays excluded through 2MSL
(240 seconds under the explicitly chosen 120-second MSL), restarted for
duplicate FIN. No automatic live-fd close hook is introduced in this step.

## Budget and frame evidence

Actual sizeof: CB 18200, retransmission descriptor 16 (32 each), output action
48, TIME_WAIT record 48 (16), full eight-CB pool 146392 bytes. The buffer total
is 131072, bitmaps 8192, descriptors 4096, remaining CB metadata 2240, TIME_WAIT
768 and pool bookkeeping 24. These are prospective live-pool costs; currently
only host tests allocate the pool. Later ABI/worker/frame scratch is additional.
No packet-size stack buffer. New code uses -Os, -Wframe-larger-than=512 and
-fstack-usage. Largest observed individual frame: tcp_pool_open 144 bytes
(dynamic, bounded); prepare 136; input 80; TIME_WAIT input 112. No complete
transitive/IRQ-stack bound or physical idle-CPU claim.

## Commands and verification

- `make test-net-tcp-tcb-host`: ASan/UBSan PASS, actual codec/engine with fake
  time and finite 256-packet simulated network. Ten directed groups cover
  submission/stale action, non-consuming RX, ACK trimming/wrap/invalid ACK,
  OOO/overlaps/FIN ordering, timers/Karn/backoff/retry exhaustion, RST/half-close,
  simultaneous open/close, Reno/window/persist/tiny MSS, pool/TIME_WAIT/tuple
  reuse, control/deadline cases and lost handshake/FIN/final ACK recovery.
  20000 additional checksum-valid hostile events assert sequence/byte/range
  invariants. Two MiB in each direction clean; three 256 KiB-per-direction
  fault schedules verify every byte exactly once, in order, beyond buffer size.
  Fault schedules include 80/85/80 wire drops, 117/122/117 duplicates,
  183/191/183 reorder injections and 61/65/62 local submission failures.
- `make bin/fortress.iso`: strict freestanding build/ISO PASS, no new warnings
  or frame-limit violation.
- `make test-net-tcp-host test-net-udp-host test-net-socket-host test-net-host
  test-net-eth-host test-net-ipv4-host test-net-icmp-host test-net-ping-host`:
  PASS, Phase 0 103/103. Host memory/scheduler/NIC adapters remain mocks.
- `make test-net-pci`: BIOS/UEFI present/absent boot regression 4/4 PASS;
  this is not TCP wire acceptance.
- `git diff --check`: PASS.

All simulated packets use actual TCP encode/decode. Directed tests assert
expected sequence/window/error behavior; the network is still engine-to-engine,
not an independent mainstream TCP peer. Live QEMU interoperability, user-copy,
fd/signal/lease semantics, worker fairness and physical TCP remain step 3+ gates.
The socket ABI remains a design baseline, not frozen by simulator success.
