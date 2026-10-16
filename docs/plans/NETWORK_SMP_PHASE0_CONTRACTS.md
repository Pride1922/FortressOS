# Network SMP Phase 0 decisions

Phase 2B acceptance qualification (2026-10-15): COMPLETE.
[UDP/DNS audit and gates](../roadmap/network-smp-phase2b.md). Retained generic
records now require explicit unlocked typed cleanup before recycling. UDP adds
checked file references, endpoint detach acknowledgement and eight 1472-byte
payload slabs (TCP must later share/extend the eight payload credits). Receive
copy commit wins later cancellation; SEND cancellation resolves accepted wire
submission before returning. The original finite receive deadline survives
reservation expiry. Acknowledgement-only scheduler waits reuse the ordinary
enrollment/context-switch core while deferring signals for the bounded owner
decision. TCP (Phase 2C/2D) and production spawn policy (Phase 3) remain separate.

UDP configuration-order qualification: “configuration at claim” means the
owner turn that starts transport after caller payload publication. An earlier
slab grant is preparation, not an accepted datagram or a reserved config epoch.
IFSET may pass a sender that has not published its copied payload; otherwise
a stopped copier could indefinitely block control progress. The owner builds
the datagram with the configuration current at protocol acceptance. Specific
bind addresses are rechecked at their owner transaction; existing TCP tuples
remain unchanged. This refines the original single-claim sketch rather than
claiming submission order freezes configuration across a copy handshake.

Phase 1 implementation qualification (2026-10-14): see
[mechanism/evidence](../roadmap/network-smp-phase1.md). The generic layer uses
bounded scans of 64 static records instead of a separate runnable-token ring;
QUEUED records consume at most 32 credits (at most 24 data). Tickets are assigned
on first offer, and the owner grants offered waiters without requiring the
caller to run again. Unoffered PREPARED records cannot obstruct FIFO service.
Claims alternate eligible classes while preserving FIFO within each class.
Pending records release runnable credits and do not themselves keep the owner
awake: service events explicitly notify it. Cancellation of PENDING/CLAIMED
work requires a typed owner step before terminal acknowledgement; generic
reclamation cannot bypass subsystem cleanup. Only fixed-size kernel scalar
assignments occur under the handoff lock, never user or payload copies.
The payload, endpoint/file-reference and reservation adapters remain Phase 2.

2026-10-13. Design specification only; no production authorization to remove
guards or pinning. Read with [the campaign plan](NETWORK_SMP_PLAN.md).
Phase 1 must implement and prove the prerequisites below before integration.

## Audit of the current implementation

| Trace (symbols are authoritative anchors) | Finding / required change at the later gate |
| --- | --- |
| `syscall_entry_stub -> syscall_dispatch` | SWAPGS and kernel RSP switch precede frame construction; IF is masked. Caller CR3 and TCB belong to the calling process. Preserve assembly unchanged. |
| `syscall_dispatch -> net_socket_syscall / net_tcp_syscall` | CPU and affinity guards precede argument/fd checks. `SYS_READ` and `SYS_WRITE` explicitly redirect TCP files to `net_tcp_syscall`; UDP VFS callbacks reject stream I/O. Include these indirect paths. |
| `sys_net_ifset -> syscall_actor -> process_record_creds -> permission_capability` | Authorization is caller-specific, before size/range validation; capture credentials by value outside all network locks. IFGET/IFSET are currently BSP serialized. |
| `fd_get` | Borrowed pointer, no reference acquisition. Safe within the present single-process continuation; insufficient for asynchronous owner retention. |
| `fd_clone_table`, `fd_dup`, `fd_dup2 -> ACQ_REL ref_count` | Inheritance and duplication share one file. 16 handles do not bound callers; up to 64 kernel tasks can compete through shared descriptors. CLOEXEC and secure-entry cleanup also release files. |
| `fd_free / fd_close_all / fd_close_cloexec -> vfs_close -> node->close -> socket_close` | Final file release invokes the socket callback once. Callback invalidates UDP operations, then publishes TCP detach, or unstages a provisional file. It runs from caller, spawn unwind, exit or reaper contexts. Never wait here. |
| `socket_close -> net_tcp_close -> net_tcp_tick` | AP close publishes under endpoint lock; BSP sweep detaches children/connections and releases protocol storage. Common slot can become free before TCP detach finishes; constructors already check endpoint detach. Preserve that reuse fence and extend it to operation references. |
| `process_exit -> fd_close_all -> thread_exit -> sched_post_switch -> sched_reap_dead` | Reaper detaches dead lists under scheduler lock, destroys CR3/stack and forgets metadata before final cleanup outside it. Abandonment must precede descriptor/space destruction and must also be idempotent in reaper cleanup. No owner may retain a TCB. |
| `process_signal_interrupt / sched_wait_until / thread_wake_for_signal` | STOP parks the continuation; CONT retries predicates; caught/terminating signals interrupt I/O. Signal checks can switch, so output validation before them is insufficient. SIGPIPE uses the original writer PID. |
| `sched_wait_until -> context_busy -> switch_context -> sched_post_switch` | Predicate is evaluated under local scheduler lock. Outgoing context remains ineligible for stealing until release publication after stack/CR3 handoff. Preserve this protocol. |
| `sched_wake_all -> all CPU blocked lists -> smp_send_resched` | Wake enqueues remotely and requests reschedule; it does not prove the outgoing save is finished. Phase 1 must force wake-before-save and enrollment races. |
| `net_request_poll -> s_work_pending -> deadline_reached -> net_worker_main` | Release hint precedes unlocked wake. Owner sleep must additionally acquire-read submission/cancel/commit work. Disable tick rescue in the inert fixture to expose lost wakes. |
| `net_tcp_syscall -> received -> net_tcp_peek -> user memcpy -> net_tcp_consume` | Global 64 KiB scratch relies on BSP IF-clear continuation. Replace with pinned kernel staging; no buffer on a 16 KiB stack. |
| `accept_socket -> accept_peek -> stage_stream -> fd_alloc -> accept_commit -> finish_adopt` | Present transaction cannot be interrupted after peek. Later owner adoption needs a reserved child and explicit caller commit/abort. Descriptor failure leaves child queued; full-pool adoption requires no ninth TCB. |

Current documentation overstates sole-worker mutation: TCP create/bind/connect,
send/consume/listen/adoption/shutdown run in BSP IF-clear syscall continuations;
NIC callbacks can be reached by these transactions. `net_set_config` runs in the
BSP caller, holding only the stack lock while invoking lockless cross-layer
setters. CPU 0 plus IF exclusion is today's serialization, not worker identity.
The desired worker-only contract is a future strengthening, not a baseline fact.
`NET_PLAN.md`'s historical “no networking” / TCP deferred sections are historical;
current headers and NET-2 evidence supersede them. `net_tcp_set_local_ip` changes
the default local IP, not existing connection tuples. Keep existing connection
behavior; do not silently reset connections. PROTECTED.md and the campaign plan
still require a 120-second quiet period, but current `net_start` announces zero
quiet time and `connect_start` has no quiet-period check. `net_tcp_init` hashes
boot entropy and records whether four hardware random samples succeeded; the
startup banner claims cryptographic assurance unconditionally. This is an
unresolved pre-existing policy/documentation discrepancy: Phase 0 preserves
code, and no later integration may silently choose a TCP security policy.
2026-10-14 reconciliation: the user explicitly authorized documenting the
existing `056a12d` implementation and zero quiet period without changing TCP
production behavior. PROTECTED.md now records conditional hardware entropy
assurance, rather than repeating the unconditional startup claim. The policy
qualification above is historical and resolved by that authorization; it is not
a security audit. Earlier quiet-time evidence remains historical.

UDP generation wraps while skipping zero today; the new request identities must
fail closed. Existing endpoint exhaustion is a separate Phase 2 audit dependency.

## Typed operations and transaction rules

No arbitrary function pointer, frame, user pointer or borrowed file/TCB appears
in an owner message. All values are copied, lengths bounded, identities checked.
Each request includes actor PID (never reused in this campaign), credential
snapshot, request token, endpoint slot/generation, operation kind and deadline.
Authorization linearizes at successful caller admission after ABI validation;
later credential changes do not retroactively revoke an admitted operation.
Death cancels caller interest. Output always revalidates in the caller CR3.

| Typed kind | Owner step / linearization | Caller completion and rollback |
| --- | --- | --- |
| IFGET | Coherent device/config snapshot on owner turn | Copy fixed reply after output validation. |
| IFSET | Apply validated copied config as one owner transaction; increment config epoch | Return apply result. Persistence remains explicit `ifup` user I/O and sync. |
| PING, TRACE | Preserve finite mailbox admission and identity/deadline semantics; register copied request | Collect copied reply; cancel on interruption. Existing busy outcomes remain. |
| CREATE UDP/TCP, STAGE | Reserve handle/transport generation; no user allocation in owner | Caller preallocates file/node and fd capacity; installs after success. Failed installation publishes rollback; owner reference prevents premature close. |
| BIND | Authorized copied local tuple becomes bound | Privileged port authorization captured before submission; preserve EADDRINUSE. |
| UDP SEND | Copied entire datagram accepted to bounded TX operation | No retry after claim; existing datagram result and timeout behavior. |
| UDP RECEIVE | Reserve exactly one queued datagram and copied source, stage entire payload | Copy truncated bytes/source, commit consumes whole datagram once. Abort restores queue ordering; new arrivals cannot replace reserved head. |
| CONNECT, CONNECT_UNTIL | Initiate once, then park operation separately from runnable queue | Completion wins or cancellation detaches this attempt. Never retry on ambiguous timeout. Preserve baseline policy pending explicit reconciliation of the quiet-time discrepancy. |
| SEND, SEND_UNTIL, socket WRITE | Accept up to 4096 copied bytes into TCP TX; partial positive count is final for this syscall | Accepted bytes cannot be retracted. Positive count wins interruption after acceptance; otherwise EINTR. EPIPE triggers SIGPIPE only in original caller. |
| RECV, RECV_UNTIL, socket READ | Reserve at most 65536 bytes, EOF/reset or error; immutable staged reply | Copy then consume reserved count once; abort consumes zero. Other readers queue behind reservation. No shared scratch. |
| LISTEN | Install bounded backlog on owner | Copied backlog, existing validation and errors. |
| ACCEPT RESERVE/COMMIT/ABORT | Reserve child identity without allocating another transport; adoption linearizes at owner COMMIT | Caller prepares file/fd and peer outputs; inserts provisional fd before adoption, retains rollback reference. Commit success returns fd; failure removes provisional fd, restores child if live. |
| SHUTDOWN | Mark local send half-close in owner order | Preserve receive capability and SHUT_WR-only ABI. |
| FINAL DETACH | Idempotent endpoint detach bit/generation acknowledged by owner | Nonblocking callback publication, no returning caller needed; cancel related pending operations and child reservations. |

Configuration epoch advances in FIFO claim order against submissions. A queued
bind validates its copied address against the current epoch/local IP and returns
EINVAL when stale. Unstarted connect and unbound UDP send use the configuration
at claim. Established TCP tuples remain unchanged. Already pending operations
retain their identity/deadline and existing link/error policy; owner serializes
IFSET with every protocol step. No silent mass invalidation.

Receive/accept reservations have a one-second BSP-tick lease while awaiting the
caller, distinct from I/O deadlines. Expiry restores unconsumed data/child and
requeues an untimed operation without adding a public timeout. After a signal
check, the caller acquires a result-copy reference, rechecks the reservation and
validates/copies with IF clear and no sleeping. Owner may request revocation but
cannot overwrite/reuse pinned bytes. Copy releases the reference before any
wait/signal check. COMMIT and ABORT use per-slot control bits, not new submissions.
A late commit against an expired generation performs no adoption/consumption;
caller removes its provisional fd. STOP occurs before a copy pin, or after its
release; abandoned reservations therefore cannot indefinitely monopolize a slab.
CPU failure during a pinned copy requires containment, never unsafe reclamation.

## State machine and ownership ledger

| State / event | Next state / owner |
| --- | --- |
| FREE, caller obtains FIFO credit | PREPARED; caller retains file reference and copies inputs outside lock |
| PREPARED, release publish | QUEUED; runnable ring contains token only |
| QUEUED, owner claims | CLAIMED; dequeue immediately frees ring position |
| CLAIMED, bounded step waits for protocol/resource | PENDING; rotate separate pending set, no head-of-line entry |
| CLAIMED/PENDING, terminal result | COMPLETED; result bytes precede release completion publication |
| QUEUED, cancellation wins claim race | COMPLETED(EINTR/ETIMEDOUT); owner acknowledges without side effect |
| CLAIMED/PENDING, cancel/death | CANCEL_REQUESTED bit; owner resolves accepted side effects and publishes one terminal result |
| COMPLETED, copy/transaction | Copy pin then COMMIT/ABORT; owner acknowledges reservation decision |
| Terminal ack and caller interest released | RECLAIMABLE; only owner clears retained operation/endpoint references |
| RECLAIMABLE, all references zero | FREE; increment generation; UINT64_MAX retires slot permanently |

Cancel is a synchronized bit alongside state, never an unsynchronized write to
the immutable reply. Exactly one owner terminal publication and one owner recycle.
The caller releases interest even on death; owner interest survives it. Channels
are static slot/admission objects and outlive all tasks. Returned token carries
slot plus uint64 generation, acquire predicates compare both and completion.

| Resource | Lifetime / release |
| --- | --- |
| Actor | PID and creds by value; no owner access to process table or signal frame |
| file/node | Explicit ACQ_REL operation reference acquired from own fd table before publication; owner drops it unlocked after terminal acknowledgement |
| endpoint | Stable generation plus operation reference; prevents handle reuse through deferred detach |
| reply/payload | Kernel slab reference held through copy and transaction acknowledgement; no caller stack storage |
| address space | Caller continuation's existing sched_ref only; owner never enters caller CR3 or keeps user addresses |
| provisional accepted file | Caller fd reference plus operation rollback reference until adoption/cancellation resolves |
| death hook | TCB stores token by value for idempotent abandonment outside process/scheduler locks; reaper fallback before frees |

Implementation prerequisites: add checked `file_get`/put helper (fd_get remains
borrowed), endpoint generation/ref acquisition and deferred final-release fence,
per-task token registration/abandon hooks before exit/reaper destruction, and
atomic reservation/copy-pin plus owner commit acknowledgement APIs. These are
specified dependencies, not existing capabilities. Phase 1 proves generic token
registration/death and cancellation; Phase 2 provides network-specific reference
and reservation adapters before removing any guard. No shared-address-space user
threads are admitted. Last operation-reference file release runs outside handoff
lock and merely publishes detach; it cannot recurse into owner execution.

## Concrete bounds, admission and service

| Allocation | Ceiling bytes |
| --- | ---: |
| 64 request slots including reply, pending metadata, stable channels; <=512 each | 32768 |
| 32 runnable tokens, 16 bytes each | 512 |
| 64 admission tickets, <=64 bytes each; one active call per TCB | 4096 |
| 8 payload slabs, 65536 bytes each | 524288 |
| 64 reservation/reference records, <=128 bytes each | 8192 |
| 16 endpoint detach/control records, <=64 bytes each | 1024 |
| 64 per-task registrations, <=32 bytes each | 2048 |
| Alignment, cursors and atomic control allowance | 4096 |
| **Total new static storage ceiling** | **577024** |

Hard compile-time ceiling 576 KiB (589824 bytes), excluding existing socket/TCP
pools and normal file/node allocations already present. Layout assertions must
fail the build if sizes exceed the ledger. No payload allocation on a hot path.
UDP is one slab up to existing NET_UDP_DATA_MAX; TCP send chunk 4096, receive up
to existing 65536 to preserve current maximum return size. Slabs are allocated
only when useful; stalled CONNECT/ACCEPT/RECV do not hold a payload slab. Maximum
8 simultaneous copy/staging payloads, 32 active execution credits, 64 retained
operations and 64 admission waiters; shared fds do not receive extra credits.
One outstanding typed operation
per task; death does not grant a second token until its old owner interest ends.

24 execution credits serve data operations, 8 reserved control/probe credits.
The 64 request records are distinct from these credits: a stopped caller's
completed or pending record retains its identity but releases execution credit
after its bounded step. It cannot strand one of the last eight control credits.
One record per task registration remains until terminal owner acknowledgement;
dead registrations are drained before new admissions. At the 64-task kernel
limit the worker and idle tasks reduce the number of simultaneously live user
registrations; abandoned records may delay admission until their owner ack.
Control can borrow idle data credits; data cannot consume the last control
credits. Within each class,
monotonic FIFO admission tickets with fail-closed exhaustion prevent overtaking;
payload grants rotate oldest eligible operations. Blocking calls sleep on static
admission channels, preserving signals and absolute deadlines; internal saturation
does not return EAGAIN. Nonblocking calls return existing EAGAIN when admission,
slab or endpoint readiness is unavailable, without initiating side effects. Handle
or TCP-pool exhaustion retains ENOSPC; fd exhaustion EMFILE; actual constructor
allocation failure ENOMEM. Existing ping mailbox busy result is EAGAIN, not
EBUSY. Existing UDP same-direction operation contention and finite receive
expiry also return EAGAIN even in blocking APIs; preserve these protocol-level
outcomes, distinct from internal handoff admission saturation.

Each worker turn retains RX <=64 frames and all existing protocol/link sweeps,
then <=8 new claims (<=4 data when control pending), <=8 rotating pending steps,
and <=8 cancellation/commit acknowledgements. Detach bits scan all 16 endpoints
each turn and need no submission credits. Work remaining republishes owner hint;
yield between turns so SMP=1 callers progress. Bound one typed step to one chunk,
one reservation decision or one state inspection; no blocking network loop.
Round-robin 64 slots yields <=8 owner turns to inspect each pending/control bit
under a responsive owner. Admission service guarantees order, not wall-clock
success for an untimed operation while the peer is stalled. Separate control
credits keep IFGET/link/deadline progress possible under all-data saturation.

## Errors, ordering, locks and failures

Preserve each handler's validation order (socket family/type, fd, flags/size,
ranges, tuple/capability, availability, clock/deadline, protocol), including
IFSET credential denial before pointer faults. The old CPU/affinity rejection
is removed only at the relevant Phase 2 subgate, replaced with owner admission.
Record exact existing host assertions as the per-handler error oracle; do not
generalize one universal order. Stale operation token is internal cancellation;
stale endpoint maps to the existing family outcome (EBADF for handle/adoption,
ENOTCONN for TCP continuation, EINTR for invalidated UDP wait). Deadline clock
reset/wrap returns EIO; expired deadline ETIMEDOUT; no deadline restart after
admission. Side effects accepted before an interruption return their positive
count; zero-progress interrupted calls return EINTR. EOF stays zero.

Lock graph: handoff(1), process(1), scheduler(1), device/socket/endpoint/stack/
mailbox(1) are disjoint ordinary locks; no edge between them. Only existing
scheduler pairs may nest. Allocation/VMM uses existing increasing ranks outside
handoff transactions. Never copy, wake, signal, release files or execute network
steps under handoff lock. Capture wake tokens under lock, release, then wake;
completion/death producers use the same rule. Predicates acquire-read atomic
completion/generation/admission sequence and signal readiness only.

Publication-before-owner-sleep and completion-before-caller-enrollment are safe
only if the predicate sees release-published work even when wake found no waiter.
Claim/cancel serializes under handoff lock; completion/cancel has exactly one
terminal publication. Wake-before-context-save relies on existing context_busy
eligibility. These are proof obligations for forced-interleaving host and guest
fixtures, not acceptance inferred from today's periodic timer wakes.

Owner not started, absent NIC or terminal service failure: EIO for new admitted
network work after existing argument precedence. Link loss preserves existing
protocol error/EOF policy, wakes pending requests and does not reset deadlines.
Declared owner failure may reject new work and release unclaimed requests.
Claimed memory remains quarantined until owner quiescence is proved. A hung CPU
or worker has no safe automatic restart: retain pinned storage and report terminal
service failure; cancellation cannot promise recovery from an unresponsive owner.

## Replacement protected wording at the implementation gate

“Application callers on any CPU publish copied typed requests and retain stable
kernel references. Only the registered CPU-0 network owner performs NIC I/O,
protocol mutation, configuration transactions and reservation commits. Final
close/death on any CPU publishes bounded cancellation/detach; it never executes
protocol work. All application CPUs, including BSP, use the same owner engine.”

At the corresponding accepted integration gate update PROTECTED.md, AGENTS.md
network assumptions, ARCH_REVIEW.md, net.h/net_tcp.h caller comments, network/SMP
annexes, UDP/TCP ABI plans and AP-negative test descriptions. Keep pinning until
Phase 3. Phase 0 changes none of those production contracts. Existing BSP syscall
mutation is documented here rather than misrepresented as worker execution.

## Phase 2C realized state ledger (2026-10-16)

The Phase 0 per-row reservation-record estimate (<=128 bytes / 8192 total) is
superseded for the combined UDP/TCP implementation: the checked ordinary ELF has
168-byte typed records, 64 records =10752 bytes. The eight 65536-byte payload
credits and hard 576 KiB ceiling are unchanged; there is no parallel TCP pool.
All writable static sections of owner/control/generic modules total549558 bytes;
including2048 registration and4096 alignment/metadata allowances gives555702,
below589824. This reallocates ledger headroom rather than claiming every original
per-row estimate was retained. The build-time aggregate assertion and ordinary
ELF section measurements are recorded in
[Phase 2C evidence](../roadmap/network-smp-phase2c.md).


## Phase 2D realized state (2026-10-16)

The original 128-byte estimate and Phase 2C's realized 168-byte typed record are
historical. LISTEN/ACCEPT add an existing-child identity and rollback file pointer:
the ordinary build measures 192 bytes per typed record, 12288 bytes for 64 records.
The shared owner object is 536645 bytes, control 8720, generic handoff 5729,
registration array 2048, plus conservative 4096 allowance: **557238 bytes**, below
the hard 589824-byte (576 KiB) ceiling. All `.bss` and `.data*` sections are counted.
The same eight 64 KiB payload slabs serve UDP and TCP; ACCEPT allocates no second
transport block and no independent payload pool. See
[Phase 2D measurement and ordinary-build identity](../roadmap/network-smp-phase2d.md).
