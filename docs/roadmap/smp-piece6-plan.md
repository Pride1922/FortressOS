# SMP Piece 6 — PMM/VMM implementation plan

Status: **DRAFT FOR USER REVIEW — no implementation authorized yet.**
Prepared 2026-09-24 against Phase 5 commit `ce6d4e1`.
The user will run builds/tests and report results; the implementation pass
will write code and verification tooling without claiming unrun checks pass.

## Goal and scope

Make concurrent physical allocation, page-table operations, and reclamation
safe across the existing SMP scheduler. Preserve SM14–SM17, lock ranks,
caller ownership of data frames, shared kernel tables, and boot metadata
lifetime. Keep the existing global PMM/VMM locks; per-CPU allocators, NUMA,
CPU hotplug, PCID, and general shared-process address spaces are out of scope.
Existing BSP-only process/input restrictions remain unless a specific memory
lifetime fix requires a narrow change.

## Findings from the current code

- PMM already uses a rank-4 lock for allocation/free and page-count getters.
  Byte-count getters, `pmm_audit`, and bootloader reclamation access mutable
  state without that lock. This is an audit and completion of synchronization,
  not an allocator replacement.
- The protected 1 GiB boot allocation cap and `pmm_unlock_high_memory()` are
  documented but absent from the current PMM source/header and boot sequence.
  Bitmap placement also needs to respect early mapped RAM.
- VMM already serializes most table operations at rank 3. Unmap drops that
  lock before shootdown, leaving mutation/reuse ordering to be established.
- Shootdown returns after an ACK timeout. Its mailbox contention loop only
  enables interrupts when entry IF was set; concurrent callers with IF clear
  can prevent each other's IPI completion. Other IRQ-disabled lock waiters
  also need analysis. A successful Phase 5 delivery test does not prove these
  contention/failure cases.
- Destruction reads other CPUs' `current_thread` pointers without scheduler
  synchronization, then flushes before taking the VMM lock. This does not
  prevent scheduling into the space between the check and reclamation.
- Higher-half invalidation is selected by root-pointer identity, although a
  shared kernel mapping may be changed through a process root.
- Spinlock telemetry increments occur before acquisition and can race.
- `SMP_DESIGN.md` still says Pieces 3–6 have not started. Historical Phase 5
  evidence should be retained while current status and limitations are fixed.

## Implementation sequence

### 6A. Restore and enforce boot memory readiness (SM16)

Files: `pmm.c/.h`, `vmm.c`, `main.c`, `smp.c/.h`.

1. Add an explicit early allocation ceiling of 1 GiB to every allocation
   entry point, including contiguous allocation and the above-address helper.
   Place the bitmap wholly inside usable early mapped RAM. Validate bounds
   and page alignment before marking usable regions.
2. Build the complete supported RAM HHDM, switch the BSP to the kernel CR3,
   verify readiness, then lift the allocation ceiling under the PMM lock.
   Distinguish free managed pages from pages currently eligible to allocate.
3. Publish readiness before AP release. Assert each AP installs the kernel
   CR3 before it can allocate. Keep serial AP bring-up: concurrent allocation
   after readiness does not require parallelizing the bootstrap protocol.
4. Keep bootloader reclamation disabled or boot-only until every retained
   Limine pointer/module backing dependency is accounted for. A lock alone
   does not authorize reclaiming live boot memory.

### 6B. Complete PMM synchronization and boundary handling (SM17)

Files: `pmm.c/.h`, narrowly scoped spinlock telemetry fixes.

1. Cover all mutable allocator state, metrics, audits and permitted reclaim
   paths with the existing rank-4 lock; use internal unlocked helpers to
   avoid recursive getter calls.
2. Validate contiguous free/allocation arithmetic and minimum-address
   rounding. Reject invalid ranges before partial mutation. Preserve frame
   zero, bitmap storage and reserved-region ownership.
3. Define audit/snapshot consistency: take the snapshot under lock; compare
   allocation sets only after workers stop and expected retained allocations
   are established. Report diagnostics after releasing the allocator lock.
4. Make lock counters race-free without changing ranks or IRQ semantics.

### 6C. Make shootdown completion safe under contention (SM14/SM15)

Files: `smp.c/.h`, `vmm.c`, relevant spinlock wait paths and per-CPU state.

1. Trace every caller, including heap growth, stack cleanup, reaping and
   rollback, recording locks and IF state. Cover both mailbox contention and
   a target spinning on a lock held by the initiator.
2. Proposed mechanism: publish a generation-tagged request with release/acquire
   ordering; share a bounded, lockless local invalidation/ACK routine between
   the IPI handler and explicitly supported IRQ-disabled wait loops. Polling
   services only TLB work and never enables IF or schedules. Handle same-CPU
   interrupt re-entry and delayed IPIs without acknowledging a newer request
   before its invalidation. Preserve dispatcher-owned EOI.
3. Document request serialization and lock order before coding it. Do not
   nest two ordinary rank-3 locks or silently weaken rank checks. Keep frame
   reuse and conflicting VMM mutations blocked until the transaction finishes.
4. Bound request acquisition, delivery and acknowledgment. Recommended failure
   policy: fatal stop on an incomplete barrier; never return success, recycle
   the mailbox, or reclaim frames after an unacknowledged shootdown.
5. Invalidate shared higher-half changes on every online CPU regardless of
   which root was passed. Audit full-flush/global-page behavior and all PTE
   permission changes, including intermediate entries. Keep conservative
   all-online targeting until a narrower scheme is proven.

This is the main protected synchronization design change proposed for review.
If call tracing disproves this mechanism, revise the plan before replacing it
with a different locking/interrupt model.

### 6D. Establish address-space and table lifetime

Files: `vmm.c/.h`, `thread.c/.h`, per-CPU state, ELF/stack callers as needed.

1. Add explicit address-space lifecycle tracking (live/dying, operation users,
   scheduler references and active/switching CPUs). Acquire a scheduler
   reference before publication and retain it across queued, running and
   switching states; release only after switching away. A dying space rejects
   new references. Keep bootstrap/kernel roots explicitly permanent.
2. Serialize entry eligibility with destruction, including all CR3 switch
   sites. No lock crosses `switch_context`; references bridge that interval.
   Raw table-pointer callers must hold a live operation/owner reference.
3. Destroy only an exclusively owned inactive space: validate structure,
   prevent new entrants, complete invalidation, then reclaim private tables
   and optionally exclusively owned leaves. Preserve refusal of active or
   kernel roots and never free shared higher-half tables.
4. Make unmap/mapping publication and barrier completion one ordered operation
   relative to conflicting mutations. The caller still owns an unmapped data
   frame and may reuse it only after successful completion.
5. Audit OOM rollback, huge-page rejection, parent permission changes, shared
   kernel PML4 provisioning and table counters. Roll back newly allocated
   empty tables without reclaiming reachable or shared tables.
6. Handle teardown errors at callers: the reaper must not discard ownership
   metadata after a failed destroy. Preserve user-range validity for the
   duration of access under the supported single-owner process model.

## Verification tooling to write; user executes

Proposed targets: `make test-smp-memory-host` and `make test-smp-memory`.

- Host: actual PMM code with explicit platform/lock shims, concurrent workers,
  allocation uniqueness, disjoint contiguous runs, cross-worker free, OOM,
  invalid ranges, boot ceiling/unlock, and exact post-quiescence allocation
  sets. Use TSan for races and a separate ASan/UBSan build for bounds; mocks
  do not establish hardware interrupt correctness.
- QEMU BIOS/UEFI, 1/4/8 CPUs: pinned allocation workers, concurrent VMM
  create/map/unmap/destroy, retained table accounting, and high-memory access
  after readiness. Single-CPU runs explicitly skip cross-core assertions.
- Real stale-translation checks: prime remote TLBs, change mappings, complete
  the barrier, and verify the new translation/access result. An ACK counter
  or PASS banner alone is insufficient.
- Deterministic contention cases: two IRQ-disabled initiators, a target
  waiting for an initiator-held allocator/VMM-related lock, delayed delivery,
  shared kernel mappings changed through a user root, and an attempted entry
  into a dying address space. Test timeout/fatal behavior in separate boots
  with bounded runners and explicit expected diagnostics.
- Check allocation sets only after workers and reapers quiesce. Distinguish
  expected permanent kernel tables from leaks. Use disposable/snapshot fixtures
  and paired OVMF images; no physical disk writes.
- Regression handoff: build, `test-smp-discovery`, `test-smp-percpu`,
  `test-smp-locks`, `test-smp-sched`, `test-smp-ipi`, `test-nmi`, `test-storage`,
  `test-ext2`, `test-ext2-write`, `test-shell`, `test-usb-mount`, and
  `test-usb-persistence`. Split focused checks from broader regressions in
  the handoff so failures can be attributed.
- Dell acceptance: 8 CPUs/32 GiB, allocation/readback above the early ceiling,
  repeated concurrent memory workers, shell/process lifecycle and selected
  USB storage. QEMU results do not establish this acceptance.

## Delivery and acceptance

Implement in reviewable commits following 6A–6D, with verification tooling
alongside the corresponding behavior. Update public contracts, SMP status,
architecture limitations, and a Piece 6 evidence document. Mark implementation
and verification separately; record only results the user actually supplies.

Acceptance requires race-free allocator evidence, correct high-memory boot
ordering, no reuse before complete shootdown, no re-entry into destroyed
spaces, exact ownership checks, and the user's reported regression/hardware
results. Approval of this plan includes the narrow boot readiness,
synchronization and address-space lifetime changes described above.
