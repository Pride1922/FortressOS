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
  current single-CPU assumptions (SMP work is tracked separately in
  `SMP_DESIGN.md` — until a piece of that plan actually lands, treat the
  single-CPU assumption as still binding).
- xHCI DMA ring state, BOT completion polling discipline (no sleeps or
  IRQ-enables under the ext2 lock), DMA quarantine on failure, and USB class
  filtering (only 0x08/0x06/0x50 accepted as BOT mass storage).

## Lock ranks (quick reference)

Acquire in increasing rank order; release LIFO; never hold a spinlock across
`switch_context`.

| Rank | Lock |
| --- | --- |
| 1 | scheduler **or** ext2 (mutually exclusive — cannot nest with each other) |
| 2 | heap |
| 3 | VMM |
| 4 | PMM |
| 5 | console |

Full rationale and violation consequences: AGENTS.md §4 (L1–L4).

## Where the detail lives

- **Why** a contract exists, evidence labels (H1–H12), and full invariant
  tables: `AGENTS.md` §4, §8, §9.
- **Deferred work and known limits** on top of these contracts:
  `ARCH_REVIEW.md`.
- **Historical evidence** for a specific hardware workaround: the relevant
  file under `docs/roadmap/`.
