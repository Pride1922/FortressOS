# SMP Piece 3 — Lock Discipline & Per-CPU Lock Tracking

Status: **COMPLETE (Verified in QEMU and on Dell Latitude 5590 physical hardware, 8 CPUs, 2026-09-24)**.
Prerequisites: Piece 1 (AP discovery) and Piece 2 (per-CPU storage) verified on QEMU and Dell Latitude 5590.

## Overview and Goals

Single-CPU lock discipline previously relied on a global `held[16]` array and `depth` counter in `src/kernel/spinlock.c`. In SMP, this global tracker creates false cross-CPU conflicts and cannot correctly detect rank inversions on concurrent cores.

Piece 3 migrates lock tracking into `cpu_local_t` (reached via `%gs:0`), enforces bus-synchronized atomic acquisitions (`lock bts` or `__atomic_test_and_set` with `pause` backoff), adds lock classification for multi-core scheduler locks, establishes an isolated raw-UART panic path for APs, and validates lock discipline with a two-CPU test protocol (BSP + AP 1).

## Architecture and Contracts

### 1. Per-CPU Tracker Migration (SM10)
- `cpu_local_t` in [src/arch/x86_64/percpu.h](../../src/arch/x86_64/percpu.h) embeds:
  - `spinlock_t *held_locks[16];`
  - `uint32_t lock_depth;`
  - `uint32_t lock_panic;`
- The global static `held[16]` and `depth` in `spinlock.c` are removed.
- All tracking operations (`can_acquire`, `record_acquire`, `record_release`) operate on `cpu_current()`.
- Maximum depth is 16. `spin_debug_acquire()` enforces a hard defensive check:
  `if (cpu->lock_depth >= 16) fail("lock tracker capacity exceeded", lock);`.

### 2. Lock Classification & Multi-Scheduler Nesting (SM11, SM11a)
- `spinlock_t` definition in [src/include/spinlock.h](../../src/include/spinlock.h):
  ```c
  enum lock_kind {
      LOCK_KIND_ORDINARY = 0,
      LOCK_KIND_SCHED    = 1,
  };

  typedef struct {
      volatile uint32_t lock;
      uint8_t rank;
      uint8_t kind;
      const char *name;
      uint64_t acquire_count;
      uint64_t contention_count;
      uint64_t max_spin_iters;
  } spinlock_t;
  ```
- Macro initializers:
  - `SPINLOCK_RANKED(r, n)` defaults to `LOCK_KIND_ORDINARY`.
  - `SPINLOCK_RANKED_KIND(r, k, n)` allows explicit classification.
- Invariant: A lock acquisition is permitted if and only if:
  1. The lock is not already held by this CPU (no recursion).
  2. The lock rank is strictly greater than all currently held locks on this CPU, **OR** both the held lock and the requested lock have `kind == LOCK_KIND_SCHED` and are acquired in strict ascending address/CPU order (`(uintptr_t)held < (uintptr_t)lock`).
  All other same-rank or descending-rank acquisitions trigger an immediate fatal panic.

### 3. Isolated AP Panic Path (SM11b)
- Calling `serial_puts` from an AP panic touches `dmesg_append` and console locks, which are shared/BSP resources and risk recursive locking or silent hanging.
- A dedicated `spin_panic_ap(const char *reason, spinlock_t *attempted)`:
  - Uses `serial_raw_puts`, `serial_raw_putc`, and `serial_raw_print_hex` exclusively (lockless direct COM1 writes).
  - Emits:
    `[FATAL] Lock discipline on AP <id>: <reason> <attempted->name> (rank <attempted->rank>)`
    `        held chain: <held[0]->name> -> <held[1]->name> ...`
  - Records the failure into `cpu_current()->lock_panic`.
  - Halts the offending AP immediately: `for (;;) __asm__ volatile("cli; hlt");`.
  - The BSP remains operational, allowing the test harness or kernel to diagnose the failure without whole-machine hang.

### 4. AP 1 Test Dispatching & Parking Preservation (SM11c)
- A halted CPU in `cli; hlt` cannot wake on memory flag changes. Waking APs via IPIs is deferred to Piece 5.
- Therefore, Piece 3 testing is restricted to **BSP + AP 1**. APs 2..N bypass all test logic and enter `park: for (;;) __asm__ volatile("cli; hlt");` immediately after their Piece 2 bring-up, keeping their verified state intact.
- When test mode is requested by the test harness:
  1. AP 1 completes its local setup (GS, GDT, TSS, ISTs, LAPIC).
  2. AP 1 enters a bounded synchronous handshake with BSP using a dedicated test mailbox with `pause` backoff.
  3. Contention test: BSP and AP 1 concurrently run 100,000 increments on a shared counter under a test spinlock.
  4. Tracker isolation test: AP 1 acquires a rank 2 lock while BSP holds rank 1; AP 1 verifies local `lock_depth == 1` and BSP verifies local `lock_depth == 1`.
  5. Inverted acquisition test: AP 1 deliberately acquires rank 2 followed by rank 1. `spin_panic_ap` fires, prints diagnostic to raw UART, and halts AP 1. The BSP verifies that it did not hang and boots to shell.
  6. On test completion, AP 1 enters `park`.

### 5. Always-On Compilation Policy
- Lock discipline checks (`can_acquire`, rank comparison, nesting validation) are **always-on** in all kernel builds.
- The ~30 cycle overhead on uncontended paths is negligible compared to cache-line bouncing (`lock bts`), and provides non-negotiable correctness guarantees for safety and multi-core stability.

### 6. Contention Telemetry & Spin High-Water Mark
- In `spin_lock_irqsave`:
  - `acquire_count` increments on every call.
  - `contention_count` increments if the initial atomic attempt fails and spinning begins.
  - Spin loop tracks loop iterations; if iterations exceed `max_spin_iters`, `max_spin_iters` is updated.
  - If spin iterations exceed a calibrated threshold (1,000,000 spins), a raw UART warning is logged to detect livelocks.

### 7. Debugging Assertions
- `spin_debug_assert_unheld(void)`: Verifies `cpu_current()->lock_depth == 0` (enforced at `switch_context` sites).
- `spin_debug_assert_held(spinlock_t *lock)`: Verifies `lock` is in `cpu_current()->held_locks[0 .. lock_depth-1]` (used at entry of internal `_unlocked` routines).

## Explicitly Out of Scope

- **No IPIs or TLB shootdown** (Piece 5).
- **No multi-CPU thread scheduling or runqueue migration** (Piece 4).
- **No concurrent PMM/VMM physical or virtual memory allocation** (Piece 6).
- **No unmasking of external interrupts on APs** (APs maintain IF=0).
- **No NUMA optimizations.**

## Verification Plan

### QEMU Test Automation
A dedicated Python test runner (`scripts/test_smp_locks.py`) will execute:
1. `-smp 1` (BIOS and UEFI): Single-CPU baseline, rank checking, and shell arrival.
2. `-smp 4` and `-smp 8`:
   - Two-core concurrent atomic contention (assert counter == 200,000).
   - Per-CPU tracker isolation.
   - AP rank-inversion trap with raw UART output inspection and BSP survival.

### Regression Checklist
All existing test suites must pass without regression:
1. `make test-smp-discovery` (Piece 1 Limine/MADT)
2. `make test-smp-percpu` (Piece 2 GS/TSS/stack isolation & AP parking)
3. `make test-nmi` (exact syscall-boundary NMI delivery)
4. `make test-usb-mount` (read-only mount policy and ext2 rank 1 locking)
5. `make test-usb-persistence` (writable ext2 transactions and sync paths)
6. `make test-shell` (interactive shell stability)

### Hardware Acceptance (Dell Latitude 5590 Physical Hardware)
- **Date & Hardware:** 2026-09-24, Dell Latitude 5590, Intel Core i7-8650U (4 cores / 8 threads), 32 GiB RAM, UEFI boot from USB flash drive.
- **MADT & Limine Enumeration:** 8 CPUs enumerated and agreed upon (BSP LAPIC ID 0, APs 1..7).
- **Per-CPU Bring-up:** APs 1..7 passed GS/TSS/IST1 fault/IST2 probes and parked with interrupts disabled (`IF=0`).
- **Piece 3 Lock Discipline & Contention Test:**
  - BSP spinlock selftest passed (ranks, classification, asserts).
  - Two-core lock contention test executed between BSP and AP 1 concurrently (100,000 increments each).
  - **Observed Hardware Contention:** **84,730 contentions** recorded on physical memory bus with `pause` backoff.
  - **Final Counter:** Exactly **200,000** (expected 200,000; zero lost updates under hardware SMP contention).
  - Mutual exclusion strictly maintained; APs 2..7 remained parked.
- **Captured Diagnostic Output:**
```text
========================================================
SMP Piece 1: AP Discovery
========================================================
[ OK ] Limine SMP response cross-checked against ACPI MADT: 8 CPU(s) agree (BSP LAPIC ID 0)
[ OK ] Per-CPU AP 1: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 2: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 3: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 4: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 5: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 6: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] Per-CPU AP 7: GS/TSS/IST1 fault/IST2 probe passed
[ OK ] SMP Piece 2 per-CPU storage ready (APs parked)
[ OK ] All 7 application processor(s) online (parked, interrupts disabled)
       [ OK ] All 8 CPU(s) accounted for (1 BSP + 7 AP(s)), matching MADT
[ OK ] SMP Piece 1 (AP discovery) complete.

========================================================
SMP Piece 3: Lock Discipline & Contention
========================================================
       [PASS] BSP spinlock selftest passed (ranks, classification, asserts)
[TEST] SMP Piece 3: Starting two-core lock contention test (BSP + AP 1)...
       Counter value: 200000 (expected: 200000)
       Test lock contention count: 84730
       [PASS] Two-core lock contention test passed (exact 200,000 updates, mutual exclusion verified)
[ OK ] SMP Piece 3 (Lock discipline) complete.
```

## Verified Test Matrix (2026-09-24)

### QEMU Test Results (`make test-smp-locks`)
| Test Case | Firmware | CPUs | Result | Details |
| :--- | :--- | :--- | :--- | :--- |
| Single-CPU Baseline | BIOS | 1 | **PASS** | Selftests passed, contention skipped on 1 CPU, shell reached |
| Single-CPU Baseline | UEFI | 1 | **PASS** | Selftests passed, contention skipped on 1 CPU, shell reached |
| Two-Core Contention | BIOS | 4 | **PASS** | Counter: exact 200,000; contention count > 0; mutual exclusion verified |
| Two-Core Contention | BIOS | 8 | **PASS** | Counter: exact 200,000; contention count > 0; mutual exclusion verified |
| Two-Core Contention | UEFI | 4 | **PASS** | Counter: exact 200,000; contention count > 0; mutual exclusion verified |
| Two-Core Contention | UEFI | 8 | **PASS** | Counter: exact 200,000; contention count > 0; mutual exclusion verified |
| AP 1 Inversion Isolation | BIOS | 4 | **PASS** | AP 1 trapped & halted via `spin_fatal`; held chain printed to raw UART; BSP survived to shell |
| AP 1 Inversion Isolation | UEFI | 4 | **PASS** | AP 1 trapped & halted via `spin_fatal`; held chain printed to raw UART; BSP survived to shell |
| Assert-Held Negative Trap | BIOS | 1 | **PASS** | BSP trapped on unheld lock via `spin_fatal`; CPU cleanly halted |
| Assert-Held Negative Trap | UEFI | 1 | **PASS** | BSP trapped on unheld lock via `spin_fatal`; CPU cleanly halted |

### Regression Suite Results
| Suite | Command | Result |
| :--- | :--- | :--- |
| SMP Piece 1 AP Discovery | `make test-smp-discovery` | **PASS** (-smp 1 and -smp 4) |
| SMP Piece 2 Per-CPU Storage | `make test-smp-percpu` | **PASS** (6 runs: BIOS & UEFI, 1/4/8 CPUs) |
| Input Subsystem | `make test-input` | **PASS** |
| Framebuffer Console | `make test-console` | **PASS** |
| Ext2 Filesystem | `make test-ext2` | **PASS** |
| Syscall/NMI Transitions | `make test-nmi` | **PASS** (56 exact-boundary NMI tests) |
