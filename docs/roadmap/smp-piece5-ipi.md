# SMP Piece 5 — Cross-Core Coordination & IPIs

Status: **COMPLETE (Verified on QEMU across BIOS & UEFI for 1, 4, 8 CPUs and bare-metal Dell Latitude 5590 with 8 CPUs, 2026-09-24)**.  
Prerequisites: Piece 1 (AP discovery), Piece 2 (per-CPU storage), Piece 3 (lock discipline), and Piece 4 (SMP scheduler) verified on QEMU and Dell Latitude 5590.

## Overview and Goals

In Pieces 1 through 4, application processors were discovered, initialized with private per-CPU storage, subjected to real lock contention, and integrated into the preemptive SMP scheduler with dual-lock work-stealing.

Piece 5 implements cross-core coordination via Local APIC Inter-Processor Interrupts (IPIs):
1. **APIC ICR Interface**: Adds hardware xAPIC Interrupt Command Register (`0x300` / `0x310`) messaging primitives (`lapic_wait_icr_idle`, `lapic_send_ipi`, `lapic_send_ipi_all_excluding_self`).
2. **Dedicated IPI Vectors**:
   - `IPI_VECTOR_TLB` (0xFC / 252): Synchronous TLB shootdown.
   - `IPI_VECTOR_RESCHED` (0xFD / 253): Remote core wake from idle / thread reschedule.
   - `IPI_VECTOR_PANIC` (0xFE / 254): Multi-core emergency halt.
3. **Synchronous TLB Shootdown Barrier (`SM14`, `SM15`)**:
   - When a page is unmapped (`vmm_unmap_page`), a synchronous shootdown is sent to all other online CPUs running or caching that address space.
   - Targets invalidate their TLB via `invlpg` and atomically clear their bit in `ack_mask` before issuing `lapic_eoi()`.
   - The initiating core spins on `ack_mask == 0` with `pause` and a bounded timeout.
   - Shootdown lock acquisition is reentrant with respect to incoming IPIs (`sti; pause; cli` spin loop) to prevent classic IPI deadlocks.
4. **Address-Space Teardown Protection**:
   - `vmm_destroy_pml4` checks that no online CPU is currently executing in the address space (`cpu_locals[i].current_thread->cr3 != pml4_phys`) and executes a full TLB shootdown prior to reclaiming table frames.
5. **Immediate Remote Core Wakeup**:
   - When `thread_create` enqueues a thread on a remote core, `smp_send_resched(target_cpu)` wakes it from `sti; hlt` immediately without waiting for the next periodic 100 Hz timer tick.
6. **Multi-Core Panic Halting**:
   - When `spin_fatal` or a kernel panic occurs, `smp_send_panic()` broadcasts `IPI_VECTOR_PANIC` to halt all other CPUs in `cli; hlt`.

---

## Architecture and Contracts

### 1. Synchronous TLB Shootdown (SM14, SM15)
- Binding invariant **SM14**: Any VMM operation that unmaps or changes protection on a page potentially cached in another CPU's TLB issues a synchronous IPI-based shootdown and waits for acknowledgment before returning.
- Binding invariant **SM15**: IPI handlers follow ISR discipline: bounded work only, no allocation or blocking, with an explicit ack/completion protocol distinct from EOI.

### 2. Lock Discipline & Deadlock Avoidance
- The shootdown mailbox lock (`g_smp_tlb_shootdown.lock`) is classified with Rank 3 (`LOCK_KIND_ORDINARY`).
- If an initiator holds the shootdown lock and waits for sibling acks, another CPU attempting to acquire the shootdown lock spins with interrupts temporarily enabled (`sti; pause; cli`), allowing it to service the initiator's incoming shootdown IPI without deadlock.

---

## Verification Evidence

### Automated QEMU Test Suite (`make test-smp-ipi`)
Validates across **BIOS & UEFI** for **1, 4, and 8 CPUs**:
1. **T5.a (Unicast & Broadcast IPI Ping)**:
   - BSP sends unicast IPI to AP 1 (`smp_send_resched(1)`).
   - BSP sends broadcast TLB shootdown IPI (`smp_tlb_shootdown`).
   - Verifies all online APs acknowledge delivery.
2. **T5.b (Remote Core Wakeup)**:
   - Enqueues a thread onto idling AP 1.
   - Verifies AP 1 wakes from `hlt` via `IPI_VECTOR_RESCHED` and executes the worker.
3. **T5.c (Real VMM Page Unmap and Shootdown Barrier)**:
   - Maps a page in kernel space, unmaps it via `vmm_unmap_page`.
   - Confirms `smp_tlb_shootdown` triggers and completes across all APs before `vmm_unmap_page` returns.
4. **Shell Prompt**:
   - Normal boot reaches interactive `fortress> ` shell prompt.

#### Results:
- **BIOS -smp 1**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- **UEFI -smp 1**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- **BIOS -smp 4**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- **UEFI -smp 4**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- **BIOS -smp 8**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- **UEFI -smp 8**: PASS (unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached)
- Regression suites: `test-smp-sched` (100% PASS), `test-smp-locks` (100% PASS).

### Bare-Metal Acceptance (Dell Latitude 5590, UEFI, 8 CPUs)
Verified on physical hardware (Intel Core i7-8650U, 8 logical cores, UEFI boot via USB) on 2026-09-24:
```
========================================================
SMP Piece 5: Cross-Core Coordination & IPIs
========================================================
[TEST] SMP Piece 5: Testing unicast IPI delivery (BSP -> AP 1) (T5.a)...
       [PASS] Unicast IPI delivery verified (BSP -> AP 1)
[TEST] SMP Piece 5: Testing broadcast synchronous TLB shootdown (T5.a)...
       [PASS] Synchronous broadcast TLB shootdown acknowledged by all online APs (7 APs)
[TEST] SMP Piece 5: Remote core wakeup via reschedule IPI (T5.b)...
       [PASS] Remote core wakeup verified (AP 1 awakened from idle)
[TEST] SMP Piece 5: Real VMM page unmap and shootdown barrier (T5.c)...
       [PASS] VMM synchronous TLB shootdown and frame unmap verified (SM14, SM15)
[ OK ] SMP Piece 5 (Cross-Core Coordination & IPIs) complete.
```

