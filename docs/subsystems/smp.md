# Multi-Core (SMP) Subsystem Annex

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS symmetric multiprocessing (SMP) subsystem. Binding contracts, locking rules (L1–L4, `SPINLOCK_RANKED`, `sched_lock_pair`), memory ownership (M1–M4), and protected concurrency invariants live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9). Architectural specifications and design invariants (`SM1`–`SM16`) live in [`docs/plans/SMP_DESIGN.md`](../plans/SMP_DESIGN.md).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **SMP (Pieces 1–6)** | **COMPLETE** (2026-09-25) | Pieces 1–6 all verified on QEMU (BIOS & UEFI, 1/4/8 CPUs) and bare-metal Dell 5590 (8 CPUs, 32 GiB). Includes per-CPU state, lock discipline, distributed scheduler with work-stealing, IPIs, contention-safe TLB shootdown, concurrent PMM safety (320k alloc/free under 623k+ contentions, 0 duplicate claims, exact baseline equality), and address-space lifetime tracking with deferred reaping. Full detail: [`docs/roadmap/smp-piece6-memory.md`](../roadmap/smp-piece6-memory.md). |

---

## 2. Hardware Facts and Verification Boundaries

### Open follow-up: pre-mount work-stealing startup timeout

During EXT4 Phase 9.5, one BIOS/2 KiB/SMP=4 recovery boot stopped at the
forced work-stealing startup selftest with `[FAIL] Timed out waiting for
stolen workers to execute!`, before USB discovery/mount and journal recovery.
The retry passed. Retained evidence is
`.codex-remote-attachments/ext4-phase9/guest-usb-fault-btljklz5/bios-2048-smp4-checkpoint-write-recovered.serial.log`
and the failed campaign image, also archived in `verification-1p643qew`.
See the [campaign report](../roadmap/ext4-phase9-5.md).

Track this separately for a future SMP/scheduler review: reproduce with the
same isolated ISO, QEMU TCG/SMP=4 and startup conditions; inspect worker
completion, runqueues, AP timer/preemption and timeout accounting. Determine
whether it reflects scheduler behavior, test timing or debugger/emulator
interaction. The cause and frequency are unconfirmed; the observed startup
failure remains open despite the successful retry. It is outside the EXT4
Phase 9 gate and provides no evidence of a journal or USB fault.

| ID | Evidence / constraint |
| --- | --- |
| H8 | **Recorded QEMU evidence:** 40 exact-boundary NMIs on IST2; no proof of physical NMI injection, nested-fault completeness, SWAPGS or SMP safety. (See `docs/plans/SMP_DESIGN.md` SM9 and `docs/roadmap/subsystems.md` for BSP-only boundaries). |

---

## 3. Physical Hardware Acceptance

- **Dell Latitude 5590 (8 logical CPUs, 32 GiB RAM):**
  - All 8 logical CPUs initialized and brought online into the distributed scheduler.
  - Concurrent PMM allocation stress tests verified under contention.
  - S5 clean shutdown and multi-core reboot verified.
- **Dell Latitude 5500 (8 logical CPUs, 8 GiB RAM):**
  - All 8 logical CPUs brought online via UEFI USB boot.
  - Monotonic 100 Hz BSP-driven timebase verified across CPUs (regression guard against multi-writer APIC scaling bug).
  - Ring 3 shell, `/bin/ps`, and `/bin/sysinfo` operational across cores.
- **Current SMP Execution Boundaries:**
  - Pipe and input peers remain BSP-pinned; cross-core wake channels remain future work.
  - Networking RX/TX worker remains BSP-pinned.

---

## 4. Test Targets and Verification Notes

| Target | Scope / evidence |
| --- | --- |
| `make test-smp-percpu` | BIOS/UEFI with 1/4/8 CPUs: CPU-local GS/GDT/TSS/stacks, AP #DF, real NMI delivery on every CPU, unchanged BSP IST/TSS/GDT and shell startup; snapshot NVMe fixture |
| `make test-pmm-boot-host` | Piece 6A host ASan/UBSan with single-threaded shims: boot ceiling, capped OOM, contiguous boundary, unlock gates and exact cleanup; no SMP exclusion claim |
| `make test-smp-memory-boot` | Piece 6A BIOS/UEFI 1/4/8 CPUs, 2 GiB: explicit test ISO, readiness/CR3, high-memory probe and AP startup; no data disks |
| `make test-vmm-host` | Piece 6D host ASan/UBSan: VMM space registry, lifecycle states, transient op_refs, context switch tracking, deferred destruction queue and drainage with zero leaks |
| `make test-smp-vmm` | Piece 6D BIOS/UEFI 1/4/8 CPUs: 100 user process spawn/exit cycles across cores, deferred destruction, table frame and page leak checks |
| `make test-smp-append` | True multi-core SMP concurrent append verification under QEMU (-smp 4, BIOS & UEFI): independent and shared handles, atomic EOF serialization under `ext2_lock`, 200 records intact, 0 loss/corruption, clean S5 shutdown, offline host `e2fsck -fn` audit. |
| `make test-nmi` | BIOS/UEFI, BSP: seven SYSRET boundaries × 4 rounds + kernel test-recovery + four sigreturn boundaries × 2 origins × 4 rounds; `build/nmi-*.json` and `.log`. Runner observes the post-IRET user boundary with a hardware breakpoint (`resume_to`) instead of single-stepping — see the [Phase 3 handoff](../roadmap/shell-s8-phase3.md). User-reported pass on three consecutive runs 2026-09-28. |

---

## 5. Architectural References

- Master Architecture & Invariants (`SM1`–`SM16`): [`docs/plans/SMP_DESIGN.md`](../plans/SMP_DESIGN.md)
- Piece 6 Memory & PMM/VMM Safety Evidence: [`docs/roadmap/smp-piece6-memory.md`](../roadmap/smp-piece6-memory.md)
