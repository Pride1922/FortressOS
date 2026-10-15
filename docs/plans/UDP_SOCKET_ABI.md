# UDP socket ABI — Phase 5 pre-coding contract

2026-10-01. Written before coding; now implemented in Phase 5a (syscalls 38–41).
Original scope: BSP-only callers, five-second blocking receive, 16 global
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
Network SMP Phase 2B removes the UDP CPU/affinity guards. BSP and AP callers
use the same registered CPU-0 owner for create, bind, send, receive and detach.
Production spawn pinning remains; genuine AP launch is fixture-only. TCP guards
remain. Generic close/dup/fd cleanup retains its existing contract.

Owner requests retain checked file references and fail-closed endpoint/request
identities. Eight bounded payload slabs support caller copy pins. A one-second
reservation lease restores unconsumed datagrams; it cannot recycle pinned bytes
or restart the original five-second receive deadline. Owner commit consumes the
whole datagram once; cancellation after copy commit returns the successful count.
SEND interruption resolves owner acknowledgement and preserves already accepted
datagrams. Nonblocking internal admission/slab saturation returns EAGAIN without
starting an operation; blocking internal saturation sleeps. Final close publishes
detach and endpoint reuse waits for the owner's acknowledgement. See
[Phase 2B audit and gates](../roadmap/network-smp-phase2b.md).

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
port from 49152–65535 with bounded scanning. The later permissions gate requires
CAP_NET_BIND for explicit ports 1–1023, checked in the caller before admission.
Ports are exclusive across wildcard/specific-local binds;
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
transport publication. Revalidate receive outputs after waking, before dequeue/copy;
socket state never stores user pointers. Reservation and copies must preserve
the no-lock-across-user-copy rule through exclusive, generation-checked storage.

## 4. Deterministic validation and error precedence

First failing stage wins. Each stage completes before the next. No externally
visible reservation, binding, enqueue or transmit occurs before static
argument and pointer validation finishes. Unknown syscall numbers retain ENOSYS.

| Call | Ordered stages |
| --- | --- |
| SOCKET | family/type/protocol; interface/tick availability; free process fd; retained owner admission; owner capacity check before reporting caller preallocation failure; caller fd installation |
| BIND | socket fd/type; exact address size; readable range/copy; family/reserved/coherent local address; low-port capability; retained admission; owner-time local address and availability; conflict/ephemeral allocation |
| SENDTO | socket fd/type; flags, length <=1472, exact address size; readable destination and payload ranges; destination fields/coherent unicast check; retained admission; owner availability and FIFO slab credit; caller revalidation/copy; owner MTU/route, direction capacity, autobind and transport publication |
| RECVFROM | socket fd/type; flags, capacity <=1472, paired source pointers; writable payload and source-size ranges, capacity >=16, source range and overlap; retained admission; owner availability; nonblocking slab credit before endpoint mutation; direction capacity/autobind; finite receive and staged copy/commit |

For zero payload size, skip payload range access/validation; no dereference is
allowed. Without source output, skip source/source-size checks and corresponding
overlap comparisons. Nonzero range wrap/inaccessibility returns EFAULT; numeric
length above the ABI cap returns EINVAL before pointer checks. Invalid address
family in BIND/SENDTO returns EINVAL, unlike unsupported SOCKET family.

After sleep and before copy: honor signal interruption, revalidate ranges and
pin the current immutable staged result. A successful receive copy publishes
commit before any later signal check; owner consumption and its nonnegative
result win subsequent cancellation. Interrupted SEND resolves owner cancellation
acknowledgement and preserves any accepted nonnegative count. Before acceptance,
interruption returns EINTR. Unavailable service returns EIO ahead of internal
slab saturation, including MSG_DONTWAIT. Data already queued when a
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
| SYSCALL_EOPNOTSUPP | −14 | unsupported SOCKET family/type/protocol; TCP CPU guard remains |
| SYSCALL_EEXIST | −15 | bind conflict or rebind |
| SYSCALL_EAGAIN | −21 | command/operation busy, empty nonblocking RX or five-second RX timeout |
| SYSCALL_EINTR | −22 | zero-progress signal interruption or invalidated operation |

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

Final UDP close publishes detach; the owner cancels work and acknowledges before
endpoint reuse. Late results cannot target a replacement identity. Retained
operation references replace the legacy five/seven-second abandoned-caller
leases. A one-second copy lease restores unconsumed data without reusing pinned
bytes; the original five-second receive deadline remains. STOP/CONT past that
deadline returns EAGAIN. Hard-kill uses Phase 1 abandonment before descriptor
and address-space destruction, with idempotent reaper fallback.

Worker alone owns routing/ARP/protocol scratch. Publish atomic readiness before
waking with all Rank-1 locks dropped. Ordinary sched_wait_until semantics and
signature, lock ranks and timer hooks remain; an acknowledgement-only entry
shares the scheduler wait core while deferring signals for a bounded owner
commit/cancel decision. It never waits for peer readiness. Physical Phase 5b
history remains separate from acceptance of this SMP migration.
