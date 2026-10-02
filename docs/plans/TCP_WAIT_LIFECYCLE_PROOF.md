# NET-2 step 3 client wait/lifecycle argument

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
