# Network SMP Phase 2A

2026-10-14. **PASS: actual-code ASan/UBSan and TSan; BIOS/UEFI × SMP=1/4/8,
two installed-root boots each, 12/12 guest cases and filesystem audits.**
The original composite invocation ended **FAIL at source preservation** because
five Markdown files changed concurrently. Its FAIL manifest is retained. An
independent qualification verifies unchanged executable sources and exact guest
recovery; this is not an unconditional PASS of that original command.

Only IFGET, authorized IFSET, ping and traceroute are integrated. Ordinary
production spawn pinning and all UDP/TCP CPU/affinity guards remain unchanged.
There is no Phase 2B implementation, general AP socket claim, additional protocol
worker, NIC interrupt path or physical acceptance.

## Implementation and ownership audit

`syscall_dispatch` retains caller-side size, range and header validation. IFSET
retains `syscall_actor -> permission_capability(CAP_SYS_ADMIN)` before size and
pointer checks. The authorized operation is captured in the copied typed request;
the worker neither obtains credentials from its own process nor revisits caller
metadata. Later interruption does not undo an already applied configuration.
There is no automatic retry of a claimed side effect. Persistence is subsequent,
explicit `ifup` filesystem I/O, not part of the owner transaction.

`net_control_call` copies fixed input into a static slot while PREPARED, then
publishes through the Phase 1 transport. Both BSP and AP callers take this path.
The sole registered CPU-0 worker asserts its own TID and CPU before servicing
control requests. No caller frame, user address, stack, TCB, descriptor or
arbitrary callback is retained. These operations need no file/endpoint reference
or payload-slab adapter. Such adapters remain prerequisites for later subphases.

The owner performs at most eight claims, eight rotating pending inspections and
eight generic acknowledgement inspections per turn. Existing RX (64 frames),
link service and protocol/timer sweeps precede control service. A probe starts
once, retains its mailbox generation while pending, and is collected into its
kernel-owned reply by the owner. Mailbox busy still returns EAGAIN; generic
admission saturation sleeps/retries and does not introduce public EAGAIN.

Results remain immutable while caller interest exists. A stopped caller can
retain its result without holding the finite probe mailbox or execution credit.
The resumed caller handles signals, copies its retained result to a local value,
releases interest, and revalidates the user output in its own active address space.
Only the caller copies user memory. Process death uses the existing Phase 1
abandon hooks before descriptor, stack and address-space destruction.

Cancellation after claim explicitly calls `net_ping_cancel` and
`net_ping_worker_tick` in the owner before generic terminal publication. This
includes the indirect `net_ipv4_ping_cancel` / `net_ipv4_trace_cancel` path.
Generic acknowledgement still cannot reclaim PENDING subsystem work. The only
generic extension is a copied `cancel_status` scalar: network unclaimed
cancellation uses the actual SYSCALL_EINTR (-22), while the inert fixture keeps
its historical default -4 value.

The worker sleeps on `handoff_owner_channel`. Submission, cancellation and
release provide explicit notifications; existing poll hints and BSP timer wakes
use that channel too. Pending records alone do not keep it runnable. Admission
waiters also receive timer notifications for absolute trace deadlines. Predicates
read atomic publication and BSP ticks without acquiring a lock under scheduler
locks. Network, handoff, process and scheduler Rank-1 locks do not nest; user
copies, network steps and scheduler wakes happen outside the handoff lock.

Traceroute retains its original absolute command deadline through admission,
queue delay and protocol execution. Admission expiration returns ETIMEDOUT
without starting traffic. Per-call clock floors reject backward time with EIO;
the owner sleep predicate notices backward BSP time. Ping retains its existing
relative echo timeout and finite mailbox lease; no socket deadline policy changes.
The existing shared mailbox generation now fails closed at UINT64_MAX instead
of wrapping. Trace wire identity exhaustion remains fail closed.

Probe validation originally read the mutable IPv4 subnet without synchronization.
`net_unicast_snapshot` now serializes that read with configuration mutation.
IFSET applies the existing cross-layer setters in the owner, clears ARP as before,
and increments a fail-closed configuration epoch in FIFO claim order. It does not
reinitialize protocol pools, NIC rings or established TCP connections. Cached
link-state validation in the caller performs no NIC MMIO.

Measured host symbols: generic records 0x1600 (5,632 bytes), copied control values
0x2200 (8,704 bytes), **14,336 bytes combined**, excluding small pool metadata
and the existing per-task registrations. Compile-time bounds remain below the
576 KiB campaign ceiling. There is no new hot allocation or large stack buffer.

## Host gates

`build/network-smp-phase2a/host-nce061tu/manifest.json`: **ASan/UBSan and TSan PASS**.
Actual handoff, control, mailbox, IPv4 and configuration code runs with explicit
pthreads scheduler/lock, clock, device and user-tool adapters. Forty-eight
producers issue 9,600 requests after forcing 48 retained records and 32 queued
credits while the owner is parked. Coverage includes concurrent subnet
validation/configuration/probes, completion of every producer, 48 pre-admission
signal checks, claimed pending cleanup, fresh mailbox reuse, cancellation before
claim, expired trace admission and a deterministic backward-clock injection.
Exactly **9,604 records** are reclaimed, with zero live/queued/pending records.
The expired admission creates no record or side effect. These adapters do not
establish IRQ, signal-frame or real scheduler behavior.

`build/network-smp-phase2a/final-regressions.log`: IFCONFIG/parser/ifup,
Ethernet worker/polling, ping and trace codec/mailbox/CLI sanitizer gates PASS.
The Phase 1 generic host regression also passes ASan/UBSan and TSan with
14,400 concurrent requests and exactly 14,474 reclamations; its manifest is
`build/network-smp-phase1/host-2km777q_/manifest.json`.

`build/network-smp-phase2a/tcp-tuple-host.log`: actual TCP socket regression PASS.
A TEST_SMP_MEMORY-only tuple observer verifies that changing the default local
IP preserves an established connection's tuple and connected status. Restoring
the configured address then transfers exactly 16,384 peer-audited bytes.
No production TCP policy is changed by this observation helper.

## Real guest execution and persistence

Final runtime evidence: `build/network-smp-phase2a/gate-8mpttgme/manifest.json`.
`qualification.json` independently checks all 12 cases and exact counts while
preserving the original FAIL result. `exclusions.json`: **12 argv audits and
61 negative exclusions PASS**, with no process launched by the exclusion selftest.

| Firmware | SMP | Actual Ring 3 entry mask | Reclaimed per boot | Remote wake observations, boot 0 / 1 |
| --- | ---: | --- | ---: | --- |
| BIOS | 1 | 0x1 | 204 | 0 / 0 |
| BIOS | 4 | 0xf | 588 | 1919 / 1282 |
| BIOS | 8 | 0xff | 1036 | 2772 / 2358 |
| UEFI | 1 | 0x1 | 204 | 0 / 0 |
| UEFI | 4 | 0xf | 588 | 1765 / 1285 |
| UEFI | 8 | 0xff | 1036 | 2654 / 2300 |

Snapshot-only staged AP launch comes from the proven Phase 0 fixture. Aggregate
provenance samples CPU at the actual Ring 3 SYS_NETCTL dispatcher entry. All
typed owner steps assert CPU 0 and the registered worker identity; the owner
kind mask is 0x1e. Foreground probes require real shell exit status zero.
The fixture exercises invalid size before pointer checks, null/read-only and
canonical-boundary outputs, malformed headers, denied IFSET before size/pointer,
authorized IFSET size/pointer precedence, BSP/AP queries, successful AP ping,
finite AP trace, and repeated authorized AP configuration changes.

Pending probes receive STOP/CONT, KILL and default SIGINT through real scheduler
and dispatcher paths. Configuration proceeds while a stopped caller retains its
operation. Concurrent query producers use up to seven background jobs; every
CPU also runs a foreground reader. Each final drain acquires FREE/RETIRED states
and verifies every typed probe token is zero. The independently checked exact
count is `76 + 64*SMP + 64*min(7,SMP)`. Guest saturation is not 48 or 64 simultaneous
Ring 3 callers: host adapters establish the 32-credit saturation case.

Every configuration cleanly shuts down and boots the same disposable installed
NVMe root again. `sudo ifup` explicitly applies settings; boot-time automatic
application is not assumed. Both `/etc/network/network.conf` and
`/etc/resolv.conf` are dumped and compared byte-for-byte after **each** boot;
Linux debugfs verifies root:root 0644, and `e2fsck -fn` passes after all 12 boots.
The default reload preserves both DNS servers. The current helper location is
`/sbin/ifconfig`; the fixture uses that explicit path. Ordinary `ifup`, query
and gateway ping also use the production owner adapter.

Exact source identities, snapshot overrides, build logs, redacted command
transcripts, argv, serial/stderr, OVMF variables and disposable disks remain in
the campaign. `qualification.json` additionally records instrumented compiled
input hashes. Initial baseline disk hash remains unchanged. Only one disposable
NVMe disk is attached; no shared live ISO/image or physical device is written.

Instrumented kernel SHA-256:
`1749284d174d90b9a7e8df8c3bd70d994e297ddb6f3bb51509a5f838ef59e886`.
Initramfs:
`9ac44157d9e950c8c86dbc40c3ee84e03e19065c57d2dccac20492d31e2a2ace`.
Baseline disk:
`f1dd8d44845e5f9b186e299665bb83b2edca81d3182b26c003a2351b67b732af`.

`build/network-smp-phase2a/release-verification.json`: isolated uninstrumented
strict build and fixture absence PASS. Kernel:
`41b4b74a85ece70c76f49b90dba3e67e9bb7d464baaf77c6e5d689958840cf1b`;
initramfs:
`eee33e800e3ae32e9908d4ad1d3708d9c7b53aace3bc1a1ae39a0c80a1e7ebb9`.
No shared release ISO/image was rebuilt.

## Retained failures and concurrent work

- `gate-8cec6_a4`: resource observer yielded with IF clear while a probe needed
  BSP timer progress. The snapshot observer now uses lock-free scheduler waits.
- `gate-a96cbu1d`: the baseline assumed an unprivileged `ifconfig` command after
  concurrent migration to /sbin. The explicit fixture path fixes the observation.
- `gate-te2_niib`: 12/12 guest cases passed; source guard FAIL during ongoing
  implementation and unrelated concurrent ifup/test updates. Not a target PASS.
- `gate-m5rp07vt`, `gate-texpv6fu`: 11 guest cases passed before the final UEFI/8
  reboot's background-launch PID observation failed. The runner now waits for
  the real launch marker; per-call UART logging became aggregate entry evidence.
- `host-ebqq_fl1`, `host-7zye2197`: clock injection raced ahead of/behind the
  intended queued operation. An explicit parked-owner/queued-request rendezvous
  fixes the host fixture. Original failed assertions and diagnostics remain.
- `initial-host.log`: Ethernet adapter initially counted the new owner startup
  notification as a timer wake. The adapter now distinguishes publication.
- `regressions.log`: exploratory traffic across a changed local IP FAIL. Existing
  IPv4/TCP receive filters follow the configured address even though connection
  tuples remain unchanged. This is a pre-existing policy limitation, not fixed
  by Phase 2A; tuple preservation and traffic after restoration are separate gates.

The final original target's changed files were AGENTS.md, COMMANDS.md,
docs/roadmap/README.md, docs/subsystems/net.md and docs/subsystems/shell.md.
Those concurrent Markdown updates were reviewed and preserved, not restored from
the snapshot. The old snapshot and its source hashes remain authoritative.
The subsequent runner records concurrent nonproduction changes as a qualification
and still rejects drift in executable or relevant fixture inputs. Historical FAIL
manifests are never relabeled. Exact-count assertions were independently applied
to the saved final cases before being added to the runner.

The legacy installed probe now expects positive control plus unchanged AP socket
rejection, and fresh baseline snapshots force genuine AP launch. The Phase 1
guest snapshot disables the production consumer and uses a read-only baseline
query so its inert owner cannot compete for the same pool. Focused BIOS SMP=1/4
inert runs pass; that composite source guard still FAILs for concurrent unrelated
`tests/free_host.c` / `scripts/test_stream_tools_host.py` updates, which are retained.
The prior accepted six-configuration Phase 1 campaign remains historical evidence.

The updated installed baseline also passes **8/8 BIOS/UEFI × SMP=1/4 × two
boots**, using the existing instrumented snapshot and fresh disposable disks:
`build/net-installed/gate-nktuep9l/manifest.json`. This covers positive AP IFGET
alongside socket rejection, pinned ordinary commands, persistence and Linux
filesystem audits. Its HTTP check is a response substring, not a new packet or
exact-download audit. `gate-vktg2iu2` retains the earlier baseline FAIL because
the unprivileged fixture also needed the explicit `/sbin/ifup` path.

`build/network-smp-phase2a/runner-validation.json` verifies the revised source
classification: executable/relevant-fixture changes reject acceptance; Markdown
and unrelated test changes are recorded without overwriting either version.
No guest rerun is claimed for this preservation/exact-count assertion refinement;
the saved final 12 cases independently satisfy the new exact-count oracle.

## Remaining scope

Phase 2B needs checked file/endpoint references, generation/reuse fencing,
copied UDP payload storage, shared-descriptor lifetime and close/death cleanup
before its guards can be removed. Phase 2C must replace the shared TCP receive
scratch and preserve send/receive/deadline/SIGPIPE transactions. Phase 2D needs
accept reservations, copy pins, commit/abort and adoption rollback. All must fit
the static budget and keep one protocol owner. Production spawn pinning remains
until the separate rollout gate.

No new e1000e, packet-capture, link-flap guest matrix, measured idle CPU result,
caught-signal handler-frame, typed wake-before-context-save rendezvous or physical
acceptance is claimed. Existing actual-code link/timer/worker adapters pass;
real gateway replies, probe deadlines and remote wakes establish guest progress.
The earlier Phase 1 forced scheduler races remain separately recorded evidence.
An unresponsive owner has no safe restart: claimed storage remains retained.

SHA-256 boot-secret/tuple/TSC ISNs and zero reboot quiet period are preserved.
Hardware entropy assurance remains conditional; neither the unchanged startup
banner nor the distinct-ISN host assertion is security evidence.

Reproduce in WSL Ubuntu-24.04 with `make test-network-smp-phase2a-host` and
`python3 scripts/test_network_smp_phase2a.py` (or the combined make target).
The qualification of the recorded original target above remains part of the result.
