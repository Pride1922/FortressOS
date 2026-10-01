# FortressOS — Subsystem Architecture & Status Annexes

This directory holds the per-subsystem architecture annexes, status logs, hardware facts, and scope boundaries for FortressOS. The binding contracts, locking disciplines, memory ownership models, coding standards, and protected invariants live centrally in [`AGENTS.md`](../../AGENTS.md) (§4 and §9). Each annex expands on its subsystem's current implementation status, verification evidence, hardware observations (H-series entries), test targets, and explicit out-of-scope boundaries.

## Annex Index

| Subsystem | Scope | Document |
| --- | --- | --- |
| **Networking** | Intel e1000/e1000e/I219-LM, DMA rings, Ethernet/ARP/IPv4/ICMP, Phase 5a BSP-only UDP sockets verified; 5590 UDP user-reported PASS; capture audit pending, ICMP 4b accepted | [`net.md`](net.md) |
| **USB** | xHCI controller, BOT mass storage, durability classification, explicit mount opt-in, Phase 9G handoff | [`usb.md`](usb.md) |
| **Storage & RAM** | ext2 writable/read-only mounts, GPT partitioning, NVMe exclusions, 32 GiB RAM / two-stage PMM | [`storage.md`](storage.md) |
| **Shell & Userland** | Shell S0–S9 (pipes, signals, jobs, variables, builtins, `/bin/top`, `/bin/ps`, `/bin/sysinfo`), input/layout | [`shell.md`](shell.md) |
| **Multi-Core (SMP)** | Distributed scheduler, work-stealing, IPI shootdown, per-CPU state, concurrent PMM/VMM safety | [`smp.md`](smp.md) |
| **Platform & Power** | ACPI S5 shutdown/reset, PCI ECAM, LAPIC NMI transitions, boot console diagnostics | [`platform.md`](platform.md) |
