# SMP Piece 4 — The SMP Scheduler (Per-CPU Runqueues & Work-Stealing)

Status: **COMPLETE (Verified on QEMU across BIOS & UEFI for 1, 4, 8 CPUs and bare-metal Dell Latitude 5590 with 8 CPUs, 2026-09-24)**.
Prerequisites: Piece 1 (AP discovery), Piece 2 (per-CPU storage), and Piece 3 (lock discipline) verified on QEMU and Dell Latitude 5590.

## Overview and Goals

Previously, thread scheduling was single-core: all threads resided on a global runqueue managed exclusively by the bootstrap processor (CPU 0), while application processors (APs) remained parked with interrupts disabled.

Piece 4 activates the SMP scheduler:
1. Every CPU owns an independent `scheduler_cpu_t` containing its own runqueue (`runqueue_head`, `runqueue_tail`), idle thread, preemption state, and scheduler spinlock (`LOCK_KIND_SCHED`).
2. APs unpark, calibrate their local APIC timer at 100 Hz, enable preemption, and run independent idle loops.
3. Dual-lock work-stealing (`SM13`) allows idle cores to safely migrate unbound threads (`cpu_affinity == -1`) from heavily loaded sibling cores without deadlocks.
4. Process tasks spawned by `sys_spawn` are pinned to the parent CPU (`cpu_affinity = parent_cpu`) to avoid cross-core wakeup latency prior to IPI implementation in Piece 5.
5. CPU 0 pre-allocates AP idle threads, TCBs, and kernel stacks (`SM16`/`SM17`); APs perform zero dynamic memory allocation.

---

## Architecture and Contracts

### 1. Per-CPU Runqueues & Storage (SM12)
- Each CPU has an isolated scheduling domain:
  - `runqueue_head` / `runqueue_tail`: Independent FIFO singly linked runnable thread list.
  - `sched_lock`: Classified scheduler spinlock (`SPINLOCK_RANKED_KIND(1, LOCK_KIND_SCHED, ...)`).
  - `idle_thread`: Dedicated per-CPU low-power halt thread.
  - `blocked_threads`: CPU-local sleep channel wait list.
  - `dead_threads`: Threads terminated on this core, drained and reaped on CPU 0 via `sched_reap_dead()`.
  - `preemption_enabled`: Core-local preemption flag.
  - `stolen_tasks_count`: Telemetry tracking successful work migrations.
- The shared kernel stack slot bitmap (`stack_slots_bitmap`) is protected by a dedicated `g_kstack_lock` (Rank 1).

### 2. Dual-Lock Work-Stealing Discipline (SM13, L1)
- Idle CPUs that find their local runqueue empty attempt to steal an unbound runnable thread (`cpu_affinity == -1`) from a sibling core in round-robin order.
- To prevent AB-BA deadlocks between two stealing cores, `sched_lock_pair(a, b)` sorts the locks by virtual address and acquires them in strict ascending order:
  ```c
  void sched_lock_pair(spinlock_t *a, spinlock_t *b) {
      spinlock_t *first = ((uintptr_t)a < (uintptr_t)b) ? a : b;
      spinlock_t *second = ((uintptr_t)a < (uintptr_t)b) ? b : a;
      spin_lock_noirq(first);
      spin_lock_noirq(second);
  }
  ```
- Releasing is performed in exact reverse (descending) order via `sched_unlock_pair(a, b)`.
- Both acquisitions are checked by `spin_debug_acquire(lock)`, validating the `LOCK_KIND_SCHED` ascending exception verified in Piece 3.

### 3. AP Initialization & Zero Dynamic Allocation (SM16, SM17)
- AP idle threads and stacks are pre-allocated exclusively on CPU 0 in `sched_init_aps()` during boot.
- APs never call `kmalloc`, `kfree`, `kstack_alloc`, or `kstack_free`.
- APs transition from their parked loop into `sched_ap_start()` when `g_smp_sched_active` is asserted:
  - Configure `TSS.RSP0` to the pre-allocated idle thread stack.
  - Load the shared kernel `CR3`.
  - Start the local APIC timer at 100 Hz via `lapic_timer_start_ap()`.
  - Enable preemption.
  - Restore context into `idle_thread_entry`.

### 4. Idle Thread Wakeup & LAPIC Timer
- In `idle_thread_entry`:
  ```c
  static void idle_thread_entry(void *arg) {
      (void)arg;
      for (;;) {
          __asm__ volatile("sti; hlt" ::: "memory");
          thread_yield();
      }
  }
  ```
- CPUs sleep in low-power `hlt` with interrupts enabled.
- The 100 Hz LAPIC timer tick wakes the core, executes `sched_on_timer_tick()`, and triggers `thread_yield()` to consume queued or stolen tasks.
- If a runnable thread calls `thread_yield()` when no other threads are ready to run, it immediately resumes without an unnecessary context switch to `idle_thread`.

---

## Verification Evidence

### QEMU Automated Multi-Core Suite (`make test-smp-sched`)
The test suite validates:
1. **T3.b (Dual-Lock Ordering)**: `sched_lock_pair` and `sched_unlock_pair` in forward and inverted order without rank or inversion panics.
2. **T3.a (Pinned Multi-Core Execution)**: Concurrently runs pinned worker threads across all online cores ($N \in \{1, 4, 8\}$), verifying exact per-CPU affinity and completion.
3. **T3.c (Forced Work-Stealing)**: Enqueues 16 unbound worker threads on CPU 0; verifies that idle APs actively steal and execute the work (measured total tasks stolen $> 0$ across APs 1..7).
4. **Interactive Shell**: Clean startup to Ring 3 `fortress> ` shell prompt.

#### Results:
- **BIOS -smp 1**: PASS (dual-lock ordering, pinned execution, work-stealing=False, shell reached)
- **UEFI -smp 1**: PASS (dual-lock ordering, pinned execution, work-stealing=False, shell reached)
- **BIOS -smp 4**: PASS (dual-lock ordering, pinned execution, work-stealing=True, shell reached)
- **UEFI -smp 4**: PASS (dual-lock ordering, pinned execution, work-stealing=True, shell reached)
- **BIOS -smp 8**: PASS (dual-lock ordering, pinned execution, work-stealing=True, shell reached)
- **UEFI -smp 8**: PASS (dual-lock ordering, pinned execution, work-stealing=True, shell reached)
- Total stolen tasks measured on 8 CPUs: **29 tasks** across APs 1 through 7.
- Regression suites: `test-smp-locks` (100% PASS), `test-ext2` (100% PASS), `test-input` (100% PASS).

### Bare-Metal Acceptance (Dell Latitude 5590, UEFI, 8 CPUs)
Verified on physical hardware (Intel Core i7-8650U, 8 logical cores, UEFI boot via USB) on 2026-09-24:
```
========================================================
SMP Piece 1: AP Discovery
========================================================
[ OK ] Limine SMP response cross-checked against ACPI MADT: 8 CPU(s) agree (BSP LAPIC ID 0)
[ OK ] Per-CPU AP 1..7: GS/TSS/IST1 fault/IST2 probe passed
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
       Test lock contention count: 80435
       [PASS] Two-core lock contention test passed (exact 200,000 updates, mutual exclusion verified)
[ OK ] SMP Piece 3 (Lock discipline) complete.

========================================================
SMP Piece 4: The SMP Scheduler & Work-Stealing
========================================================
[TEST] SMP Piece 4: Testing sched_lock_pair dual-lock ordering (T3.b)...
       [PASS] sched_lock_pair acquired and released safely in forward and inverted order
[TEST] SMP Piece 4: Spawning concurrent pinned workers across cores (T3.a)...
       [PASS] All pinned workers executed on their assigned CPU cores (verified 8 cores)
[TEST] SMP Piece 4: Forced work-stealing test (16 workers queued on CPU 0) (T3.c)...
       [INFO] CPU 1 stole: 2 tasks
       [INFO] CPU 2 stole: 2 tasks
       [INFO] CPU 3 stole: 2 tasks
       [INFO] CPU 4 stole: 2 tasks
       [INFO] CPU 5 stole: 1 tasks
       [INFO] CPU 6 stole: 2 tasks
       [INFO] CPU 7 stole: 2 tasks
       [PASS] AP work-stealing verified (total tasks stolen across APs: 13)
[ OK ] SMP Piece 4 (Scheduler & Work-Stealing) complete.
```

### Hardware Bug Fixes & Discoveries
1. **AP Hardware IRQ Interception in `idt.c`**:
   - *Bug:* Early AP fault trap (`if (cpu->id != 0) { cpu->fault_vector = frame->vector; ... }`) was placed above `if (frame->vector >= 32)`. The first LAPIC timer tick (vector 32) halted the AP.
   - *Fix:* Reordered `idt.c` so `frame->vector >= 32 && frame->vector != 0x80` is dispatched first to registered handlers.
2. **Dead Thread Stack Deallocation Race Condition (`zombie_thread`)**:
   - *Bug:* On bare metal, `thread_exit` previously added `curr` to `dead_threads` and unlocked `sched_lock` *before* `switch_context`. Core 0 immediately reclaimed `curr` and unmapped its kernel stack (`vmm_unmap_page`), causing an instant `#PF` on the exiting AP inside `switch_context`.
   - *Fix:* Exiting thread is stashed in `scheduler_cpus[cid].zombie_thread`. It is only moved to `dead_threads` via `sched_post_switch()` *after* `switch_context` has returned on the new stack.
3. **Idle Loop Execution**:
   - *Bug:* `idle_thread_entry` previously executed `sti; hlt` without calling `thread_yield()`. APs could not check their runqueue voluntarily when idle.
   - *Fix:* Added `thread_yield()` inside `idle_thread_entry` before `sti; hlt`.
4. **Ring Bus Contention during Wait Loops**:
   - *Fix:* Added `__builtin_ia32_pause()` inside multi-core wait spin loops in `main.c` to prevent bus flooding on multi-core Intel hardware.

