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
  network worker and all socket syscalls remain BSP-pinned; cross-core
  socket access is deferred).
- xHCI DMA ring state, BOT completion polling discipline (no sleeps or
  IRQ-enables under the ext2 lock), DMA quarantine on failure, and USB class
  filtering (only 0x08/0x06/0x50 accepted as BOT mass storage).

## Networking contracts (established in NET-1 and NET-2; see docs/subsystems/net.md)

- Sole BSP protocol owner: all NIC I/O, timer sweeps, and TCP state changes
  run on CPU 0. AP code may publish detach requests but must not mutate
  connections or perform NIC work.
- Polling-only ingress: no NIC interrupt handlers or MSI vectors. Adding one
  is a contract change, not an optimization.
- Rank-1 network locks (g_net_dev_lock, g_socket_table_lock,
  g_tcp_endpoints_lock, the ping mailbox lock, the socket manager lock):
  never nest with each other or with any other Rank-1 lock.
- Bounded static state: 16 socket handles, 8 TCP connections,
  4-datagram-per-socket RX queues, one wake hint per endpoint. No dynamic
  allocation on hot paths.
- Absolute per-call I/O deadlines; no socket-wide timeout option.
- 120-second TCP reboot quiet period. ISN generation is deliberately
  non-cryptographic; the quiet period is the mitigation.
- NIC DMA quarantine: same shape as the storage quarantine. Uncertain
  controller ownership means terminal FAILED, never reallocation.

## Lock ranks (quick reference)

Acquire in increasing rank order; release LIFO; never hold a spinlock across
`switch_context`.

| Rank | Lock |
| --- | --- |
| 1 | per-CPU scheduler locks **or** `ext2_lock` **or** `g_process_lock` **or** `g_net_dev_lock` **or** `g_socket_table_lock` **or** `g_tcp_endpoints_lock` **or** the ping/socket-manager mailbox locks (all ordinary lock kind). Process/ext2 cannot nest with any rank-1 lock in either order; only scheduler pairs in increasing address order via `sched_lock_pair` are exempt. Network Rank-1 locks follow the same rule: they never nest with each other or with any other Rank-1 lock. |
| 2 | heap |
| 3 | VMM |
| 4 | PMM |
| 5 | console |

Full rationale and violation consequences: AGENTS.md §4 (L1–L4).

## Where the detail lives

- **Why** a contract exists, evidence labels (H1–H13), and full invariant
  tables: `AGENTS.md` §4, §8, §9 and [`docs/subsystems/`](docs/subsystems/README.md) annexes.
- **Deferred work and known limits** on top of these contracts:
  `ARCH_REVIEW.md`.
- **Historical evidence** for a specific hardware workaround: the relevant
  file under `docs/roadmap/`.
