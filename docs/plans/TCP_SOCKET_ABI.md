# TCP socket contract — NET-2 design baseline

2026-10-01. Intended semantics, not an implemented/frozen syscall ABI. Step 1
does not add syscall numbers, change socket_abi.h or enable SOCK_STREAM.
Freeze this document with lifecycle proof and full error precedence before
step 3. Existing UDP 38–41 and SYS_NETCTL=42 retain their current semantics.

## Address and calls

Reuse the 16-byte, align-4 net_sockaddr_in_t: family uint16 host-order at 0,
port uint16 network-order at 2, address uint32 network-order at 4,
reserved uint8[8] zero at 8. Copy unaligned input/output via aligned locals.
No new pointer-bearing address structure. SOCK_STREAM=1, protocol 0 or 6;
SOCK_CLOEXEC maps to the existing fd flag. UDP protocol acceptance is unchanged.

Proposed register map (RAX signed result; RDI/RSI/RDX/R10/R8/R9 arguments):

| Proposed number | Call | Arguments | Success |
| --- | --- | --- | --- |
| 43 | CONNECT | fd, destination, 16 | 0 after handshake |
| 44 | LISTEN | fd, backlog | 0 |
| 45 | ACCEPT | fd, source-or-null, source-size-or-null | new fd |
| 46 | SEND | fd, data, length, flags | bytes accepted |
| 47 | RECV | fd, data, capacity, flags | copied bytes or EOF=0 |
| 48 | SHUTDOWN | fd, SHUT_WR=1 | 0 |

All callers remain CPU0/affinity0. Start with flags 0 or MSG_DONTWAIT=0x40
on SEND/RECV; unknown bits rejected. CONNECT/ACCEPT blocking initially; nonblocking
variants would require separately specified flags/readiness, not magic EAGAIN
success. SHUT_RD/SHUT_RDWR unsupported initially. Bound each copy to 16384
bytes (existing write bound), allow positive short transfers. read/write on
stream sockets must use the same implementation and semantics with flags zero.

## Observable stream behavior

Send accepts bytes into connection-owned TX storage, not into a descriptor owned
by the NIC. A positive result never promises peer delivery. Accepted bytes stay
retained until ACK or connection failure. Space exhaustion blocks, or EAGAIN in
nonblocking mode. Interruption after acceptance returns the positive byte count;
before acceptance returns EINTR. No duplicate acceptance on syscall retry.

Receive returns available ordered bytes, not a message. Never wait for the full
requested capacity. Invalid outputs do not consume bytes. Peer FIN returns zero
only after preceding buffered bytes drain. RST/error read ordering must be frozen
before step 3; do not arbitrarily present reset as clean EOF. A zero-length valid
send/receive is a no-op, not an EOF test. Source outputs on accept are optional
as a pair and must not overlap; invalid output must not dequeue a child.

Blocking receive and accept have no application timeout. Internal reservation
expiry is not EAGAIN, connection expiry or a five-second data discard. It releases
the reservation, wakes the stale waiter and lets that continuation re-register
with generation validation. The implementation must prove this through STOP,
KILL, shared fds and fd reuse before enabling the calls. No user/TCB pointers in
protocol storage. Existing wait primitives and IRQ-excluded copy/commit remain.

Shutdown write stops future acceptance, schedules FIN after accepted bytes, and
leaves receive available. Writes after shutdown return EPIPE with existing signal
publication discipline; SEND's future no-signal flag, if offered, must be explicit.
Last fd close is nonblocking application detachment; shared dup/inherited fds
keep the endpoint live. Worker performs bounded orphan teardown independently.
Closing the listener does not close already accepted sockets. Connections in
TIME_WAIT retain their tuple outside the file descriptor/socket-handle lifetime.

## Errors to freeze before integration

Preserve existing EINVAL=-1, EFAULT=-2, EBADF=-3, EMFILE=-6, EIO=-9,
ENOMEM=-10, ENOSPC=-13, EOPNOTSUPP=-14, EPIPE=-20, EAGAIN=-21, EINTR=-22.
Propose new project errors ECONNREFUSED=-26, ECONNRESET=-27, ENOTCONN=-28,
EADDRINUSE=-29, ETIMEDOUT=-30, EISCONN=-31; verify these remain free before
adding them. No Linux numeric errno assumptions. UDP's current error mapping
must not change when introducing TCP-specific errors.

Intended validation order: BSP eligibility; fd/type; full-width scalar bounds;
pointer ranges; copied address/flags fields; connection/resource state. SOCKET
checks family/type/protocol before resource capacity. Never dequeue ACCEPT or
RECV before output validation; revalidate after blocking. CONNECT needs an explicit
deadline and interruption policy; interrupted pending handshake must not become
an invisible connection. Listener/backlog allocation/accept rollback, repeated
shutdown, competing shared-fd operations and user-copy error versus pending-reset
precedence need concrete fixtures before ABI freeze.
