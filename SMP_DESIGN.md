# FortressOS — SMP (Multi-Core) Design

Status: Piece 1 (AP discovery) verified in QEMU and on Dell 5590
(2026-09-21). Piece 2 (per-CPU storage) implemented and verified in QEMU and
on Dell 5590 (UEFI, 8 CPUs, 2026-09-23); post-boot shell and storage verified.
Evidence is tracked in [the Piece 2 evidence](docs/roadmap/smp-piece2-percpu.md).
Pieces 3–5 are complete with recorded QEMU and Dell evidence; see
[Piece 5](docs/roadmap/smp-piece5-ipi.md). APs now schedule kernel work.
Piece 6A boot memory readiness is implemented, pending user-run verification;
6B–6D allocator/translation/lifetime work remains. See
[Piece 6 handoff](docs/roadmap/smp-piece6-memory.md) and its approved plan.
Earlier piece descriptions below retain their stage-specific scope.

This document sequences multi-core support as six pieces, each a stated
prerequisite for the next. `SM` IDs are binding invariants for their piece,
in the same style as AGENTS.md §4 (L1–L4, S1–S4, I1–I3, M1–M4) — read them
before implementing or reviewing that piece.

## Workflow

Per piece, in order:

1. Implementation is written against that piece's `SM` invariants, with the
   relevant `make test-*` target added or pointed to.
2. Verification (QEMU SMP, and Dell hardware where the piece needs real
   timing/hardware — e.g. the AP trampoline or APIC behavior) is run
   separately, not by the same pass that wrote the code.
3. Results are reported back — pass, or what failed — before the next
   piece starts. A piece isn't "done" because it compiles; it's done when
   its verification evidence exists, per the existing evidence-tied-to-claims
   rule (AGENTS.md §2).

No piece is implemented without an explicit go-ahead for that specific
piece.

## 1. AP discovery

Status: **verified (QEMU and Dell 5590, 2026-09-21).** See the recorded
[evidence table](docs/roadmap/smp-piece1-ap-discovery.md#evidence).

Enumerate and identify application processors before anything else in this
plan can start.

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM1 | ACPI MADT is the sole source of truth for core count and APIC IDs. Never assume a fixed or detected-at-build core count. | Trace `madt_parse` → AP list; reject a missing/malformed MADT rather than falling back to a guessed count. |
| SM2 | AP bring-up uses Limine's SMP boot protocol (`goto_address` handoff), not a hand-rolled INIT-SIPI-SIPI trampoline — Limine already performs that sequence, with documented timing, before `kmain()` runs, and reimplementing it would duplicate logic the bootloader has to get right anyway. Every Limine-reported LAPIC ID is cross-checked against MADT's enabled set (SM1) before being started. | Confirm `smp_init()` refuses to start any CPU Limine reports that MADT didn't enumerate as enabled; confirm every enumerated AP reports online before the boot checkpoint passes, not on a timeout alone. |

## 2. Per-CPU storage

Status: **verified in QEMU and on Dell 5590 (UEFI, 8 CPUs, 2026-09-23).** See
[implementation, verification and limits](docs/roadmap/smp-piece2-percpu.md).
The same TSS selector value (0x28) resolves through a distinct GDT on each
CPU; descriptor bases and active TSS objects are distinct (SM6).

Every CPU needs its own kernel state before it can run any code that reads
"current CPU" — this piece must land before scheduler or lock-discipline
changes (Pieces 3–4) can be CPU-aware.

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM3 | Every CPU has its own per-CPU struct reachable via GS base, installed before any code that reads current-CPU state runs on that CPU. | Confirm `wrmsr(IA32_GS_BASE, ...)` happens before the AP's first scheduler or interrupt-enabled instruction. |
| SM4 | Per-CPU fields (CPU id, current thread, preempt count, IRQ nesting depth) are never shared or aliased across CPUs — no global mutable scheduler state survives this piece. | Grep for the current single global `current_thread`-style symbols; each must become a per-CPU access, not a shared one. |
| SM5 | Each AP has its own kernel stack, double-fault (IST1) and NMI (IST2) stacks — never the BSP's. | Inspect per-CPU TSS/IST setup at AP bring-up; run a fault on a non-BSP CPU and confirm it does not corrupt or touch the BSP's IST region. |
| SM6 | Each AP has its own TSS and GDT selector; no CPU ever loads another CPU's active TSS. | Confirm `ltr` on each AP loads a CPU-local TSS descriptor, not a shared one. |
| SM7 | Heap/allocator calls made during AP bring-up respect the existing single-writer assumption (M-series, AGENTS.md §4) until Piece 6 makes PMM/VMM CPU-safe — AP init must not allocate concurrently with the BSP or another AP. | Serialize AP bring-up (one AP fully up before releasing the next) until Piece 6 lands; grep AP init path for any allocation call and confirm no overlap window. |
| SM8 | BSP-only init-time globals (boot_info parse, Limine response parsing) are finished and read-only before any AP is released. | Confirm AP release happens strictly after `kmain`'s boot-info parsing phase completes, not interleaved with it. |
| SM9 | `make test-nmi`'s exact-boundary NMI delivery verification (ARCH_REVIEW.md, "Implemented: exact-boundary NMI delivery verification") is BSP-only until this piece lands. Extending it per-CPU requires per-CPU IST2 stacks (SM5) and a per-CPU raw-UART path that stays lockless, matching the existing NMI discipline (I3, AGENTS.md §4). | Do not claim per-CPU NMI coverage until SM5 is in place and the extended test exercises a non-BSP CPU; the existing BSP-only result does not transfer. |

## 3. Lock discipline

The current lock ranks and single-CPU assumptions (L1–L4, AGENTS.md §4;
PROTECTED.md) must be re-derived for concurrent CPUs, not silently dropped.
Single-CPU global lock tracking (`held[16]` and `depth` in `spinlock.c`) is
migrated into `cpu_local_t`, making rank enforcement and recursion detection
independent per CPU.

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM10 | Lock tracking is per-CPU: each CPU tracks its own held stack in `cpu_local_t` (`held[16]`, `lock_depth`). The legacy single-CPU global tracker is completely removed. | Inspect `spinlock.c` and `percpu.h`; verify all tracking accesses resolve via `cpu_current()`. Ensure no global tracker state remains. |
| SM11 | Spinlocks use true cross-core atomic acquire/release (`__atomic_test_and_set` / `lock bts` with `pause` backoff), not IRQ-disable alone. Rank-ordering enforcement strictly prohibits acquiring equal or lower ranks on the same CPU, with an explicit exception for classified scheduler locks (SM11a). | Confirm atomics use bus-locked instructions; verify that two CPUs concurrently acquiring the same lock achieve mutual exclusion without missed updates. |
| SM11a | Classified lock types: `spinlock_t` gains a `kind` field (`LOCK_KIND_ORDINARY = 0`, `LOCK_KIND_SCHED = 1`). A CPU holding a scheduler lock may only acquire a second scheduler lock if addresses/CPUs are strictly ordered (ascending order), preventing work-stealing deadlocks. All other equal-rank or descending-rank acquisitions panic. | Confirm `SPINLOCK_RANKED` initializes `kind` and `can_acquire()` checks `kind` before permitting same-rank acquisition. |
| SM11b | Dedicated AP panic path: rank violations on an AP call `spin_panic_ap()`, which writes diagnostic state (AP id, attempted lock name/rank, and the local held stack) exclusively to raw UART (`serial_raw_puts`). It never touches console locks, dmesg buffers, or other CPUs' states, and halts only the offending AP (`cli; hlt`). | Induce a deliberate rank inversion on AP 1; confirm serial outputs the fatal diagnostic, AP 1 halts, and the BSP continues running without hanging or deadlocking. |
| SM11c | AP 1 test dispatch scoping: Piece 3 verification is restricted to BSP + AP 1. APs 2..N remain parked with `cli; hlt` and interrupts disabled, preserving Piece 2's verified state. AP 1 executes synchronous test routines during bring-up before parking. | Audit AP release logic; confirm APs 2..N do not spin or execute test routines and go straight to `cli; hlt`. |

### Implementation and Contracts

1. **Per-CPU Lock State:**
   - `cpu_local_t` in `percpu.h` embeds:
     - `spinlock_t *held_locks[16];`
     - `uint32_t lock_depth;`
     - `uint32_t lock_panic;`
   - Maximum nesting depth is bounded to 16. `record_acquire` contains a defensive guard: if `lock_depth >= 16`, it fails immediately rather than overflowing.

2. **Lock Classification (`kind`) & Multi-Scheduler Nesting:**
   - `spinlock_t` definition:
     ```c
     typedef struct {
         volatile uint32_t lock;
         uint8_t rank;
         uint8_t kind; /* LOCK_KIND_ORDINARY (0) or LOCK_KIND_SCHED (1) */
         const char *name;
         uint64_t acquire_count;
         uint64_t contention_count;
         uint64_t max_spin_iters;
     } spinlock_t;
     ```
   - Same-rank acquisition is rejected unless:
     `held[i]->kind == LOCK_KIND_SCHED && lock->kind == LOCK_KIND_SCHED && (uintptr_t)held[i] < (uintptr_t)lock`.

3. **Debugging Assertions & Diagnostics:**
   - `spin_debug_assert_unheld(void)`: Asserts `cpu_current()->lock_depth == 0` (used at context-switch sites).
   - `spin_debug_assert_held(spinlock_t *lock)`: Asserts `lock` is currently in `cpu_current()->held_locks[0 .. lock_depth-1]`.
   - On rank or reentrancy violation, the diagnostic prints:
     - CPU id
     - Attempted lock name and rank
     - Full chain of currently held locks (`held[0] -> held[1] -> ...`)
     - Reason for failure

4. **AP Test Execution Protocol:**
   - During boot when test mode is active, AP 1 completes its local setup and checks a test mailbox before final parking.
   - Synchronous coordination with BSP verifies:
     - High contention under true concurrent execution (concurrent increments to a shared counter under a test spinlock, asserting exact expected total).
     - Per-CPU lock tracker isolation (AP 1 holding rank 2 while BSP holds rank 1).
     - AP rank-inversion trap triggering `spin_panic_ap` without affecting BSP.
   - After testing completes, AP 1 enters `park: for (;;) __asm__ volatile("cli; hlt");`.
   - APs 2..N always jump directly to `park`.

5. **Compilation Policy:**
   - Lock discipline checks and assertions are **always-on** in all kernel builds. The overhead (~30 cycles on uncontended paths) is negligible compared to cache-line synchronization, and invariant validation remains essential for kernel stability.

6. **Contention Telemetry:**
   - `acquire_count`, `contention_count`, and `max_spin_iters` in `spinlock_t` provide empirical data on lock pressure.
   - A spin iteration high-water mark logs a warning if spinning exceeds a calibrated threshold (e.g. 1,000,000 iterations), aiding in early livelock detection.

### Explicitly Out of Scope for Piece 3

- **No IPIs or TLB shootdown** (Piece 5).
- **No multi-CPU scheduling or work-stealing execution** (Piece 4).
- **No concurrent PMM or VMM frame/page allocation** (Piece 6).
- **No unmasking of external interrupts on APs** (APs maintain IF=0).
- **No NUMA optimizations.**

### Verification and Acceptance Criteria

- **QEMU (BIOS & UEFI, 1, 4, 8 CPUs):**
  - Contention test: BSP and AP 1 concurrently perform 100,000 increments each on a shared counter under a test spinlock; verify counter strictly equals 200,000 with zero missed updates.
  - Tracker isolation: AP 1 holding rank 2 does not prevent BSP from acquiring rank 1 or rank 2.
  - Inverted rank test: AP 1 deliberately attempts rank 2 $\to$ rank 1 acquisition; verify `spin_panic_ap` emits fatal diagnostic to raw UART and halts AP 1, while BSP continues to shell.
  - Full regression pass over 6 test suites:
    1. `make test-smp-discovery` (Piece 1 MADT/Limine agreement)
    2. `make test-smp-percpu` (Piece 2 GS/TSS/stack isolation & AP parking)
    3. `make test-nmi` (exact syscall-boundary NMI delivery)
    4. `make test-usb-mount` (read-only mount policy and ext2 rank 1 locking)
    5. `make test-usb-persistence` (writable ext2 transactions and sync paths)
    6. `make test-shell` (interactive shell stability)
- **Bare-Metal Dell Latitude 5590 (UEFI, 8 CPUs):**
  - Boots to interactive shell with USB storage mounted, confirming lock discipline changes cause no regression on physical hardware.

## 4. Scheduler

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM12 | Run queues become per-CPU with explicit migration points; no implicit cross-CPU access to another CPU's thread state without holding that CPU's run-queue lock. | Grep scheduler code for any read of another CPU's run queue or `current_thread` that isn't behind an explicit lock acquire. |
| SM13 | Any load-balancing/work-stealing logic acquiring a second CPU's run-queue lock must respect L1 rank ordering (SM10) — never nest run-queue locks out of a fixed, documented order. | Trace every site that holds two run-queue locks simultaneously; confirm a fixed acquisition order (e.g., ascending CPU id) and test for deadlock under concurrent balancing. |

## 5. IPIs / TLB shootdown

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM14 | Any VMM operation that unmaps or changes protection on a page potentially cached in another CPU's TLB issues a synchronous IPI-based shootdown and waits for acknowledgment before returning. | Audit `vmm_unmap`/`vmm_protect`-style call sites; confirm none return to the caller before all targeted CPUs have acknowledged invalidation. |
| SM15 | IPI handlers follow the same ISR discipline as ordinary IRQ handlers (I1–I3, AGENTS.md §4): bounded work only, no allocation or blocking, with an explicit ack/completion protocol distinct from EOI. | Inspect the IPI handler body against I1's constraints; confirm the shootdown ack path is separate from `lapic_eoi()` (I2). |

## 6. PMM/VMM re-audit

The two-stage PMM cap/unlock sequence is protected (`PROTECTED.md`,
AGENTS.md §9) precisely because it assumes single-CPU boot ordering; this
piece re-derives it for concurrent APs rather than touching it piecemeal
in an earlier piece.

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM16 | The `pmm_init` (cap at 1 GiB) → `vmm_init` (build kernel PML4, switch CR3) → `pmm_unlock_high_memory()` sequence is re-derived for N CPUs starting concurrently, not just the BSP — until re-derived, APs must not allocate physical memory before the BSP completes this sequence (see SM7/SM8). | Confirm AP release (SM8) happens strictly after `pmm_unlock_high_memory()` on the BSP, until this piece explicitly changes that ordering with a documented replacement. |
| SM17 | The frame allocator (PMM) moves from BSP-serialized allocation to a lock-protected or per-CPU free-list design, consistent with lock rank 4 (L1). | Confirm concurrent `pmm_alloc`/`pmm_free` calls from two CPUs are race-free under a tool like TSan/host stress test before removing the SM7 serialization requirement. |
