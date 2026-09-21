# FortressOS — SMP (Multi-Core) Design

Status: planned, not started. The kernel is currently single-CPU by design
(see `PROTECTED.md` and AGENTS.md §9); every invariant below is binding
*once its piece lands*, not yet enforced in the current codebase.

This document sequences multi-core support as six pieces, each a stated
prerequisite for the next. `SM` IDs are binding invariants for their piece,
in the same style as AGENTS.md §4 (L1–L4, S1–S4, I1–I3, M1–M4) — read them
before implementing or reviewing that piece.

## 1. AP discovery

Enumerate and identify application processors before anything else in this
plan can start.

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM1 | ACPI MADT is the sole source of truth for core count and APIC IDs. Never assume a fixed or detected-at-build core count. | Trace `madt_parse` → AP list; reject a missing/malformed MADT rather than falling back to a guessed count. |
| SM2 | INIT-SIPI-SIPI sequencing follows Intel/AMD-documented timing (10ms INIT deassert delay, 200µs between SIPIs). Trampoline code lives in identity-mapped, sub-1MiB memory reclaimed once all APs report in. | Verify trampoline placement against Limine's memory map before use; confirm the reclaim happens only after every expected AP has signaled ready, not on a timeout alone. |

## 2. Per-CPU storage

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

| ID | Binding invariant | How to check |
| --- | --- | --- |
| SM10 | Lock ranks L1–L4 gain an explicit cross-CPU exception list; the current single-CPU assumption embedded in L2/L3 (`spin_debug_assert_unheld`, bootstrap-CPU-only tracking) must be re-derived for N CPUs, not deleted. | Every current "bootstrap-CPU-only" comment or check in spinlock code must have a stated replacement before this piece is considered done. |
| SM11 | Spinlocks use true cross-core atomic test-and-set/exchange, not IRQ-disable alone; rank-ordering enforcement (`SPINLOCK_RANKED`) becomes cross-CPU aware. | Confirm the lock implementation uses `lock`-prefixed atomics; run a two-CPU contention test and verify rank-violation detection still panics correctly when two different CPUs violate ordering against each other. |

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
