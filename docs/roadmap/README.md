# FortressOS Roadmap — Index

This directory replaces the old monolithic `ROADMAP.md`. Each file below
covers one phase's implementation notes and verification evidence. Point an
agent at the specific phase file relevant to its task, not this whole
directory — that's the entire point of the split.

For binding implementation rules, task routing, and current hardware
evidence, read `AGENTS.md`. For the always-load guardrail list, read
`PROTECTED.md`. For audit limits and technical debt, read `ARCH_REVIEW.md`.
Code and public headers remain the implementation reference.

Historical statements in each phase file describe *that* checkpoint; later
phases supersede earlier deferred-work notes. They are not new instructions
and are not proof that every current revision has passed every test — check
`AGENTS.md`'s status table for current, up-to-date status per phase.

## Phase files

| File | Covers |
| --- | --- |
| [phase-9c2-readonly-ext2.md](phase-9c2-readonly-ext2.md) | Read-only ext2 mount at `/mnt` (superseded/extended by Phase 9D for writable support) |
| [phase-9d-writable-ext2.md](phase-9d-writable-ext2.md) | Bounded writable ext2: allocation/truncation ordering, superblock clean/dirty lifecycle, 3-boot persistence |
| [phase-9e-exec-and-files.md](phase-9e-exec-and-files.md) | Program execution (spawn/wait, ABI, exit status, chaining), directory ops (mkdir/rename/unlink), Bug H4 AZERTY fix |
| [phase-9g1-xhci-enumeration.md](phase-9g1-xhci-enumeration.md) | xHCI controller discovery, MMIO/reset, rings, ports, device addressing & descriptors (9G.1a–9G.1e) |
| [phase-9g2-usb-block.md](phase-9g2-usb-block.md) | Bulk-Only Transport, SCSI engine, block device registration, GPT discovery |
| [phase-9g3-usb-mount.md](phase-9g3-usb-mount.md) | Production `/mnt` mount, PARTUUID selection, cmdline parsing |
| [phase-9g4-usb-durability.md](phase-9g4-usb-durability.md) | Writable persistence, BOT stall recovery, durability classification |
| [phase-9g5-superspeed.md](phase-9g5-superspeed.md) | Multi-controller enumeration (9G.5a), SuperSpeed/USB 3.x (9G.5b) |
| [phase-9h-ram.md](phase-9h-ram.md) | 32 GiB RAM support, two-stage PMM/VMM init, HHDM coverage limit |
| [smp-piece1-ap-discovery.md](smp-piece1-ap-discovery.md) | SMP Piece 1: AP discovery via Limine's SMP protocol, cross-checked against ACPI MADT; implemented, verification pending |
| [subsystems.md](subsystems.md) | Cross-cutting notes not tied to one phase: NMI delivery, boot console, interactive shell/input, general Dell acceptance |

## Checkpoint sequence (for orientation only — not authoritative status)

```
[Phase 1–9C] Boot, memory, threads, syscalls, VFS, storage track (COMPLETE)
[Phase 9D]   Bounded writable ext2                         (COMPLETE)
[Phase 9E]   Program execution, ABI, exit status            (COMPLETE)
[Phase 9F]   Raw disk image packaging                       (COMPLETE)
[Phase 9G]   USB storage, through 9G.5b                     (COMPLETE)
[Phase 9G.5] USB topology expansion                         (9G.5a, 9G.5b complete; 9G.5c/d/e open)
[Phase 9H]   32 GiB RAM support                              (COMPLETE)
[SMP]        Multi-core support                              (Piece 1/6 implemented, unverified — see SMP_DESIGN.md)
[Following]  Accounts/permissions, then installer
```
