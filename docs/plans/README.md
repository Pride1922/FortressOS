# FortressOS — Plans & Design Documents

This directory archives architecture designs and staged implementation plans. For historical verification logs and hardware acceptance evidence, see [`docs/roadmap/`](../roadmap/README.md). For current binding invariants and implementation status, see [`AGENTS.md`](../../AGENTS.md).

## Archived Plans & Designs

| Document | Scope | Status |
| --- | --- | --- |
| [SMP_DESIGN.md](SMP_DESIGN.md) | Architectural specification for multi-core (SMP) support: 6 sequenced pieces and binding invariants `SM1`–`SM16`. | **COMPLETE** & verified on Dell 5590 hardware |
| [smp-piece6-plan.md](smp-piece6-plan.md) | Detailed implementation plan for Piece 6 (PMM/VMM multi-core memory architecture: 6A, 6B, 6C, 6D). | **COMPLETE** & verified on Dell 5590 hardware |
| [PHASE_9G4_PLAN.md](PHASE_9G4_PLAN.md) | Staged plan for Phase 9G.4 USB writable persistence and durability tiers. | **COMPLETE** & verified on Dell 5590 hardware |
| [PHASE5_HARDENING.md](PHASE5_HARDENING.md) | Plan for hardening early Phase 5 memory, locks, and test fixtures. | **COMPLETE** |
