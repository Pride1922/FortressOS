# TCP socket contract — NET-2 steps 3–4 stream ABI

2026-10-02. Client ABI frozen for step 3: CONNECT/SEND/RECV/SHUTDOWN and
read/write on streams. Step 4 now implements LISTEN/ACCEPT and bounded passive
children; host/Ring 3 evidence is recorded in net2-step4.md. UDP 38–41 and SYS_NETCTL=42
retain their semantics. No pointer-bearing structure or scheduler ABI change.

## Address and calls

Reuse the 16-byte, align-4 net_sockaddr_in_t: family uint16 host-order at 0,
port uint16 network-order at 2, address uint32 network-order at 4,
reserved uint8[8] zero at 8. Copy unaligned input/output via aligned locals.
No new pointer-bearing address structure. SOCK_STREAM=1, protocol 0 or 6;
SOCK_CLOEXEC maps to the existing fd flag. UDP protocol acceptance is unchanged.

Register map (RAX signed result; RDI/RSI/RDX/R10/R8/R9 arguments):

| Number | Call | Arguments | Success |
| --- | --- | --- | --- |
| 43 | CONNECT | fd, destination, 16 | 0 after handshake |
| 44 | LISTEN | fd, backlog | 0 |
| 45 | ACCEPT | fd, source-or-null, source-size-or-null, flags | new fd |
| 46 | SEND | fd, data, length, flags | bytes accepted |
| 47 | RECV | fd, data, capacity, flags | copied bytes or EOF=0 |
| 48 | SHUTDOWN | fd, SHUT_WR=1 | 0 |

All callers remain CPU0/affinity0. Start with flags 0 or MSG_DONTWAIT=0x40
on SEND/RECV; unknown bits rejected. CONNECT/ACCEPT blocking initially; nonblocking
variants would require separately specified flags/readiness, not magic EAGAIN
success. SHUT_RD/SHUT_RDWR unsupported initially. Bound each copy to 16384
bytes (existing write bound), allow positive short transfers. read/write on
stream sockets must use the same implementation and semantics with flags zero.

## Step 4 listener ABI (implemented)

LISTEN requires an explicitly bound, unused TCP endpoint. Unbound endpoint,
already-started endpoint, repeated LISTEN or backlog outside 1..4 gives EINVAL.
Wrong fd/type gives EBADF; unavailable network gives EIO. Check BSP eligibility,
fd/type, full-width backlog, online state, then bound/endpoint state. No implicit
bind or backlog clamping. SYS_LISTEN/SYS_ACCEPT now dispatch to real handlers.

ACCEPT flags in R10 are 0 or NET_SOCK_CLOEXEC only; no nonblocking flag.
Optional peer output and uint32_t capacity pointers must appear as a pair;
mismatch or overlapping outputs gives EINVAL. Validate both writable ranges,
copy capacity (must be >=16, else EINVAL), then validate output overlap. Null
pair skips output. Success writes the existing zero-reserved sockaddr and
capacity=16. Revalidate ranges and capacity after each sleep. Use aligned local
copies for unaligned outputs. No output copy or fd insertion on failed accept.

ACCEPT error order: BSP eligibility; fd/stream (EBADF) and listener state
(EINVAL); flags/pointer pair (EINVAL); writable ranges (EFAULT), capacity and
overlap (EINVAL); network (EIO); caught interruption (EINTR); saved listener
identity invalidation (EBADF); completed-child and resource checks. Allocation
failure is ENOMEM, common-handle exhaustion ENOSPC, fd exhaustion EMFILE; each
leaves a completed child queued exactly once. Blocking absence of a completed
child sleeps rather than returning EAGAIN. A successful transfer returns its
fd even if a signal is subsequently published.
Because the interruption check may park for STOP and resume after CONT, repeat
output range/capacity, online and identity validation after that check, before
staging resources. No interruption check or sleep occurs during staged transfer.

Use sched_wake_all on listener events, after publishing and releasing locks.
All acceptors recheck; BSP IF-clear transfer lets only one acquire each child.
Other acceptors retry/sleep with a new event snapshot; no FIFO/fairness promise.
Closing another inherited/duplicated fd leaves the blocked acceptor's reference
live, so does not close the listener or produce an error. If saved listener
identity is invalidated, return EBADF (caught interruption wins if both occur).
KILL follows existing teardown and does not promise a syscall return.

Fd insertion precedes queue removal within one non-sleeping BSP IF-clear
transaction, after resource/output preparation. The new file/node are fully
initialized before fd_alloc; no other continuation can observe the provisional
endpoint. Finish child adoption/queue removal, set CLOEXEC and copy output
before IRQ state may be restored or user execution resumes. The detailed
ownership argument is in TCP_WAIT_LIFECYCLE_PROOF.md; runtime evidence and its
limits are in [net2-step4.md](../roadmap/net2-step4.md).

Backlog 1..4 shares one static queue for half-open and completed children.
New SYN overflow or global connection/TIME_WAIT shortage silently drops the SYN;
duplicates use their existing full tuple. Half-open deadline is 30 seconds.
ACCEPT skips half-opens, can adopt ESTABLISHED/CLOSE_WAIT, and returns a child
with independent file/endpoint/channel ownership. Accepted passive connections
and their TIME_WAIT records reserve full tuples rather than exclusive port
binds: a new listener can rebind while old children remain live. A live listener
or client bind still conflicts; identical retained tuples remain excluded.

## Observable stream behavior

The userspace serial nc client, and listeners with nonterminal stdin, send stdin
before receiving. A peer that sends
a large response before consuming the whole request can deadlock the serial nc;
use small finite requests or a cooperating peer. nc has no application deadline;
this socket ABI does not promise a timeout for a stalled bidirectional exchange.
The bounded early-response regression uses a peer deadline followed by RST,
then requires an nc error and shell recovery. It is not an automatic nc timeout.
In listener mode nc checks fd 0 using existing TERM_ISATTY: terminal stdin is
skipped, followed by SHUT_WR and receive-until-EOF. Plain `nc -l port` is therefore
receive-only on a terminal. Pipes, files and /dev/null retain the serial stdin
copy/EOF/SHUT_WR flow. Client mode always consumes stdin. Terminal-query failure
is an ordinary tool error. Socket ABI and single-accept semantics are unchanged.

Send accepts bytes into connection-owned TX storage, not into a descriptor owned
by the NIC. A positive result never promises peer delivery. Accepted bytes stay
retained until ACK or connection failure. Space exhaustion blocks, or EAGAIN in
nonblocking mode. Interruption after acceptance returns the positive byte count;
before acceptance returns EINTR. No duplicate acceptance on syscall retry.

Receive returns available ordered bytes, not a message. Never wait for the full
requested capacity. Invalid outputs do not consume bytes. Peer FIN returns zero
only after preceding buffered bytes drain. Buffered ordered bytes precede reset
error as specified below; never present reset as clean EOF. A zero-length valid
send/receive is a no-op, not an EOF test. Source outputs on accept are optional
as a pair and must not overlap; invalid output must not dequeue a child.

Blocking receive has no application timeout. TCP holds no reservation while
sleeping: a continuation captures endpoint generation/event, checks readiness,
waits with the existing atomic-only predicate, then revalidates/retries. Only
immediate RX copy/consume or TX acceptance owns bytes. STOP cannot retain an
expired reservation; accepted TX is independent of syscall/continuation life.
No user/TCB pointer enters protocol storage. ACCEPT's indefinite contract still
uses the reviewed step 4 extension and its host/Ring 3 gates. Existing wait
primitives and IF-clear copy/commit remain.

Shutdown write stops future acceptance, schedules FIN after accepted bytes, and
leaves receive available. Writes after shutdown return EPIPE with existing signal
publication discipline; SEND's future no-signal flag, if offered, must be explicit.
Last fd close is nonblocking application detachment; shared dup/inherited fds
keep the endpoint live. Worker performs bounded orphan teardown independently.
Chosen final-close policy is graceful with local unread-data discard: retain
accepted TX until ACK/failure and send FIN afterward; discard/drain orphan RX
without bypassing sequence validation or closing its window indefinitely.
Existing orphan deadlines can abort stalled teardown. SHUT_WR alone preserves
receive; final close does not promise that unread data reached the application.
The worker implements this discard/drain after pure detach. Buffered ordered
bytes precede ECONNRESET while an
application remains attached; an empty reset stream returns ECONNRESET, not EOF.
Closing the listener does not close already accepted sockets. Connections in
TIME_WAIT retain their tuple outside the file descriptor/socket-handle lifetime.

The named pre-coding gate is [TCP_WAIT_LIFECYCLE_PROOF.md](TCP_WAIT_LIFECYCLE_PROOF.md),
reviewed for the reservation-free client design; runtime evidence is recorded
in the step 3 report. No explicit receive timeout fallback was needed. CONNECT
before 120 seconds of BSP uptime returns EAGAIN without starting a handshake;
the boot diagnostic states this reboot quiet period. After it, CONNECT has an
absolute 30-second handshake deadline (ETIMEDOUT). Interrupted CONNECT cancels
and detaches even if the handshake raced to completion; close/recreate that
endpoint before trying again. See [NET2_PLAN.md](NET2_PLAN.md) for ISN limits.
Cancellation processed in SYN_SENT/SYN_RCVD requests a best-effort RST if SYN
was submitted; if SYN/ACK wins before deferred detach, ESTABLISHED instead
uses graceful orphan FIN teardown, with existing timeout/reset fallback.

## Client errors and precedence

Preserve existing EINVAL=-1, EFAULT=-2, EBADF=-3, EMFILE=-6, EIO=-9,
ENOMEM=-10, ENOSPC=-13, EOPNOTSUPP=-14, EPIPE=-20, EAGAIN=-21, EINTR=-22.
New project errors ECONNREFUSED=-26, ECONNRESET=-27, ENOTCONN=-28,
EADDRINUSE=-29, ETIMEDOUT=-30, EISCONN=-31 were checked free and added.
No Linux numeric errno assumptions. UDP's current error mapping
must not change when introducing TCP-specific errors.

Client validation order: BSP eligibility; fd/type (EBADF); full-width scalar bounds;
pointer ranges; copied address/flags fields; connection/resource state. SOCKET
checks family/type/protocol before resource capacity. Wrong protocol/type returns
EOPNOTSUPP; no worker/NIC or unavailable frequency returns EIO; fd capacity
returns EMFILE, common-handle/connection/TIME_WAIT capacity ENOSPC, heap failure
ENOMEM. Closed/deferred slots are reclaimed by worker, so allocation failure can
persist until its next pass. TCP BIND conflicts return EADDRINUSE; repeated bind
or bind after CONNECT returns EINVAL. TCP and UDP port namespaces are separate.

CONNECT size must be exactly 16 (EINVAL), then address readability (EFAULT),
family/reserved/nonzero port/unicast/route (EINVAL), online state (EIO), quiet
period (EAGAIN), endpoint state (EISCONN for any already-started attempt), then
tuple/resource state. A valid SYN refusal is ECONNREFUSED. Other failed endpoints
are recreated, not silently retried with a retained old tuple.

SEND/RECV and stream read/write reject full-width length >16384 or unknown flags
with EINVAL, then validate the entire requested range (EFAULT). Valid zero length
returns 0 without connection/EOF testing. Nonzero operations check interruption,
identity and revalidate ranges after every sleep. Invalid memory wins over reset
and never consumes RX. A disconnected or pending-open endpoint gives ENOTCONN.
Buffer/space absence gives EAGAIN only for MSG_DONTWAIT; blocking mode sleeps.
SEND after SHUT_WR gives EPIPE and publishes SIGPIPE to the writer using existing
signal behavior; no MSG_NOSIGNAL exists. A positive short result wins over future
interrupt/error. SHUTDOWN accepts SHUT_WR=1 only, rejects other values EINVAL,
returns ENOTCONN before connection and succeeds idempotently once requested.
Online failure is EIO; received reset is ECONNRESET after buffered ordered bytes;
transport retry exhaustion is ETIMEDOUT. Peer FIN produces 0 after data.

Stream read/write dispatches into this same implementation, preserving syscall
return/error values. UDP read/write remains unsupported. No TCP socket owns a
NIC descriptor or retained DMA pointer. Listener/backlog and ACCEPT output/fd
rollback and child ownership remain step 4 freeze obligations.
