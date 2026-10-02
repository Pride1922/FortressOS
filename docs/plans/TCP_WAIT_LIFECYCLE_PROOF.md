# NET-2 client and Step 4 ACCEPT wait/lifecycle argument

## Reviewed design: reservation-free sleeping continuations

The implementation design removes persistent TCP operation reservations rather
than renewing UDP reservations. This is an intentional replacement of the
earlier transition sketch, preserving its observable indefinite-wait contract.
No reservation exists while a stream caller sleeps: RX peek/copy/consume and TX
acceptance complete in one BSP IF-clear continuation. There is nothing to lease,
expire, remember or reclaim on STOP. The requirements below remain acceptance
criteria; executed evidence and limitations are in
[net2-step3.md](../roadmap/net2-step3.md). This is an engineering argument and
runtime verification, not a machine-checked formal proof.

Each common handle has a separately allocated TCP endpoint identity and a static
event channel. A local wait snapshot carries slot, endpoint generation and event
counter. Only the worker publishes/wakes readiness; close may publish identity
invalidation under the socket lock and defer wake to the worker. Predicates
acquire-load generation/event only. Under the existing IRQ-excluded scheduler
insertion, an event is either seen before BLOCKED publication or wakes afterward.
Every loop snapshots the event before checking readiness; a competitor consuming
data results in another check/sleep, not a user-visible EAGAIN for blocking mode.

The caller's own fd reference pins its file/endpoint while its single process
continuation sleeps. Inherited/dup references use existing file ref_count;
another process cannot remove this caller's own fd. Signal handlers run at the
user-return boundary, after an interrupted syscall unwinds. KILL/reaping closes
the fd table using existing fd_close_all, with no extra local reference to leak.
STOP parks the exact continuation in sched_wait_until/process_signal_interrupt;
CONT rechecks predicate. Slot reuse cannot match an old endpoint generation.
Generation exhaustion fails allocation, never wraps to an old identity.

Actual existing anchors: thread.c:sched_wait_until, process_signal_interrupt,
fd_get, fd_clone_table, fd_dup/dup2 and fd_close_all; vfs.c:vfs_close. Their
contracts are unchanged. All user validation/copy and signal publication occur
without the socket lock. Under BSP IF-clear exclusion, worker protocol mutation
cannot run between snapshot/copy/commit. AP final-close only publishes a detach
request; it does not mutate a connection or perform NIC/protocol work.

Worker prepare -> unlocked NIC submit -> commit uses one explicit IF-clear
bounded transaction with no socket lock across the driver call. AP close requests
are deferred, so neither BSP preemption nor AP teardown can invalidate an action
between submission and commit. Full tuple demux, pool/timer mutation and actual
wire submission remain worker-owned. Syscalls may queue TX/consume RX under the
same BSP exclusion (no subsystem lock across user copy); they never call the
NIC or feed protocol RX.

CONNECT has one endpoint-wide pending handshake with an exclusive generation-
checked cancellation on interruption. SEND accepts a positive short count once;
it only sleeps before accepting any bytes. RECV copies available ordered bytes
then consumes once; reset follows buffered bytes and FIN produces EOF after data.
Final close requests orphan detach, independent of channel/file lifetime. Sharing
allows competing callers to progress when scheduled; no strict FIFO or bounded
starvation claim is made. Step 4 ACCEPT remains outside this proof.

| Event | Linearization / ownership | Continuation behavior |
| --- | --- | --- |
| TX space | Queue copies once with BSP IF clear; connection retains bytes | Positive short count returns immediately |
| RX bytes | Peek, unlocked user copy, consume with BSP IF clear | Invalid ranges consume nothing; competing reads retry |
| Readiness publication | Worker observes RX/TX/state/error changes, release-publishes event before unlocked wake | Predicate acquire-loads only generation/event |
| Idle time / old UDP lease interval | No TCP reservation exists | No timeout, expiry reason or timer-only wake churn |
| STOP / CONT | Existing scheduler parks/resumes exact continuation | No owned RX bytes or pending syscall TX to reclaim |
| Caught signal / KILL | Existing interruption check returns EINTR before user-return signal processing | CONNECT requests generation-checked cancellation; read/write own no sleep reservation |
| Final fd close | Existing final ref callback requests detach, invalidates endpoint identity | Worker alone detaches/drains orphan; shared references prevent premature final close |
| Reset / FIN | Engine sequence validation and buffered ordering | Data then reset error; data then EOF |
| Slot reuse / counter exhaustion | New nonwrapping pool generation; event exhaustion invalidates identity | Stale wait cannot operate on replacement endpoint |

Budget: 16 static endpoint/channel records, eight connection sweeps, at most eight
TCP data/control submissions plus one queued TIME_WAIT ACK per worker pass.
Unresolved ARP sends at most once per block per second. At most 64 RX packets
precede the timer/TX sweep, which still runs after a full batch; the existing
full-batch yield and idle tick wait stay unchanged. The cap is per worker pass,
not per timer tick. Each net_tcp_input restores incoming IRQ state; its IRQ-off
scope is per segment, not one continuous RX-batch scope. All storage is bounded.
The driver has existing bounded synchronous DD/PIT polling and diagnostics;
IF-clear submission inherits that latency. This is not a measured physical
IRQ-latency or throughput guarantee. Device/scheduler rank-1 locks do not nest.

## Step 4 ACCEPT extension: pre-implementation ownership argument

2026-10-02. This extends the client argument for the planned implementation;
ACCEPT code and runtime acceptance remain pending. The implementation must
match these steps before its proof gate is considered satisfied.

### Existing exclusion and fd-table evidence

thread.c:fd_alloc inserts a fully initialized file pointer and clears flags;
fd_get reads that caller's table. Neither acquires a table lock. thread.h:tcb_t
owns fd_table/fd_flags per continuation. fd_clone_table copies the entries and
atomically increments file reference counts: children share files, not a mutable
fd table. fd_dup/dup2 likewise share files. There is no shared-table user-thread
API in this phase. A blocked caller retains its own listener fd; another process
closing its copy cannot destroy that reference. Signal handlers run only after
the syscall returns. Reaping targets terminated continuations, not the running
acceptor. BSP-only syscall eligibility and IF-clear entry exclude a competing
acceptor, spawn/dup/close or signal-handler operation during the transfer.
This is not a general SMP fd-table synchronization proof.

### Wait and wake policy

Capture listener generation/event before testing for an eligible child. With
no child ready, call existing sched_wait_until using the static listener channel
and atomic-only predicate, owning no queue entry, staged fd/file, user output
or reservation across sleep. Worker release-publishes readiness/identity changes
then calls sched_wake_all without endpoint/socket locks. This follows existing
thread.c:sched_wait_until and sched_wake_all, not a new wake_one primitive.
After wake, recheck interruption, outputs, generation and readiness. One child
can wake multiple acceptors; the first BSP continuation to complete transfer
takes it, and others recheck and sleep. No FIFO or starvation bound is claimed.

### Transfer order and rollback

1. Revalidate flags/output ranges/capacity, online state, caught interruption
   and saved listener identity with BSP IF clear. Find an eligible completed
   child without removing it. Save only local peer/identity values.
   process_signal_interrupt may itself switch context for STOP, even when it
   eventually returns false after CONT. Therefore repeat output range/capacity,
   online and generation validation after the last interruption check and
   before selecting/staging the child. No interruption check is permitted once
   staging begins; otherwise STOP could retain unpublished allocations.
2. Prepare a common socket handle and fully initialized heap file/node for
   adoption, without allocating another transport block. Perform allocations
   outside rank-1 locks and never sleep or enable IF. The provisional endpoint
   must not own the queued child's block or expose it to worker maintenance.
3. Call fd_alloc while the child remains queued. If it fails, destroy only
   staged file/node/handle resources and return EMFILE; heap/common-handle
   failures return ENOMEM/ENOSPC respectively without child removal. Staged
   cleanup must not invoke ordinary TCP close on the queued child.
4. After successful fd insertion, transfer the existing transport block from
   the listener queue to the new endpoint and remove its queue entry in one
   manager-lock section. This removal/adoption is the ownership linearization
   point. The selected identity must still match; mismatch here is an invariant
   violation, not an ordinary recoverable failure. No fallible allocation,
   validation, wait, tick, RX processing or signal check remains after insertion.
5. Release the manager lock, set fd flags, then copy the staged peer sockaddr
   and size to previously validated user ranges without subsystem locks. Return
   the fd. BSP IF exclusion and the caller's address-space ownership prevent
   mapping changes between validation and copy. A future fault-recovering copy
   API or shared address-space API would require revisiting this proof.

The briefly provisional inserted fd is unobservable: it exists only in the
running caller's private table, with IF clear and no callback/reentry that can
inspect or operate on it. AP file close may publish listener detach, but cannot
be final close while this acceptor still owns its reference; APs cannot mutate
the pending queue or accepted transport. Complete insertion/adoption/output
before any sleep, IRQ restoration or user execution. Do not present fd_alloc
itself as atomically publishing a ready child across arbitrary CPUs.

### Exit, interruption and error contract

Closing another shared descriptor leaves the listener live and does not wake
or fail ACCEPT merely due to that close. Invalid initial fd gives EBADF; a
non-listener stream gives EINVAL. Saved listener generation invalidation returns
EBADF; caught interruption wins with EINTR if both are present. Normal final
listener close while a live blocked acceptor owns its fd is unreachable under
the current per-continuation table model. Test invalidation defensively via a
manager fixture, without claiming a nonexistent shared-table close API.
STOP retains no reserved child and CONT retries/revalidates. Caught signal
before transfer returns EINTR without consuming a child. KILL follows existing
termination/descriptor teardown, with no promised syscall result and no staged
allocation across a wait to leak. A successful transfer wins over later signal
publication. Listener final close cleans only unaccepted children; an adopted
child has independent endpoint/file/channel lifetime and remains usable.

### Required implementation evidence

Fault-inject each allocation/fd failure and prove the same child is subsequently
accepted once. Test invalid outputs/caught interruption without dequeue, two
acceptors racing one child, shared descriptor close preserving a blocked accept,
defensive generation invalidation, STOP/CONT/KILL, listener exit and independent
accepted-child traffic. Verify no placeholder leaks and no ninth-block adoption
dependency. Host fixtures do not establish real IRQ/signal execution; run Ring 3
fixtures separately. Preserve the worker prepare/commit ordering and trap.

### Seven-property pre-coding audit (2026-10-02)

The seven properties here are the extension's requested linearization, fd
publication, shared lifetime, competing acceptors, AP exclusion, rollback and
STOP/CONT/KILL properties. This verifies the design against existing APIs;
it does not mark future listener code or its runtime tests as already passed.

| Property | Existing source evidence / argument | Step 4 obligation |
| --- | --- | --- |
| 1. Transfer linearization | Sole BSP protocol ownership and IF-clear syscall execution exclude queue mutation by another continuation; net_tcp_close only publishes deferred detach. | Move queue ownership and adopt the same block in one manager-lock section after fd insertion; selected identity mismatch must trap. No fallible step follows insertion. |
| 2. Fd publication order | thread.h:75-76 embeds each fd table in its TCB; thread.c:1775 fd_alloc stores a file pointer, and :1745 fd_clone_table copies tables while sharing references. There is no shared-table thread API. | Fully initialize staged file/node before fd_alloc; queued child is untouched on allocation failure. Finish adoption/flags/output without enabling IF, sleeping, or callbacks inspecting the provisional fd. |
| 3. Shared listener lifetime | fd_clone_table/dup/dup2 increment file references; net_socket.c:socket_close runs only through final VFS close. Each blocked acceptor retains its own descriptor. | Closing another process's reference must preserve the listener. Adopted children must have no listener ownership link; final listener teardown touches unaccepted children only. |
| 4. Competing acceptors and wake | thread.c:633 sched_wait_until checks predicate and inserts BLOCKED under scheduler IRQ exclusion; :687 sched_wake_all queues every matching waiter. net_tcp_snapshot/net_tcp_ready use atomic identity/event reads. | Publish event before unlocked wake_all, snapshot before availability check, and recheck after wake. One continuation removes one child; losers resnapshot and sleep, without FIFO claims. |
| 5. AP and lock exclusion | net_tcp_close is deferred publication under manager lock; existing BSP eligibility rejects AP socket syscalls. spinlock.h:138 restores saved flags, preserving an IF-clear caller. thread.c:229 reaps only on BSP and refuses the current thread. | No AP queue/block mutation. No socket/manager/process/scheduler rank-1 nesting. Allocate/copy/wake outside manager lock, preserve worker prepare/commit ordering. |
| 6. Failure rollback | fd_alloc failure does not insert a descriptor; holding the child queued until resource preparation removes the need for an abandoned-child reservation. Current stream constructor would allocate another block and is unsuitable for adoption. | Add a dedicated staged-handle constructor/cleanup that never owns or closes the queued block. Heap/handle/fd/validation/interruption failures before transfer leave queue membership unchanged. Prove with fault injection. |
| 7. STOP/CONT/KILL and stale identity | thread.c:2047 process_signal_interrupt may park via sched_stop_current; KILL stays pending to unwind; syscall.c:1407 invokes signal handling at user return. Generation/event snapshots detect reuse. | Hold nothing staged over either wait or interruption check. Revalidate again after the final interruption check. EINTR precedes invalidation EBADF; STOP resumes/retries, KILL unwinds then tears down with no user-result promise. |

Verdict: pre-coding design gate satisfied with the explicit post-interruption
revalidation requirement above. All seven have existing mechanism support and
finite implementation obligations; no scheduler, fd-table, signal, wait-signature
or lock-rank change is required. Listener code must still demonstrate the stated
adoption/rollback/runtime gates before Step 4 acceptance.

### Step 4 implementation correspondence and evidence

The pre-coding argument/audit above is retained as the design record. Step 4
implements it in net_tcp_syscall.c:accept_socket/accept_outputs,
net_socket.c:create/net_socket_stage_stream/socket_close/net_socket_finish_adopt
and net_tcp.c:net_tcp_stage/net_tcp_unstage/net_tcp_accept_commit.
No accepted file or child reservation exists over sched_wait_until or
process_signal_interrupt. Validation repeats after the last interruption check;
staging then remains entirely IF-clear and nonblocking. Ordinary staged-file
close calls unstage, not protocol detach. Child ownership moves under the manager
lock after fd_alloc; invariant mismatches trap. Output copies occur after unlocking.
socket_close captures stream/staged kind before releasing its common slot; a
failed staged constructor racing reuse cannot change which manager cleanup the
original close performs. A host unlock interleaving fixture covers that boundary.

Host fixtures assert the child remains queued at fd insertion, unchanged child
identity across heap/fd/common-handle/invalid-output/signal failures, adoption
with all eight blocks occupied, child independence and defensive invalidation.
The five BIOS/UEFI/NIC/BSP-SMP QEMU server cases exercise real Ring 3 competing
acceptors and inherited fd references, caught SIGINT, STOP >9s/CONT and KILL
recovery. Thus all seven implementation obligations have corresponding evidence;
this remains an engineering argument, not a formal proof or physical IRQ claim.
Full command/capture qualifications: [net2-step4.md](../roadmap/net2-step4.md).

2026-10-02. The original renewal sketch below is retained as review context and
is superseded for the client by the reservation-free design above. ACCEPT must
be proved before step 4 enables it. Scheduler, signals, lock ranks and wait
signatures stay intact; no timed-receive fallback was selected.

## Existing evidence and gap

`src/net/net_socket.c:reserve`, `net_socket_ready`, `net_socket_result` and
`net_socket_finish` implement finite UDP token reservations. The predicate uses
atomic token/done loads and the channel has static backing. In
`src/net/net_socket_syscall.c:net_socket_syscall`, the continuation waits once,
checks interruption/liveness, revalidates outputs, stages/copies with IF clear,
then commits. A stale token currently returns EINTR. This does not prove that
TCP can renew a reservation indefinitely or distinguish lease expiry from
endpoint destruction. Preserve UDP's accepted behavior.

## Earlier renewal sketch (superseded for client)

1. Validate scalars/ranges and retain a stable endpoint identity/reference using
   an existing permitted lifetime mechanism. Identify fd, handle, connection
   generation and reservation token separately; slot number alone is insufficient.
2. Reserve a bounded operation and test published readiness before sleeping via
   the existing `sched_wait_until(channel, atomic_predicate, &local_wait)`.
   No user buffer, syscall frame or scheduler TCB pointer enters protocol storage.
3. Worker publication precedes wakeup outside the socket lock. Lease expiry
   releases only the operation reservation, publishes a distinguishable renewal
   reason and wakes the continuation. Connection/TX/RX lifetime is independent.
   Specify how that reason remains identifiable after token/slot reuse without
   retaining an unbounded history of expired operations.
4. On resume, check genuine interruption/endpoint failure separately from lease
   expiry. An expired operation on the same live endpoint revalidates ranges,
   reacquires a reservation and sleeps again without a user-visible timeout.
   Recheck endpoint identity and readiness atomically with registration to close
   the publication/re-registration race. Fairly handle another shared-fd waiter
   taking the reservation; no indefinite busy loop or EAGAIN for blocking mode.
5. On success, revalidate outputs, stage a bounded snapshot, drop locks, copy
   with the existing BSP/IF-clear exclusion, then commit only against the same
   endpoint/generation/token. RX bytes are consumed exactly once after copy;
   accepted TX bytes are never accepted again when renewing a wait.
6. Signal/exit/final-reference close cancels reservations, not already accepted
   transport TX. Final close detaches the application and starts bounded orphan
   teardown. Old continuations/completions cannot affect replacement endpoints.

## Required proof record

- Table of each state/event: ready, expiry, renewal contention, signal, STOP,
  CONT, KILL, close, reset, EOF and fd/slot reuse; owner and linearization point.
- Exact reference lifetime through sleep and process exit; static-channel
  lifetime; token/generation rollover policy; no pointer to a freed file/TCB.
- Lock and IRQ state at registration, publication, wake, revalidation,
  copy/commit and teardown. Predicates are atomic-only under scheduler lock;
  scheduler/process/socket rank-1 locks never nest; no lock spans a switch.
- Maximum storage/stack and finite worker work budgets; renewal must allow
  idle sleep and progress for other waiters, UDP, ICMP and the shell.
- Distinct internal reasons mapped to errno only at the syscall boundary.
  Partial send acceptance returns the positive count; reset follows buffered
  ordered RX; EOF follows data. Include CONNECT's separate 30-second handshake
  deadline and interrupted-open cleanup; do not renew a handshake forever.
- For ACCEPT: child remains queued through invalid user output, failed fd
  allocation or abandoned reservation; successful transfer has one owner.

## Required executable evidence before enabling integration

Host fixtures must exercise repeated renewals, expiry immediately before/after
publication, shared-fd contention, stale tokens and handle/connection reuse,
copy failure without RX consumption, accepted TX during STOP/expiry/close and
pending-reset ordering. Real Ring 3 fixtures must cover sleep across multiple
leases, STOP beyond a lease then CONT, caught interruption, KILL/reclamation,
dup/inherited references, prompt recovery and idle CPU/progress observations.
Record actual commands/results; mocks cannot establish IRQ/scheduler behavior.

If the proof cannot be completed using existing contracts, stop and report
the failed obligation. Proposed fallback for discussion: an explicitly specified
application receive timeout with ETIMEDOUT, documented units/default/configuration
and precedence; no silent five-second EAGAIN. Neither this fallback nor a
scheduler/signal change is authorized by this document.
