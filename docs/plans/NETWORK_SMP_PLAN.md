# Network access from SMP callers

Date: 2026-10-13. Updated 2026-10-14: Phase 0 contracts/fixtures/baseline executed;
protected TCP documentation reconciled with existing behavior by explicit user
authorization. Phase 1 generic mechanism and inert-owner host/guest gates PASS;
Phase 2A control/finite probes pass actual-code ASan/UBSan/TSan and 12 installed
guest boots (BIOS/UEFI × SMP=1/4/8, two boots each). The original composite target
FAIL for concurrent documentation drift remains qualified, not erased.
[Phase 2A evidence](../roadmap/network-smp-phase2a.md). Phase 2B has not begun.
[Phase 1 evidence](../roadmap/network-smp-phase1.md).
[Decisions](NETWORK_SMP_PHASE0_CONTRACTS.md) and
[evidence/qualifications](../roadmap/network-smp-phase0.md).

Goal: ordinary installed-root commands may execute on any CPU and use networking safely, while NIC I/O and protocol state remain owned by the BSP network worker. The current BSP-pinned spawn policy is a deliberate intermediate fix. Keep it until the handoff and network lifecycle gates pass.

This document revises the proposed five-phase plan after inspecting the current implementation. Saving this plan does not establish implementation, fixture, QEMU or physical acceptance. Future implementation must reconcile the protected cross-core socket contract explicitly.

## Critical review

The single-owner strategy is appropriate. The original queue sketch is insufficient as a foundation in these specific ways:

1. **Caller-owned asynchronous replies are unsafe.** A killed caller can lose its stack, address space and descriptors before the owner completes. Requests and replies must have kernel-owned lifetimes independent of the caller.
2. **Executing existing syscalls in the worker is incorrect.** They use `thread_current()`, the active page tables, credentials, descriptor tables and signal state. In the worker these identify the wrong process. The worker also cannot block in CONNECT, ACCEPT or RECV while it is responsible for making those operations progress.
3. **CPU 0 is not sufficient evidence of ownership.** Existing BSP syscall continuations depend on IRQ exclusion for multi-step operations. AP publication introduces concurrency, and unpinned continuations may resume elsewhere. Retaining an independent BSP fast path would require a separate proof of ordering and transaction safety. Defer that optimization.
4. **Routing only NETCTL and named socket syscalls misses essential paths.** Socket `SYS_READ`/`SYS_WRITE`, final close, inherited descriptors, accept rollback, signals and process death are part of the same contract.
5. **Host sanitizers do not prove cross-core scheduling.** Real guest publication, sleep, wake/IPI and post-switch races are a prerequisite to the network rollout.
6. **A generic mechanism has dependencies.** Scheduler predicates, stable identities, object references and reaping are dependencies even if no other subsystem consumes the mechanism yet.

The design must separate syscall-facing work from owner-facing operations. A queue of interrupt frames or arbitrary callbacks is not an acceptable implementation.

## Source contracts and evidence boundaries

Read [PROTECTED.md](../../PROTECTED.md), [AGENTS.md](../../AGENTS.md), the [network annex](../subsystems/net.md), [SMP annex](../subsystems/smp.md) and [NET plan](NET_PLAN.md) before implementation.

Relevant implementation anchors:

| Source | Consequence for this design |
| --- | --- |
| [thread.c](../../src/kernel/thread.c), `sched_wait_until` / `sched_wake_all` | Predicate evaluation occurs under a scheduler lock; wakes scan CPU queues and send remote reschedule IPIs. Preserve the context-save/post-switch eligibility rules. |
| [net_socket_syscall.c](../../src/net/net_socket_syscall.c) | User validation, fd installation and datagram copy/consume currently occur in the caller continuation. |
| [net_tcp_syscall.c](../../src/net/net_tcp_syscall.c) | Blocking loops, shared receive scratch, accept staging/adoption, SIGPIPE and output commit depend on current BSP serialization. |
| [net_socket.c](../../src/net/net_socket.c) / [net_tcp.h](../../src/net/net_tcp.h) | Static channels and generation identities exist; final close already includes deferred owner work. Audit the entire close and staged-child path. |
| [net_ping.c](../../src/net/net_ping.c) | Existing finite mailbox demonstrates copied requests, atomic readiness and wake-after-unlock; it is a reference, not proof that all socket operations have identical lifetimes. |
| [net.c](../../src/net/net.c) | The worker polls RX, services link changes and sweeps protocols; request handling must preserve progress in each service. Configuration updates touch several layers. |
| [tcp_tcb.h](../../src/net/tcp_tcb.h) | TCP receive capacity is 65,536 bytes. Do not multiply full receive buffers by a proposed request count without a memory budget. |

Existing SMP tests that reject AP direct dispatch are negative evidence for the current guard. They are not successful Ring 3 AP networking evidence. Existing BSP network passes remain baseline regressions, not acceptance of this design.

## Required architecture

### Scope and ownership

Network is the first and only consumer in this campaign. Preserve polling-only NIC ingress, CPU-0 protocol ownership, bounded socket/TCP pools, current wire behavior and absolute per-call BSP-tick deadlines. Do not add NIC interrupts, new transport features or global affinity controls to make the handoff easier.

The reusable portion owns bounded admission, request identity, publication, completion, cancellation acknowledgement and reclamation. Network-specific code owns typed operations and their protocol transactions. No generic API may invoke an arbitrary blocking subsystem function. Future storage, USB or other consumers require their own lock, lifetime and progress review; reuse is not an acceptance claim for those subsystems.

Initially, BSP and AP application callers use the same owner execution path. An inline path is allowed only for the actual registered owner executing an operation already proven nonblocking, with the same state transitions and ordering. Optimizing ordinary BSP callers is deferred until correctness and measurements justify it.

### Caller and owner responsibilities

The caller validates ABI arguments and user ranges, captures its authorized actor and stable endpoint identity, copies input into bounded kernel storage, and retains the necessary object references. Define authorization's linearization point and the treatment of changes before owner execution.

The owner receives typed, copied values and generation-checked identities. It performs a bounded nonblocking step, publishes an immutable result or registers pending protocol work, then continues NIC and timer service. It never dereferences a user pointer, caller stack, syscall frame or unretained TCB/file pointer, switches to a caller CR3, or waits for network traffic.

The resumed caller checks signals, revalidates output ranges in its own address space, copies a kernel-owned result and completes any required commit/abort handshake. Descriptor installation and signals target the original caller, never the worker. Input/output copying must obey existing VMM lifetime rules; this plan does not introduce shared-address-space user threads.

### Bounded request lifetime

Use a fixed kernel-owned pool and stable per-slot completion channels. A public token identifies a slot plus a generation; slot reuse is forbidden until all owner, waiter and operation references are released. Generation exhaustion fails closed rather than reintroducing an old identity.

Document a transition table covering at least:

`FREE -> PREPARED -> QUEUED -> CLAIMED -> PENDING or COMPLETED -> RECLAIMABLE -> FREE`

Cancellation and abandonment are explicit branches. Once an operation is claimed, cancellation is a request to the owner, not permission to free its storage. Completion/cancellation races have one terminal result and one reclamation owner. Caller death must release its interest without invalidating owner-held memory.

Long-lived protocol waits must not occupy a queue entry at the head of the runnable queue. Separate submission credits from pending-operation storage, or provide an equivalent bounded design with demonstrated fairness.

Phase 0 must choose actual counts, payload chunk sizes and a total memory ceiling. Include queue entries, replies, pending reservations, cancellation traffic, endpoint references and concurrent calls through shared descriptors. Do not assume the 16-handle limit bounds the number of simultaneous callers. Do not place a 64 KiB receive buffer on a 16 KiB kernel stack.

Specify admission behavior for blocking and nonblocking calls. Do not silently introduce `EAGAIN` into existing blocking APIs because the internal queue filled. Preserve error precedence and partial-I/O behavior; review any necessary ABI-visible change explicitly.

### Sleep, wake and locks

Use existing `sched_wait_until` and `sched_wake_all`. General spin-polling is rejected: it wastes a CPU and can obstruct the owner, particularly at SMP=1.

Wait predicates only acquire-read published atomic state and compare identities; they do not acquire the handoff or network lock under a scheduler lock. Stable channels must outlive every waiter. Publish result bytes before release-publishing completion; callers acquire-read completion before reading those bytes. Mutable cancellation fields require their own synchronization.

The handoff lock follows the Rank-1 rules. Never hold it while taking another Rank-1 lock, waking a scheduler queue, executing a network operation, copying user memory or switching context. Collect wake targets under the appropriate lock and wake after unlocking. Process-death hooks likewise publish cleanup without nesting process/scheduler and handoff locks.

Prove request publication versus owner sleep, completion versus caller sleep enrollment, cancellation versus claim, and wake versus unfinished context save. The owner idle predicate must see pending handoff work as well as existing poll hints. Timer wakeups must not conceal a lost publication wake in the tests.

### Transactions, deadlines and cleanup

Queue order alone does not make an entire syscall atomic. Define linearization and commit points separately for every operation family.

Receive needs kernel-owned staging/reservation so owner expiry, another reader, close or configuration changes cannot overwrite data while an AP copies it. Consume exactly once after the defined successful copy/commit; specify abort on invalid output or caller death. The current global TCP receive scratch cannot remain shared among concurrent caller continuations.

Accept needs explicit child reservation, caller-side file/fd preparation, owner adoption commit and rollback. Preserve the existing guarantee that descriptor failure does not accidentally consume the listener's child. Test cancellation between each step and full-pool adoption. A stopped caller holding a reservation cannot indefinitely starve unrelated operations; bound reservations and define safe abandoned-reservation recovery.

Send cancellation cannot retract bytes already accepted. State exactly which result is returned when interruption races with partial progress. Never automatically retry a claimed side-effecting request after timeout. SIGPIPE belongs to the original writer. Final close must remain valid from exit/reaper contexts without sleeping or requiring a returning caller.

Absolute I/O deadlines include admission and queue delay and never restart on dequeue, wake or continuation. Preserve current per-call validation and clock rules. Internal reclamation leases are distinct from user deadlines and must not create a new timeout for ordinary untimed calls.

Define owner-not-started and absent-NIC behavior, link loss with pending work, and terminal owner/service failure. An unresponsive owner is not safely recoverable by freeing a slot it may still access. Do not promise automatic restart without an ownership/quiescence protocol; identify any terminal containment limitation.

Configuration changes execute in the owner and have a documented ordering against queued sends, binds, connects and pending operations. Preserve coherent cross-layer updates; specify stale-endpoint handling instead of assuming an address change invalidates everything safely.

## Phase 0 — Contracts and fixtures

Deliverables:

- Complete the typed operation inventory, state/transition table, ownership ledger, cancellation/commit rules, error mapping, lock graph, memory budget and fair service budget described above.
- Trace syscall entry, fd references, inherited close, process reaping, signal interruption and scheduler wake paths, including indirect callbacks. Record any missing reference/lifecycle API as an implementation prerequisite.
- Define the replacement protected contract: callers may publish from any CPU; owner-only NIC/protocol mutation remains on CPU 0. Identify documentation that must change at the implementation gate. Reconcile existing documentation/code discrepancies without silently changing unrelated TCP policies.
- Build disposable installed-root fixtures with `/etc/network/network.conf`, `/etc/resolv.conf`, login/sudo and NICs. The old `/etc/network.conf` fixture path is not the installed default after the current fix. Apply configuration explicitly with `sudo ifup`; boot-time automatic application is not assumed.
- Add isolated fixture instrumentation to record actual Ring 3 syscall-entry CPU and owner execution CPU. Force AP entry through a test-only launch mechanism while production commands remain pinned. A filename, unpinned flag or BSP kernel direct-dispatch test does not establish AP execution.
- Capture current pinned baseline behavior and source/build hashes. Exercise fixture scripts and disk exclusions before implementation begins.

“No code” here means no production handoff, syscall or spawn-policy changes. Fixture scripts and test-only probes are permitted and must stay out of release images.

Gate: design reviewed with concrete choices and no unresolved safety dependency; fixtures built and audited. This document alone does not pass the gate. Phase 1 depends on this gate and the scheduler/lifetime contracts, not on “nothing.”

## Phase 1 — Handoff mechanism and cross-core proof

Implement only the bounded transport/lifetime layer and an inert test owner. API names are provisional; the essential operations are submit copied work, claim, publish completion, request cancellation, collect/release and abandon. A three-function submit/poll/complete API without lifetime operations is insufficient.

Host gates use actual mechanism code with explicit scheduler adapters: multiple producers, saturation, owner unavailable, reordered claim/completion, cancellation before/after claim, caller death, stale generations, exhaustion, owner wake, allocation failure where applicable and exact reference/pool recovery. Run ASan/UBSan; use a separate race-detector build where supported, plus deterministic interleaving tests. Sanitizer success alone is not a synchronization proof.

Guest gates: BIOS/UEFI × SMP=1/4/8, with real Ring 3 AP callers at 4/8 CPUs and an inert BSP owner. Force publish-before-sleep and complete-before-enrollment races, remote wake and post-switch races, STOP/CONT/KILL, concurrent producers and saturation. Show owner/caller CPU provenance and bounded recovery. Keep production spawn pinning.

Gate: host and all six guest configurations pass, with no stuck waiter, stale delivery, double reclamation or resource leak. Phase 2 cannot start on host results alone.

## Phase 2 — Network integration with staged gates

Keep production commands pinned. Use test-only forced AP callers for positive cross-core acceptance. Integrate in reviewable subphases:

| Subphase | Required behavior and gate |
| --- | --- |
| 2A: control / finite probes | IFGET, authorized IFSET, ping and traceroute; permission/pointer precedence, concurrent configuration and cancellation. `ifup` persists and reloads the installed default file. |
| 2B: UDP / DNS | Create, bind, send, receive, close and shared lifetime; bounded queues, cancellation and independent UDP capture. `nslookup` and UDP DNS fallback paths work. |
| 2C: TCP client / streams | CONNECT, SEND/RECV and timed variants, socket READ/WRITE, shutdown/EOF/reset and SIGPIPE. Numeric and hostname clients, DNS TCP fallback and HTTP downloads pass exact-byte/capture checks. |
| 2D: listener / lifecycle | LISTEN/ACCEPT staging and adoption, shared/inherited fds, competing readers/acceptors, descriptor exhaustion, rollback, STOP/CONT/KILL, close during pending work and reaper cleanup. |

At every subgate, exercise actual BSP and AP caller continuations using the same owner engine, preserve the pinned baseline, and update old AP-rejection tests only when replaced by stronger positive and failure coverage. Retain owner-only mutation assertions.

Fairness gate: unanswered CONNECT, empty listener, stalled receive and zero-window send cannot prevent IFGET, ping, another endpoint's transfer, link recovery or protocol deadline sweeps. Bound runnable-request credits per worker turn, admission starvation and cancellation latency; pending network waits are not synchronous worker jobs.

Final gate: BIOS/UEFI × SMP=1/4/8 on installed disposable roots for `ifup`, `ifconfig`, `ping`, `nslookup`, `wget`, plus UDP, TCP listener/stream and lifecycle fixtures. At 4/8 CPUs prove AP entry, including concurrent callers. Include e1000 and e1000e coverage and an independent controlled peer/capture for hostile wire and deadline cases; list exact case counts in the runner manifest.

Verify configuration bytes/metadata after sync and reboot, Linux filesystem audit, download hashes, shell recovery and exact pool/reference reclamation. Link-flap, absent-NIC and saturation cases must terminate with documented outcomes. Public internet availability is not a gate dependency.

## Phase 3 — Restore ordinary unpinned commands

Only after Phase 2 passes, restore ordinary VFS spawn eligibility to the intended unpinned policy, including correct affinity/mask initialization before publication. Preserve explicit benchmark `--worker` placement. Remove CPU/affinity rejection only from handlers whose ownership and lifecycle paths have passed integration.

Repeat the installed-root BIOS/UEFI × SMP=1/4/8 matrix using ordinary shell-launched production commands. Prove actual AP syscall entry through test instrumentation without changing release command arguments. Cover shell/sudo/pipeline/background descendants, inherited socket descriptors and STOP/CONT/KILL. Run relevant non-network spawn, permissions, pipeline and persistence regressions.

Gate: ordinary commands work on BSP and APs, benchmarks retain explicit placement, non-network descendants remain correct, and test-only launch/probe code is absent from release images. Publish a fresh image with build hashes and precise automated evidence; physical acceptance remains separate.

## Phase 4 — Physical acceptance

User-run Dell 5590 installed-root acceptance: ordinary unpinned commands, `sudo ifup`, `ifconfig`, gateway ping, DNS and HTTP download with checksum. Repeat after reboot to demonstrate configuration persistence/reload. Include concurrent/background commands and cable loss/recovery, with shell recovery.

Record firmware, CPU count, image hash, config path, commands, results and failures in a roadmap evidence report. Establish AP execution through approved diagnostic evidence; functional success alone does not establish cross-core use. No physical write or automated hardware acceptance is authorized by this plan.

Gate: explicit user physical evidence accepted. QEMU success does not pass this gate; a retry does not erase a prior unexplained failure.

## Phase 5 — Deferred throughput redesign

Multiple RX queues, per-CPU protocol workers and flow steering remain out of scope. They require measured demand, hardware capability evidence and a separate ownership design. This handoff may remain useful for control-plane operations, but it does not prove a future SMP data plane. Physical acceptance here is a release gate, not a logical dependency that automatically justifies RSS work.

## Evidence and sequencing

Sequence: Phase 0 -> Phase 1 host and guest proof -> Phase 2A–2D -> Phase 3 -> Phase 4. Phase 5 is a separate deferred proposal.

Each campaign retains source/build hashes, exact QEMU argv, disposable image identities, paired OVMF variables, serial logs, CPU provenance, peer captures, filesystem audits, failure artifacts and bounded process cleanup. Never attach physical disks or overwrite shared fixture images. Report host mocks, real guest execution and physical evidence separately.

The foundation is accepted when ownership, progress, cancellation and reclamation are demonstrated under contention and failure—not merely when five commands succeed once.
