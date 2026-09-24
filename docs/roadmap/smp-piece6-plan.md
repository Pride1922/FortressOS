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

### 6A. Add the missing documented boot memory invariant (SM16)

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

## Review revision — concrete decisions (2026-09-24)

These details refine the steps above and take precedence over their shorthand.
The plan remains pending approval; no implementation or test execution yet.

### Verified cap discrepancy

The requested command was run:

```text
rg -n "pmm_unlock_high_memory|PMM_BITMAP_CAPACITY_BYTES|g_alloc_ceiling|1 GiB|0x40000000" src/mm/pmm.c src/mm/pmm.h src/mm/vmm.c src/kernel/main.c
```

It finds the 32 GiB bitmap definitions and unrelated huge-page/probe/test
references, but no allocation ceiling or unlock function. A local-history
search, `git log --all -S pmm_unlock_high_memory -- src/mm/pmm.c src/mm/pmm.h src/kernel/main.c`,
finds no matching commits. Phase 9H records an unlock message and successful
high-memory probes; retain that evidence but mark the implementation mismatch.
The cause is unproven: this does not establish a revert or that the feature
was never implemented outside the available history. Step 6A adds the missing
documented invariant. The concrete hazard is allocation before kernel CR3
activation; this alone does not prove high-memory access after activation is
broken. The approximately 2.5 GiB boundary is a historical Dell observation,
not a guaranteed threshold on other boots or machines.

### PMM lock and latency decision

Hold the rank-4 IRQ-save lock across the ENTIRE bitmap scan and mutation.
There is no speculative or lock-free scan. Allocation is O(N) worst case,
with up to 8,388,608 frames at 32 GiB. The occupied-byte shortcut does not
eliminate the fragmented worst case. Contending CPUs also wait for earlier
holders; the existing spinlock provides neither fairness nor a hard latency
bound. Do not promise a microsecond bound without measurement. Add fragmented
bitmap cases and report maximum observed scan/lock duration separately from
correctness. The TLB polling integration must cover this IRQ-disabled wait
and any long critical section identified by the progress audit.

### Address-space record, synchronization and waiting

- Use a kernel-owned `vmm_space_t` per private root containing normalized CR3,
  LIVE/DYING state, owner/scheduler/operation reference counts, and an active
  or switching CPU mask. Allocate metadata before taking the VMM lock; publish
  in a registry under it. Remove before final reclamation and free metadata
  after unlocking. No rank-3-to-heap acquisition.
- Serialize EVERY state/count/mask access under the existing rank-3 VMM lock.
  LIVE check and reference increment are one critical section. Counts are
  lock-protected rather than lock-free atomics; an unlocked decrement or
  observation is forbidden. This provides atomic check/decrement semantics
  without a second equal-rank lock.
- A TCB takes a scheduler reference before publication and retains it while
  queued, blocked, running or switching. Switching away updates CPU residency
  after loading the new CR3, but does not drop a runnable TCB's reference.
  Terminated TCB references are released only after a switch-away handoff
  establishes that both the old CR3 and old stack are no longer in use.
  Never release that reference in `thread_exit` before the handoff. Cover
  first-entry and resumed-thread switch tails; enumerate all switch sites
  before implementation. References bridge the unlocked context switch.
- Retirement marks DYING under the VMM lock and prevents new references.
  Existing operation users may finish; queued references must be explicitly
  drained/cancelled, never forcibly decremented. Provide a separate retirement
  and try-reclaim path. Ordinary `vmm_destroy_pml4` continues to reject an
  active space without silently retiring it. The final owner remains until
  reclamation finishes.
- Try-reclaim checks DYING, zero non-owner references and an empty CPU mask
  under the same lock. If not ready, return BUSY immediately: no spinning or
  sleeping for reference release. The reaper retains ownership on a pending
  list and retries in later passes outside scheduler locks. There is no
  timeout that permits freeing. Test runners bound retirement time and fail
  with diagnostics; production retains a stuck space safely.
- The last `put` never frees tables inline. The reclaimer retains the DYING
  registry record exclusively across the final barrier and reclamation.
  No new lookup/reference can appear between eligibility and freeing. A raw
  root/table pointer alone is not a lifetime reference. API callers must hold
  an owner or operation reference for the entire access.

### Shootdown call-site proof required before 6C implementation

Write a call-site matrix covering every direct and transitive entry path:
boot/thread/ISR/exception context, IF state, held locks, target progress,
ownership through completion and failure behavior. Current direct inventory
includes VMM boot guards/destruction, heap rollback, thread stack rollback/free,
main diagnostics, AP guard setup and xHCI probe cleanup. There is no public
`vmm_protect` API today; include intermediate permission mutations and any new
protection API. This search inventory is not a completed transitive proof.

Ordinary IRQ/NMI handlers cannot initiate synchronous shootdowns; reject an
unsupported context before changing a PTE. Exception paths need their own
explicit proof. An unexpected lock-held caller must either move mutation and
barrier work outside the lock while retaining ownership, or demonstrate a
lockless TLB polling path on every dependent target. If neither is possible,
revise the design before implementation; never enable IF as an improvised fix.
The design permits proven lock-held polling, not a blanket claim that callers
hold no locks while waiting.

Polling reads a published shared request, invalidates ONLY this CPU's TLB,
then publishes its ACK. It does not drain an APIC queue, execute arbitrary
handlers, or send an EOI. The eventual hardware interrupt still gets its
dispatcher-owned EOI. Request fields remain immutable until all target ACKs
arrive. Acquire-load publication before fields; release-publish ACK after
invalidation. Each CPU tracks its completed generation. Polling excludes
local maskable-interrupt re-entry; NMI never calls it. A delayed old IPI may
service the current request, but only after actually invalidating that
generation. Both the initiator's wait and every dependent target path need
this progress proof. Failure to acquire/publish/complete within the bounded
protocol is fatal and never allows reuse or successful return.

### Concrete verification ownership and Dell acceptance

The implementation pass writes kernel checks, host harnesses, scripts and
Makefile targets. The user runs builds, host tests, QEMU and hardware checks.
Both review the supplied evidence: the user reports observed results; the
agent checks logs against each criterion and records gaps without inferring
passes. Piece 6 acceptance requires implementation AND verification evidence.
TSan and ASan/UBSan apply only to hosted builds. The freestanding kernel/QEMU
tests use explicit invariants and diagnostics, not those sanitizers.

Dell criteria (8 logical CPUs, 32 GiB), in an explicit memory-test mode:

1. Before readiness, exercise every allocator entry point and require all
   returned runs to end at or below `0x40000000`; high-only requests fail.
   After readiness, request a free usable frame at or above each of 2, 4, 16
   and 30 GiB using the above-address helper. Log actual addresses; firmware
   may reserve an exact threshold address. Verify every 64-bit word of each
   4 KiB page through HHDM with address-derived and complementary patterns,
   then free it. Ordinary allocation need not prefer high memory.
2. Run 32 workers pinned four per CPU, each completing 10,000 single-frame
   allocate/write/verify/free iterations: exactly 320,000 successes, zero OOM.
   Release from a start barrier and bound completion at 120 seconds. Use a
   preallocated atomic live-frame ownership table to detect duplicate live
   allocation; claim after allocating, verify patterns before free, and clear
   the test claim immediately before freeing the caller-owned frame. After
   worker exit and reaping, require exact allocation-bitmap and table-count
   equality with a baseline that accounts for permanent test infrastructure.
3. Complete 100 BSP process spawn/wait/exit cycles alongside AP memory workers.
   Require expected exit status every time and exact post-quiescence ownership
   baselines. Concurrent multi-CPU spawning remains out of scope: the current
   API explicitly requires BSP execution.
4. On an explicitly PARTUUID-selected RW-eligible USB, save the run's boot log
   to `/mnt/boot.log`, sync, shut down cleanly, power-cycle, and verify the
   complete saved prior-run log before overwriting it. Record device, GUID,
   durability class and actual mount mode. This establishes clean persistence,
   not sudden-power-loss durability; never force writable eligibility.

Record actual elapsed times and each assertion. A shell prompt or aggregate
allocation count alone is insufficient. QEMU results do not establish Dell
acceptance.
