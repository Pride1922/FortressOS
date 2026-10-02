# NET-2 Step 4: listener, accept and finite server

Started 2026-10-02 after Step 3 commit ce37bd3. This is the pre-coding
implementation contract, retained below as the design record. Step 4 is now
implemented and host/Ring 3 verified; see [evidence](../roadmap/net2-step4.md).

## First gate: existing client regression fence

Before any listener implementation, run:

```
make test-net-tcp-socket-host test-net-tcp-tcb-host
```

First gate executed after the Step 3 commit on 2026-10-02: both ASan/UBSan
suites PASS, including all three client regression groups. No listener code
has been added; the next gate is the concrete ACCEPT ABI/lifetime argument.

The interrupted CONNECT/SYN-ACK race, three SEND short-count distinctions and
same-timestamp tick regression were implemented and passed in Step 3, rather
than deferred. Rerun them first in Step 4 and retain them throughout its tests.
The CONNECT fixture proves SYN/ACK delivered after cancellation publication
but before worker detach uses FIN without RST. SEND tests prove exact space
acceptance, pre-existing interruption accepting zero bytes, and post-acceptance
publication preserving the count and exact peer bytes. Signals/scheduling are
host adapters; this does not prove real signal-handler delivery or IRQ timing.

## Bounded listener model

Retain the eight global transport blocks, sixteen common handles and existing
TIME_WAIT reservations. A listener uses its existing block; each pending child
requires another real block and TIME_WAIT reservation before SYN/ACK is sent.
No separate unbounded SYN table or SYN cookies. Global resource exhaustion can
limit capacity below the requested backlog.

Choose backlog 1..4 inclusive, reject zero, negative/full-width invalid or >4
with EINVAL. One four-entry static queue per listener counts both SYN_RCVD and
established-but-unaccepted children. Overflow/resource exhaustion silently drops
a new SYN without allocation or evicting an existing child. Duplicate SYN for
an existing tuple uses its existing connection and does not add a queue entry.
Only established children are eligible for ACCEPT; half-open entries must not
block completed children behind them. SYN_RCVD uses the existing absolute
30-second handshake deadline; reap expired/reset children and release their
queue membership, block and reserved TIME_WAIT state after bounded teardown.

LISTEN requires an explicitly bound TCP endpoint and transitions only an unused
client endpoint; repeat LISTEN is EINVAL for this phase. Do not apply the active
CONNECT-only quiet-time gate to LISTEN. Preserve TCP/UDP namespace separation.

## ACCEPT ABI and ownership gate

Use reserved SYS_LISTEN=44: RDI listener fd, RSI backlog. Use SYS_ACCEPT=45:
RDI listener fd, RSI optional peer sockaddr output, RDX optional uint32_t
in/out capacity, R10 flags (0 or NET_SOCK_CLOEXEC). Both output pointers must
be present together or absent together. Capacity must be at least 16; success
writes exactly the existing 16-byte sockaddr and capacity=16. Reject overlapping
outputs and unknown flags. No nonblocking ACCEPT is introduced in this step.

Validation order: BSP eligibility, fd/stream/listener state, scalar/pointer-pair
and flag checks, writable output ranges and capacity/overlap, online state,
interruption and generation identity, then child/resource availability.
Revalidate outputs after every sleep before touching a child. Freeze exact
error precedence and layouts in TCP_SOCKET_ABI.md before coding the syscall.

Prefer no dequeue/reservation until success: snapshot listener identity/event,
check for a completed child, then sleep on the static channel with no child,
file allocation, user-pointer storage, lease or token held. On readiness, check
interruption, revalidate outputs, allocate common-handle/file/node/fd resources
without holding rank-1 locks, and commit transfer in the BSP IF-clear continuation
without sleeping. Accepted endpoint adopts the child's existing transport block;
it must not allocate a replacement block or require a ninth transport slot.

If validation/allocation/interruption fails before transfer, leave the child
queued exactly once and roll back only new handle/file/fd allocations. If an
implementation temporarily removes a child, it must restore queue ownership on
every failure; avoiding that removal is preferred. Positive accepted fd wins
over later signal publication. Do not copy user output under a subsystem lock.

Extend TCP_WAIT_LIFECYCLE_PROOF.md before implementation with the exact transfer
linearization point, fd publication order, shared listener lifetime, concurrent
acceptors, AP close exclusion, failure rollback and STOP/CONT/KILL behavior.
The above is a design argument to verify against actual APIs, not a completed
proof. Stop for discussion if exclusion/lifetime cannot be established with
existing scheduler, signal and lock contracts.

The 2026-10-02 proof extension and listener ABI now specify these decisions:
fd_alloc inserts the staged file before queue removal under uninterrupted BSP
IF-clear exclusion; allocation failure leaves the child queued. Wake policy is
sched_wake_all with recheck/one-child transfer, without a fairness guarantee.
Another process closing a shared descriptor cannot remove the acceptor's own
reference; defensive listener generation invalidation gives EBADF, caught
interruption gives EINTR. LISTEN on an unbound endpoint gives EINVAL. These
were implementation obligations and are now wired and tested in Step 4.
The seven-property audit in TCP_WAIT_LIFECYCLE_PROOF.md satisfies the pre-coding
design gate. It adds mandatory revalidation after process_signal_interrupt,
which can itself park for STOP/CONT; stage nothing before its final call.
The existing stream constructor must not be reused for adoption because it
allocates another transport block. A dedicated staged-handle path is required.

## Listener exit and worker invariants

Listener final close invalidates its generation and requests worker cleanup of
all unaccepted children, including half-opens. Accepted children have independent
endpoint/channel/file ownership and no remaining listener dependency. Shared
listener fds keep it alive until final close. No reaper performs protocol work.

Preserve uninterrupted IF-clear prepare/submission/commit transactions and the
rejected-commit trap: tick/input/consume must not invalidate an in-flight action.
Pool maintenance runs after transactions finish. The RX cap remains per worker
pass, not timer tick; each segment restores incoming IRQ state. Keep eight TX
actions plus one TIME_WAIT ACK per pass and finite static queue scans.

## Implementation and acceptance sequence

1. Pass the client fence, freeze listener ABI and review the ACCEPT wait argument.
2. Add bounded ingress/half-open/listener lifecycle and queue tests.
3. Add fd adoption and ACCEPT dispatch with complete rollback tests.
4. Add a finite numeric-port server fixture: accept one child, close listener,
   receive/echo at most 65536 bytes using bounded chunks, SHUT_WR and close.
   Its independent peer must drain responses concurrently while sending; the
   serial Step 3 request-then-response fixture is not suitable for streaming echo.
5. Run host sanitizers and real Ring 3 QEMU server/independent-peer captures.

Required cases: backlog saturation/duplicate SYN/global pool exhaustion;
half-open deadline/recovery; fd/heap/common-handle failure without child loss;
invalid/read-only/cross-page/overlapping output; caught interruption before
transfer; STOP/CONT and KILL while waiting; competing acceptors without double
delivery; listener exit with queued children; accepted child still usable after
listener close; finite binary server ordering/EOF; unchanged client fence and
UDP/ICMP/driver regressions. Record exact evidence and distinguish host adapters,
Ring 3 execution, capture audits and physical acceptance.
