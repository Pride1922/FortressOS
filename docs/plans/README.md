# FortressOS — Plans & Design Documents

This directory archives architecture designs and staged implementation plans. For historical verification logs and hardware acceptance evidence, see [`docs/roadmap/`](../roadmap/README.md). For current binding invariants and implementation status, see [`AGENTS.md`](../../AGENTS.md) and [`docs/subsystems/`](../subsystems/README.md).

## Plans & Designs

| Document | Scope | Status |
| --- | --- | --- |
| [DOWNLOAD_TOOLS_PLAN.md](DOWNLOAD_TOOLS_PLAN.md) | Streaming checksums, bounded USTAR extraction, finite ICMP traceroute and separate wget Dell acceptance. | **DRAFT FOR USER REVIEW** |
| [NET2_PLAN.md](NET2_PLAN.md) | Seven steps: TCP design/codec, simulator, client, server, finite nc, physical gate and userspace DNS. | **7/7 COMPLETE**; Dell 5590 physical DNS user-confirmed PASS |
| [NET2_STEP7_DNS.md](NET2_STEP7_DNS.md) | Userspace DNS, nslookup, hostname nc, bounded fallback and acceptance gates. | **COMPLETE**; host/QEMU evidence and user-confirmed physical acceptance |
| [TCP_IO_DEADLINE.md](TCP_IO_DEADLINE.md) | Opt-in absolute TCP connect/send/receive deadlines, concrete syscall ABI and non-owning wake-hint proof. | **APPROVED + IMPLEMENTED**; host/live evidence in Step 7 |
| [DNS_USER_API.md](DNS_USER_API.md) | Shared resolver layout, ownership, limits, statuses and transaction contract. | **APPROVED + IMPLEMENTED** |
| [TCP_TRANSPORT_DESIGN.md](TCP_TRANSPORT_DESIGN.md) | Measured layout, bounded transport/OOO/retransmission/TIME_WAIT ownership and simulator contract. | **IMPLEMENTED**, pure engine |
| [TCP_SOCKET_ABI.md](TCP_SOCKET_ABI.md) | Client/listener stream ABI/lifetime semantics; existing UDP and SYS_NETCTL preserved. | **STEPS 3–4 ABI IMPLEMENTED** |
| [TCP_WAIT_LIFECYCLE_PROOF.md](TCP_WAIT_LIFECYCLE_PROOF.md) | Reservation-free waits, seven-property ACCEPT argument and runtime evidence. | **CLIENT/ACCEPT DESIGN REVIEWED + VERIFIED** |
| [NET2_STEP4_ADDENDUM.md](NET2_STEP4_ADDENDUM.md) | Client fence, bounded listener and adoption/rollback pre-coding contract. | **IMPLEMENTED**; historical gate record |
| [NET2_STEP5_SOCKET_FIXTURE.md](NET2_STEP5_SOCKET_FIXTURE.md) | Synthetic TCP peer roles, bounded wire state/faults, golden reference, independent audit and artifact retention. | **IMPLEMENTED**; executed evidence in Step 5 report |
| [UDP_SOCKET_ABI.md](UDP_SOCKET_ABI.md) | Phase 5 socket/address layout, register arguments, datagram semantics and deterministic error precedence; SYS_NETCTL unchanged. | **IMPLEMENTED**; BSP-only |
| [NET_PHASE5_PLAN.md](NET_PHASE5_PLAN.md) | Bounded UDP sockets, concrete ABI, worker ownership, fd lifecycle, QEMU and physical LAN gates. | **5a IMPLEMENTED + VERIFIED**; 5590 UDP user-reported PASS; capture audit pending |
| [NETCTL_PING_ABI.md](NETCTL_PING_ABI.md) | Concrete 48-byte layout, field offsets, errors and finite owner-exit fallback. | **IMPLEMENTED** in Phase 4a; BSP-only |
| [NET_PHASE4_PLAN.md](NET_PHASE4_PLAN.md) | IPv4/ICMP codecs and worker delivery, narrow Ring 3 ping ABI, QEMU gates and separate physical Dell acceptance. | **4a IMPLEMENTED + 4b physically accepted** (2026-10-01, manual) |
| [SHELL_DESIGN.md](SHELL_DESIGN.md) | Shell architecture and staged delivery: editing/history, terminal support, cwd/completion, environment, redirection/pipes, jobs and scripting. | **S0–S2 IMPLEMENTED** (QEMU verified, Dell pending); S3–S10 planned |
| [SMP_DESIGN.md](SMP_DESIGN.md) | Architectural specification for multi-core (SMP) support: 6 sequenced pieces and binding invariants `SM1`–`SM16`. | **COMPLETE** & verified on Dell 5590 hardware |
| [smp-piece6-plan.md](smp-piece6-plan.md) | Detailed implementation plan for Piece 6 (PMM/VMM multi-core memory architecture: 6A, 6B, 6C, 6D). | **COMPLETE** & verified on Dell 5590 hardware |
| [S7_PLAN.md](S7_PLAN.md) | Staged implementation plan & acceptance checklist for Milestone S7 (Pipes and stream utilities). | **PHASES 1–4 AND 5A COMPLETE**; Phase 5B next |
| [S7_PHASE4.md](S7_PHASE4.md) | BSP-only external pipeline executor: loop shape, launch/wait separation, fd wiring, cleanup, error mapping and acceptance gates. | **COMPLETE**; user acceptance 2026-09-27 |
| [S7_PHASE5.md](S7_PHASE5.md) | Stream utility behavior, bounded runtime/tail storage, cat/view migration, packaging and acceptance. | **5A COMPLETE**, user accepted 2026-09-27 |
| [S7_PHASE5B.md](S7_PHASE5B.md) | Selected builtins in child pipeline stages: explicit allowlist, shared implementations, spawn bounds, output errors and acceptance. | **PLANNED**, not implemented |
| [PHASE_9G4_PLAN.md](PHASE_9G4_PLAN.md) | Staged plan for Phase 9G.4 USB writable persistence and durability tiers. | **COMPLETE** & verified on Dell 5590 hardware |
| [PHASE5_HARDENING.md](PHASE5_HARDENING.md) | Plan for hardening early Phase 5 memory, locks, and test fixtures. | **COMPLETE** |
