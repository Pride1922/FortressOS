# FortressOS — Protected Contracts

Read this file first, every session, regardless of task scope. It's small on
purpose — cheap enough to always include, even for a one-file fix.

If your change touches any item below, **stop and discuss before
implementing**, unless the current task already explicitly authorizes it.
Routine edits that *preserve* the behavior described need no extra approval —
the goal is preserving the behavior, not preserving specific lines of code.
Full invariant IDs, evidence, and "how to check" detail live in AGENTS.md §4
and §9 and ARCH_REVIEW.md; this file is the boundary list, not the mechanism.

## Do not touch without discussion

- Limine request markers and linker `KEEP` placement.
- Boot/entry stack alignment (System V ABI, 16-byte before every `call`).
- Interrupt-frame layout (register preservation order in `interrupts.asm`
  and `syscall_entry.asm`).
- Syscall transition windows (no stack writes before the RSP switch;
  canonical RIP/RSP validation before SYSRET).
- Dependency ordering of subsystem init in `kmain`, including the two-stage
  PMM/VMM ordering: `pmm_init` caps allocation at 1 GiB → `vmm_init` builds
  the kernel PML4 with the full HHDM and switches CR3 →
  `pmm_unlock_high_memory()` lifts the cap. No allocation may reach above
  Limine's HHDM coverage until the kernel PML4 is active and CR3 points at it.
- Lock ranks, the no-lock-across-`switch_context` rule, IRQ-excluded
  sleep/wakeup, and CR3/stack ownership.
- Evidence-backed hardware workarounds (including any `empirical: Dell`
  comment) — read the cited evidence before changing or removing one.
- `ENABLE_*` raw-write gates, disposable fixture separation, hardware
  storage exclusions, and DMA quarantine.
- Shared kernel PML4 ownership, boot-module backing lifetime, and the
  current single-CPU assumptions (SMP pieces 1–6 are complete (see
  `docs/roadmap/smp-*.md`). The current binding assumption is that the
  network worker remains BSP-pinned and ordinary VFS commands are unpinned;
  LISTEN/ACCEPT use the same registered owner from every application CPU;
  explicit benchmark workers retain their requested CPU placement. Phase 3
  rollout verification is recorded separately from physical acceptance).
- xHCI DMA ring state, BOT completion polling discipline (no sleeps or
  IRQ-enables under the ext2 lock), DMA quarantine on failure, and USB class
  filtering (only 0x08/0x06/0x50 accepted as BOT mass storage).

## Networking contracts (established in NET-1 through NET-4; see docs/subsystems/net.md)

- **Sole BSP protocol owner.** All NIC I/O, timer sweeps, and TCP state changes
  run on CPU 0. Application callers on any CPU may publish copied IFGET,
  authorized IFSET, ping and traceroute requests to the registered CPU-0
  worker, plus UDP create/bind/send/receive and detach requests (including UDP
  DNS queries). UDP callers on every CPU retain checked file references and copy
  through bounded pinned staging; only the registered owner mutates UDP state or
  commits consumption. Final UDP close/death publishes cancellation/detach without
  protocol work. TCP client/stream CREATE/BIND/CONNECT/SEND/RECV, timed calls,
  READ/WRITE and SHUT_WR use that same owner from BSP and AP, sharing eight
  bounded pinned 64 KiB credits with UDP. AP code must not mutate connections or
  perform NIC work. LISTEN/ACCEPT use retained owner transactions on BSP and AP.
  ACCEPT reserves an existing child, caller-installs a provisional fd, then
  owner-commits or aborts with a retained rollback reference until acknowledgement.
  Pinned decisions cannot expire; all final socket closes only publish detach.
  Ordinary VFS children allow all CPUs from publication; explicit benchmark
  placement is preserved. Staged-list exclusion is disjoint from scheduler,
  process and filesystem locks; detach before publication or cleanup.
- **Polling-only ingress.** No NIC interrupt handlers or MSI vectors. Adding one
  is a contract change, not an optimization.
- **Rank-1 network locks.** `g_net_dev_lock`, `g_socket_table_lock`,
  `g_tcp_endpoints_lock`, `g_net_stack_lock`, the ping mailbox lock, and the
  socket-manager mailbox lock, generic `handoff` and `udp_handoff_values`
  locks are all Rank 1. They never nest with each other
  or with any other Rank-1 lock.
- **Bounded static state.** 16 socket handles, 8 TCP connections,
  4-datagram-per-socket RX queues, one wake hint per endpoint, bounded work
  credits per transaction. No dynamic allocation on hot paths.
- **Absolute per-call I/O deadlines.** `SYS_SEND_UNTIL`/`SYS_RECV_UNTIL`/
  `SYS_CONNECT_UNTIL` take a by-value uint64_t BSP-tick deadline. No
  socket-wide timeout option.
- **Existing TCP ISN and reboot policy.** Preserve the SHA-256 boot-secret /
  tuple / TSC ISN implementation introduced by `056a12d`, with no reboot
  quiet period. Hardware entropy assurance is conditional on all four random
  samples succeeding; hashing fallback inputs does not establish cryptographic
  assurance. The unconditional startup banner is not evidence of assurance.
  Documentation reconciliation authorized 2026-10-14; this does not authorize
  changing production TCP behavior or claim an independent security audit.
- **NIC DMA quarantine.** Same shape as the storage quarantine. Uncertain
  controller ownership means terminal FAILED, never reallocation.
- **Metadata cache coherence.** The ext4 metadata cache invalidates affected
  entries before any write and republishes only after the durability
  barriers succeed. This is a filesystem contract, noted here because the
  network and storage paths share the bounded-state discipline.

## Lock ranks (quick reference)

Acquire in increasing rank order; release LIFO; never hold a spinlock across
`switch_context`.

| Rank | Lock |
| --- | --- |
| 1 | per-CPU scheduler locks **or** `ext2_lock` **or** `g_process_lock` (global process/child metadata) **or** network locks (`g_net_dev_lock`, `g_socket_table_lock`, `g_tcp_endpoints_lock`, `g_net_stack_lock`, the ping mailbox lock, the socket-manager mailbox lock, `handoff`, `udp_handoff_values`; all ordinary lock kind). Process/ext2 cannot nest with any rank-1 lock in either order; only scheduler pairs in increasing address order via `sched_lock_pair` are exempt. Network Rank-1 locks follow the same rule: they never nest with each other or with any other Rank-1 lock. |
| 2 | heap |
| 3 | VMM |
| 4 | PMM |
| 5 | console |

Full rationale and violation consequences: AGENTS.md §4 (L1–L4).

Phase 3's `staged_lock` is also ordinary Rank 1, disjoint from every other lock.
It owns only never-run list membership/TCBs. Detach before process metadata,
descriptor/VM cleanup or scheduler publication; never hold it across a switch.

## Where the detail lives

- **Why** a contract exists, evidence labels (H1–H13), and full invariant
  tables: `AGENTS.md` §4, §8, §9 and [`docs/subsystems/`](docs/subsystems/README.md) annexes.
- **Deferred work and known limits** on top of these contracts:
  `ARCH_REVIEW.md`.
- **Historical evidence** for a specific hardware workaround: the relevant
  file under `docs/roadmap/`.

## Phase 2C update (2026-10-16, verified; preservation qualified)

Client CREATE/BIND/CONNECT/SEND/RECV, timed variants, socket READ/WRITE and SHUT_WR now use retained copied requests through the registered CPU-0 owner from every application CPU, including BSP. This supersedes the client-continuation/guard wording above. LISTEN/ACCEPT retain BSP/affinity guards and existing adoption until Phase 2D. Final close only publishes detach. Production pinning, polling-only ingress, ranks, bounded pools and ISN policy remain.

Phase 2C network behavior is verified: 12 e1000e matrix boots, 4 current-checkout e1000 boots and 4 carrier-recovery boots pass. The full-matrix repository-preservation guard remains qualified for concurrent unrelated user-command edits, with byte-identical network executable-section reconciliation; its original FAIL is retained. [Audit, exact results and preservation qualification](docs/roadmap/network-smp-phase2c.md). No physical acceptance is claimed.


## Phase 2D update (2026-10-16)

LISTEN/ACCEPT now use the registered CPU-0 owner from every application CPU.
ACCEPT reserves the existing child generation, retains it in the listener queue,
and defers allocation, output validation and provisional descriptor insertion to
the caller. The owner commits or aborts before rollback references are released.
Reservations are exclusive; leases release only unpinned offers, with no public
ACCEPT timeout. Published decisions receive bounded acknowledgement-only waits.
Final close publishes detach; the owner fences common-slot reuse through cleanup.
Production spawn pinning and Phase 3 rollout are unchanged. No physical acceptance
is claimed. Earlier Phase 2C preservation qualifications remain historical evidence.
See [Phase 2D audit](docs/roadmap/network-smp-phase2d.md) for implementation proof, exact artifacts, tests and retained failures.
