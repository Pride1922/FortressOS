# UDP socket ABI — Phase 5 pre-coding contract

2026-10-01. Written before coding; now implemented in Phase 5a (syscalls 38–41).
Approved scope: BSP-only callers, five-second blocking receive, 16 global
sockets, four RX datagrams per socket, and at most 1472 payload bytes.
Companion: [Phase 5 plan](NET_PHASE5_PLAN.md).

**SYS_NETCTL=42 is unchanged: NETCTL_PING=1, its 48-byte v1 structure,
command set, return/error semantics and existing behavior are preserved.**

## 1. Calling convention and constants

RAX holds the syscall number and signed result. Arguments use RDI, RSI, RDX,
R10, R8, R9; RCX/R11 are clobbered by the existing entry mechanism.
Inspect arguments as full 64-bit register values before narrowing; reject
out-of-range integer arguments rather than accepting truncated aliases.
Descriptor arguments outside 0–31 return EBADF.

| Number | Operation | Registers / result |
| --- | --- | --- |
| 38 | SOCKET | RDI=domain, RSI=type, RDX=protocol; returns fd |
| 39 | BIND | RDI=fd, RSI=address, RDX=16; returns 0 |
| 40 | SENDTO | RDI=fd, RSI=data, RDX=length, R10=0, R8=destination, R9=16; returns data length |
| 41 | RECVFROM | RDI=fd, RSI=data, RDX=capacity, R10=flags, R8=source or null, R9=source-size pointer or null; returns copied length |
| existing 3 | CLOSE | Existing descriptor API and semantics |

AF_INET=2; SOCK_DGRAM=2; protocol=0 or 17. SOCKET optionally accepts
SOCK_CLOEXEC=0x80000 in type, mapped to the existing descriptor CLOEXEC flag.
Other family/type/protocol values return EOPNOTSUPP. RECVFROM flags are 0 or
MSG_DONTWAIT=0x40; other flags return EINVAL. SENDTO accepts flags=0 only.
All four new calls require current CPU=0 and caller affinity=0, including
nonblocking calls. Generic close/dup/fd cleanup retains its existing contract.

## 2. Complete address layout

Implemented shared header: src/include/socket_abi.h. Ordinary C layout, not packed:

```c
typedef struct {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t reserved[8];
} net_sockaddr_in_t;
```

Size=16, alignment=4, no implicit gaps or tail padding. Compile-time assertions
must check size, alignment and all offsets. User address pointers need not be
aligned: validate the range and copy bytes into an aligned kernel value.

| Offset | Field | Type | Size | Input/output representation |
| --- | --- | --- | --- | --- |
| 0 | family | uint16_t | 2 | host-order AF_INET |
| 2 | port | uint16_t | 2 | network-order port |
| 4 | address | uint32_t | 4 | network-order IPv4 |
| 8 | reserved | uint8_t[8] | 8 | all zero |

BIND/SENDTO require address size exactly 16. Received source output always
contains family=AF_INET, actual sender port/IP, and zero reserved bytes.
The source-size object is uint32_t, size/alignment 4, host-order in/out capacity.
It too may be unaligned; use byte copies. No other user-visible structure.

## 3. Datagram and binding semantics

BIND accepts address=0 or configured local IPv4. Port=0 allocates an ephemeral
port from 49152–65535 with bounded scanning. Explicit ports 1–65535 need no
privilege check. Ports are exclusive across wildcard/specific-local binds;
rebinding an already-bound socket or a conflicting port returns EEXIST.
No SO_REUSEADDR, connected sockets or local self-delivery.

SENDTO auto-binds an unbound socket. Destination port must be nonzero and IP
must satisfy existing supported-unicast/route policy: reject zero-net,
loopback, multicast, broadcast, subnet network/broadcast and the local IP.
An off-link destination uses configured gateway ARP while retaining final IP
destination. A valid destination with no usable route returns EIO.
An implicit bind can persist after a subsequent transport failure; malformed
arguments or bad user ranges must not bind, queue or transmit anything.

Maximum payload=min(1472, interface MTU−28), using checked subtraction.
Exceeding that payload limit returns EINVAL; an unusable MTU itself returns EIO.
SENDTO rejects oversize before copying or publication; no partial datagrams.
Zero payload is valid; null data is permitted only with zero length.
Return length means driver submission succeeded, not remote receipt.
Cold ARP is bounded to three attempts over three seconds; queue/active-send
capacity failure is EAGAIN. SENDTO does not support nonblocking flags initially.

RECVFROM capacity is bounded to 1472. It takes one datagram, copies at most
capacity bytes and discards its remainder. Capacity=0 still consumes a datagram.
Return 0 is a valid zero-length result, not stream EOF. No MSG_TRUNC/PEEK.
Empty MSG_DONTWAIT returns EAGAIN; flags=0 waits up to five seconds from
operation reservation using an absolute BSP deadline and returns EAGAIN on
timeout. Wakes do not restart this deadline. Receive on an unbound socket
auto-binds before waiting; this binding remains after timeout/interruption.

R8/R9 must both be null or both nonnull. If nonnull, validate the source-size
object for writing before reading its capacity; require capacity >=16, validate
exactly 16 bytes at source, and on success write size=16. Larger capacity is
accepted but does not authorize writing more than 16 bytes. Failure leaves
source, source-size and payload outputs unchanged. No datagram is consumed
on output validation failure.

Reject overlap among the writable payload range [data,data+capacity), the
16-byte source range and four-byte source-size range. Empty payload ranges
do not overlap. Check range overflow before overlap calculations. SENDTO
input payload/destination overlap is allowed because both are copied before
publication. Revalidate receive outputs after waking, before dequeue/copy;
socket state never stores user pointers. Reservation and copies must preserve
the no-lock-across-user-copy rule through exclusive, generation-checked storage.

## 4. Deterministic validation and error precedence

First failing stage wins. Each stage completes before the next. No externally
visible reservation, binding, enqueue or transmit occurs before static
argument and pointer validation finishes. Unknown syscall numbers retain ENOSYS.

| Call | Ordered stages |
| --- | --- |
| SOCKET | (1) BSP context; (2) family/type/protocol; (3) interface/worker/tick availability; (4) free process fd; (5) global socket slot; (6) node/file allocation and publication |
| BIND | (1) BSP context; (2) socket fd/type; (3) exact address size; (4) readable address range/copy; (5) family/reserved/local-address fields; (6) interface availability; (7) already-bound/conflicting port; (8) ephemeral allocation if requested |
| SENDTO | (1) BSP context; (2) socket fd/type; (3) flags, length <=1472, exact address size; (4) readable destination range/copy; (5) readable payload range/copy; (6) destination family/reserved/port/unicast fields; (7) interface/MTU/route availability; (8) active-send/capacity; (9) implicit ephemeral binding; (10) reserve/publish and wait for transport result |
| RECVFROM | (1) BSP context; (2) socket fd/type; (3) flags, capacity <=1472, paired source pointers; (4) writable payload range; (5) writable source-size range/read capacity; (6) capacity >=16 and writable source range; (7) output overlap; (8) interface availability; (9) active-receive capacity; (10) implicit ephemeral binding; (11) reserve, take ready data or wait/return EAGAIN |

For zero payload size, skip payload range access/validation; no dereference is
allowed. For recvfrom without source output, skip stages 5–6 and corresponding
overlap comparisons. Nonzero range wrap/inaccessibility returns EFAULT; numeric
length above the ABI cap returns EINVAL before pointer checks. Invalid address
family in BIND/SENDTO returns EINVAL, unlike unsupported SOCKET family.

After sleep: honor existing signal interruption first (EINTR); reject a stale
operation/generation (EINTR); revalidate receive output ranges (EFAULT), then
observe latched interface failure (EIO), then operation result/ready data,
then deadline expiration (EAGAIN for receive). Data already queued when a
receive is reserved is immediately eligible; worker expiration of an empty
receive is final for that token. Later packets remain queued for another call.
Transport completion is final when worker publishes it; do not overwrite a
completed send with later link loss. No partial outputs on an error return.

| Macro | Signed value | Mapping |
| --- | --- | --- |
| SYSCALL_EINVAL | −1 | invalid scalar/length/flags/fields/address or output overlap |
| SYSCALL_EFAULT | −2 | inaccessible/wrapped user range, read-only output |
| SYSCALL_EBADF | −3 | fd absent/out of range or wrong node type |
| SYSCALL_EMFILE | −6 | no process fd |
| SYSCALL_EIO | −9 | unavailable interface/worker/ticks, unsupported route, observed link/fatal failure, ARP exhaustion, TX failure |
| SYSCALL_ENOMEM | −10 | node/file allocation failure |
| SYSCALL_ENOSPC | −13 | socket pool or ephemeral range exhausted |
| SYSCALL_EOPNOTSUPP | −14 | unsupported SOCKET family/type/protocol or caller CPU context |
| SYSCALL_EEXIST | −15 | bind conflict or rebind |
| SYSCALL_EAGAIN | −21 | command/operation busy, empty nonblocking RX or five-second RX timeout |
| SYSCALL_EINTR | −22 | existing signal interruption or stale/expired operation lease |

Reuse project errno values, not Linux numbers. Creation/auto-bind failures must
unwind reservations and allocations exactly once; failed explicit bind leaves
the socket unbound. No new errno, syscall-entry layout or transition change.

## 5. Lifetime and implementation boundary

16 static slots, each with four RX datagrams of up to 1472 bytes, at most one
send and one receive operation. Pool sizes are Phase 5 implementation limits,
not fields embedded in the address structure. Full RX drops newest; never
retains NIC DMA buffers. Shared dup/spawn descriptors reference the same
socket. One concurrent operation of each direction per socket; competitors
return EAGAIN. Existing CLOEXEC and final file-reference cleanup apply.

Final close invalidates generation and cancels work; late worker results and
stale waiters cannot target a reused slot. Finite operation leases reclaim
abandoned reservations (send five seconds, receive seven seconds including
collection margin), without expiring an otherwise live socket or RX queue.
STOP/CONT beyond lease may resume with EINTR. Hard-kill descriptor cleanup
uses existing exit/reaper behavior; no new owner-exit or signal hook.

Worker alone owns routing/ARP/protocol scratch. Publish atomic readiness before
waking through existing primitives, with all rank-1 locks dropped. No scheduler,
lock-rank, sched_wait_until-signature or timer-hook change. If tracing actual
final-close/signal callers reveals an incompatibility, stop and report before
implementing an alternative. Physical Phase 5b acceptance remains separate.
