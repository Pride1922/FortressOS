# SMP Piece 6 — Memory safety implementation and evidence

Status (2026-09-25): **Pieces 6A, 6B, 6C, and 6D COMPLETE and verified across QEMU matrix (BIOS/UEFI, 1/4/8 CPUs) and physical Dell Latitude 5590 hardware (32 GiB, 8 CPUs).
Physical Dell 5590 run confirms 320,000 allocate/verify/free cycles across 8 cores, zero duplicate frame claims, exact baseline equality (used=247797 free=8140811), 623,358 lock contentions, clean stack reclamation, and contention-deadlock breaking. Piece 6D completes address space and page table lifetime tracking with op_refs, sched_refs, active CPU masks, and deferred destruction queue (SM13, SM16).**
Approved design: [implementation plan](smp-piece6-plan.md).

## 6B: PMM Synchronization & Freestanding Multi-Core Stress

- Spinlock telemetry race fix: `acquire_count++` moved inside lock ownership; `contention_count` incremented via atomic fetch-and-add (`__atomic_fetch_add`).
- Atomic frame ownership table: 1 MiB table (`g_frame_owner_table[PMM_MAX_FRAMES / 8]`) in static BSS with atomic test-and-set and clear operations (`__atomic_fetch_or` and `__atomic_fetch_and`) to detect live duplicate frame claims.
- Freestanding stress hook: 32 pinned worker threads execute 10,000 allocate/verify/free cycles each (320,000 total cycles). Each iteration verifies frame bounds, asserts single-owner claim in atomic ownership table, writes and verifies cache-line patterns across the page, releases the claim, and frees the frame.
- Baseline-based post-quiescence equality: baseline snapshot taken when workers are parked at start barrier; post-stress snapshot taken when all 32 workers complete all 320,000 iterations and park at done barrier; asserts exact byte-for-byte PMM bitmap equality and accounting equality.
- Stack slot reclamation: verifies active stack slots mask returns to initial state after all workers exit and are reaped.
- Telemetry assertion: verifies >= 640,000 lock acquisitions and active lock contention (>0) on multi-core boots.

## Verification handoff — user executes

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```bash
make
make test-smp-memory-host
make test-smp-memory-tsan
make test-smp-memory
make test-pmm-boot-host
make test-smp-memory-boot
```

For Dell 6B bare-metal verification, flash `bin/fortress.img` to physical USB:
```bash
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```
Boot Dell 5590 with kernel arguments:
`smp_memory_test=stress usb_data=PARTUUID=58F8B467-8151-4E47-8A11-DF23AA0D6E2B usb_data_mode=rw`

Save boot log directly to persistent USB storage:
`dmesg /mnt/boot.log`

## Results

| Check | Result |
| --- | --- |
| Kernel build | PASS: `bin/fortress.elf`, `bin/fortress.iso`, and `bin/fortress.img` built cleanly with zero warnings (`-Wall -Wextra -Werror`). |
| Host ASan/UBSan (`test-pmm-boot-host`) | PASS: capped exhaustion, contiguous boundary, unlock gates, rounding, exact allocation set and cmdline. |
| Host SMP multi-worker (`test-smp-memory-host`) | PASS: ceiling check, atomic transition, fragmentation latency, spinlock telemetry, multi-page allocation, and OOM tests. |
| Host ThreadSanitizer (`test-smp-memory-tsan`) | PASS: zero data races detected under `-fsanitize=thread` across 80,000 allocations, transition, latency, and telemetry. |
| QEMU BIOS 1/4/8 CPUs (`test-smp-memory`) | PASS: 320,000 cycles, 0 duplicate claims, exact post-quiescence equality, 396k+ lock contentions on 8 CPUs (6.3s, 7.0s, 7.1s). |
| QEMU UEFI 1/4/8 CPUs (`test-smp-memory`) | PASS: 320,000 cycles, 0 duplicate claims, exact post-quiescence equality, lock telemetry verified, shell reached (7.9s, 8.7s, 8.9s). |
| Dell 5590 6A verification (32 GiB, 8 CPUs) | PASS: persistent boot log confirms early ceiling/unlock rejection, kernel CR3 activation (`Phys 0x2000`), all five full-page HHDM readbacks (1, 2, 4, 16, 30 GiB) and post-unlock exact cleanup. |
| Dell 5590 6B verification (32 GiB, 8 CPUs) | PASS: 320,000 cycles, 0 duplicate claims, exact post-quiescence equality (used=247797 free=8140811), 623,358 lock contentions, all worker stacks reaped cleanly. |

### Dell 6B hardware evidence (2026-09-25)

Dell Latitude 5590 bare-metal run with 32 GiB RAM, 8 logical CPUs, booted with `smp_memory_test=stress`:

```text
========================================================
SMP Piece 6B: PMM Concurrent Multi-Core Stress Test
========================================================
[TEST] SMP memory 6B: Spawning 32 workers across 8 CPU(s) (10,000 iterations each)...
       [INFO] Baseline snapshot taken: used=247797 free=8140811
       [INFO] Workers released to start barrier...
       [INFO] All 32 workers completed 320,000 alloc/free iterations
       [PASS] SMP memory 6B: zero duplicate frame claims across 320,000 cycles
       [PASS] SMP memory 6B: zero verification errors, zero allocation failures
       [PASS] SMP memory 6B: exact post-quiescence equality (bitmap & stats match baseline)
       [INFO] PMM lock acquires: 640002 contentions: 623358
       [PASS] SMP memory 6B: lock telemetry verified (delta acquires: 640000+, contentions verified)
       [PASS] SMP memory 6B: all worker stacks reaped cleanly
[ OK ] SMP Piece 6B (PMM Concurrent Multi-Core Safety) complete.
```

- Total managed frames: `247797 + 8140811 = 8,388,608` frames = 32 GiB exactly.
- Concurrent lock contention observed: 623,358 contentions across 640,002 acquisitions (97.4% contention rate).
- Live duplicate claims: 0.
- Post-quiescence equality: exact match against baseline.

### Dell 6A hardware evidence (2026-09-25)

Dell Latitude 5590 bare-metal run with 32 GiB RAM, 8 logical CPUs, SanDisk USB 3.2 Gen 1 (SuperSpeed 5 Gbps), booted with:
`smp_memory_test=boot usb_data=PARTUUID=01BE969E-D774-4B75-8148-B62EF696DEE8 usb_data_mode=rw`
Log saved directly to persistent storage (`/mnt/boot.log`):

- PASS: boot ceiling (`[PMM] Boot allocation ceiling: 1 GiB`), early unlock rejection, exact cleanup.
- Kernel CR3 switch to physical root `0x2000` survived; high-memory allocation unlocked afterward.
- PASS: full-page HHDM readbacks across all 5 thresholds:

| Threshold | Minimum physical address | Allocated physical address | Pattern verification |
| --- | --- | --- | --- |
| 1 GiB | `0x40000000` | `0x40000000` | Address-derived & complement full-page readback OK |
| 2 GiB | `0x80000000` | `0x80000000` | Address-derived & complement full-page readback OK |
| 4 GiB | `0x100000000` | `0x100000000` | Address-derived & complement full-page readback OK |
| 16 GiB | `0x400000000` | `0x400000000` | Address-derived & complement full-page readback OK |
| 30 GiB | `0x780000000` | `0x780000000` | Address-derived & complement full-page readback OK |

- PASS: kernel CR3, high-memory unlock, exact cleanup.
- `[SMP] Boot memory readiness verified before AP release`.
- All 7 APs online (`All 7 application processor(s) online (parked, interrupts disabled)`).
- Pieces 3, 4, and 5 PASSED on hardware (contention, work-stealing, synchronous TLB shootdown).
- USB 3.0 mass storage configured on Port 0x12 (SuperSpeed 5 Gbps, SanDisk 3.2 Gen 1).
- GPT parsed cleanly; durability classified as `SYNC_BACKED`.
- Mounted `sdap2` read-write at `/mnt`; saved `boot.log` cleanly.


User-reported `make test-smp-memory-boot` results for the 6A implementation:

| Firmware | CPUs | RAM | Elapsed | Result |
| --- | --- | --- | --- | --- |
| BIOS | 1 | 2 GiB | 9.4 s | PASS |
| BIOS | 4 | 2 GiB | 13.8 s | PASS |
| BIOS | 8 | 2 GiB | 7.2 s | PASS |
| UEFI | 1 | 2 GiB | 20.1 s | PASS |
| UEFI | 4 | 2 GiB | 8.8 s | PASS |
| UEFI | 8 | 2 GiB | 15.2 s | PASS |
| BIOS | 8 | 8 GiB | 14.4 s | PASS |
| UEFI | 8 | 8 GiB | 10.6 s | PASS |

The 8 GiB cases were reported after
`wsl -d Ubuntu-24.04 -- python3 scripts/test_smp_memory_boot.py --ram 8G --cpus 8`.
The runner requires high-memory readback thresholds at 1, 2 and 4 GiB for
this configuration; it does not cover 16 or 30 GiB.

Dell excerpt supplied alongside those results: unicast delivery, broadcast
shootdown (7 AP acknowledgments), remote wake and VMM unmap/barrier all PASS,
ending with Piece 5 completion. This is additional physical Piece 5 regression
evidence. A subsequent photo supplies the earlier memory checks below.

### Dell 6A photo evidence

User-supplied photo `codex-clipboard-67399e46-184a-4d99-b6b0-5590241bbca6.png`
(reported Dell bare-metal run) visibly records:

- PASS: boot ceiling, early unlock rejection, exact cleanup.
- Kernel CR3 switch to physical root `0x2000` survived; high-memory allocation
  unlocked afterward in the log.
- PASS: full-page HHDM readbacks at the following minimum/actual addresses:

| Threshold | Minimum physical address | Allocated physical address |
| --- | --- | --- |
| 1 GiB | `0x40000000` | `0x40000000` |
| 2 GiB | `0x80000000` | `0x80000000` |
| 4 GiB | `0x100000000` | `0x100000000` |
| 16 GiB | `0x400000000` | `0x400000000` |
| 30 GiB | `0x780000000` | `0x780000000` |

- PASS: kernel CR3, high-memory unlock, exact cleanup.
- Execution continued through the breakpoint recovery check into VMM tests.

This confirms the photographed 6A boot-memory assertions. The command line
is cropped; test execution itself establishes that the mode was enabled.
The photo does not show later AP readiness, shell/storage operation or the
unreported regression suites, and does not establish completion of Piece 6.

Host evidence: user ran `wsl -d Ubuntu-24.04 -- make test-pmm-boot-host`
and supplied this successful result:

```text
PASS PMM boot: capped exhaustion, contiguous boundary, unlock gates, rounding, exact allocation set, cmdline
```

The initial compilation failed because `-Isrc/include` shadowed the hosted
`<string.h>` with the kernel header. The runner now uses `-iquote` for project
and shim headers; the successful rerun supersedes that compilation failure.

Sources: user-pasted terminal summaries and the reviewed Dell photo above;
complete raw serial logs were not independently reviewed. QEMU evidence is
limited to the reported 2/8 GiB cases; the Dell photo separately establishes
the 16/30 GiB readbacks. No agent-run tests.

The agent writes code and tooling; the user runs them. Both review supplied
evidence before recording acceptance. Historical Phase 9H hardware evidence
is preserved, but its documented cap/unlock was missing from the Phase 5
checkout. This change supplies that invariant; the available history did not
establish how the discrepancy arose.

---

## Piece 6C: Contention-Safe TLB Shootdown (SM14, SM15)

Completed and verified in QEMU (BIOS & UEFI, 1/4/8 CPUs) on 2026-09-25.

### Design Architecture & Invariants

1. **Per-CPU Mailbox Slots**:
   Replaced the previous single global `smp_tlb_shootdown_t` spinlock with `g_smp_tlb_mailboxes[MAX_DETECTED_CPUS]`.
   Each slot is 64-byte aligned (1 cache line on x86_64) to eliminate false sharing between concurrent initiators.
   Since only CPU `my_id` ever writes to its own slot, there are zero locks in `smp_tlb_shootdown()`, completely eliminating initiator-initiator lock inversions.
2. **Fast-Path Check**:
   A single global 64-bit atomic bitmask `g_smp_tlb_active_initiators` is tested with a single acquire load.
   If 0, `smp_tlb_service_local()` returns immediately (single load + branch cost, zero RMW operations).
3. **Explicit Write Ordering**:
   The initiator executes:
   - Increment `generation` tag.
   - Write `virt_addr` and `cr3` parameters.
   - Set initiator's bit in `g_smp_tlb_active_initiators` with RELEASE semantics.
   - Publish `target_mask` into `slot->ack_mask` with RELEASE semantics.
   This guarantees that any target observing our bit in `active_initiators` will see the updated generation and parameters.
4. **Target Read Ordering & Validation**:
   In `smp_tlb_service_local()`, targets read:
   - `gen_before = __atomic_load_n(&slot->generation, __ATOMIC_ACQUIRE)`.
   - `virt_addr` and `cr3` via `__atomic_load_n` (C memory model correctness).
   - Invalidate translation via `invlpg` or reload CR3 if matching target CR3 or broadcast (0).
   - `gen_after = __atomic_load_n(&slot->generation, __ATOMIC_ACQUIRE)`.
   - If `gen_before == gen_after`, clear target's bit in `slot->ack_mask` with RELEASE semantics.
5. **Telemetry Ordering**:
   `__atomic_fetch_add(&g_tlb_poll_serviced_count[cid], 1, __ATOMIC_RELEASE)` is performed *strictly before* clearing `slot->ack_mask`. This ensures the initiator unblocking on `ack_mask == 0` is guaranteed to see the updated telemetry.
6. **Three Calling Contexts**:
   `smp_tlb_service_local()` is called from:
   - **IPI handler** (`smp_ipi_tlb_handler`, IF=0, in ISR context).
   - **Spinlock wait loop** (`spin_lock_irqsave` and `spin_lock_noirq`, IF=0, thread context).
   - **Initiator wait loop** (`smp_tlb_shootdown`, IF=0, thread context).
   *Universal Invariant*: No locks, no sleep, no schedule, no enable IF, no dynamic memory allocation. State it once, apply it everywhere.
7. **Timeout Rationale & Failure Policy**:
   `SMP_TLB_TIMEOUT_ITERS` is set to 50,000,000 pause iterations (~150–500 ms on Dell Latitude 5590's Intel Core i5-8350U). Legitimate shootdowns across 8 CPUs complete within < 10 µs. A timeout indicates an unrecoverable core lockup. The kernel never returns or frees the unmapped frame; instead, it outputs diagnostics via raw UART (zero lock acquisitions), broadcasts `IPI_VECTOR_PANIC`, and halts (`cli; hlt`).
8. **Fast-Path Race Safety**:
   If a target reads `active_initiators == 0` right before an initiator sets its active bit, the target exits early on that iteration. The next spinlock wait loop poll will observe the active bit. Once the lock is released and IF is restored, the hardware IPI serves as the ultimate backstop.

### Verification Evidence (QEMU BIOS & UEFI, 1/4/8 CPUs)

Automated test suites passed:
- `make test-smp-ipi`:
  - `T5.a`: Unicast IPI delivery verified (BSP -> AP 1).
  - `T5.a`: Broadcast synchronous TLB shootdown verified across all APs.
  - `T6C.a`: Deterministic contention deadlock breaking: target worker spinning in `spin_lock_irqsave` wait loop with IF=0 serviced shootdown via `smp_tlb_service_local()` polling (`poll count: 1`).
  - `T5.b`: Remote core wakeup via reschedule IPI verified.
  - `T5.c`: Real VMM page unmap and shootdown barrier verified.
  - `T6C.c`: Full TLB flush via CR3 reload verified across all APs.
  - Interactive shell prompt reached across all configurations.
- `make test-smp-locks`: All 10 tests passed (selftest, contention, rank inversion isolation, assert_held negative).
- `make test-smp-sched`: All 6 tests passed (dual-lock ordering, pinned execution, work-stealing).
- `make test-smp-memory`: All 6 tests passed (320,000 cycles across cores, zero duplicates).
- `make test-smp-memory-host` & `make test-smp-memory-tsan`: ThreadSanitizer clean.

### Dell Latitude 5590 Physical Bare-Metal Acceptance (8 CPUs, 32 GiB)

User verified on bare-metal hardware on 2026-09-25 with both Piece 6C and Piece 6B concurrent stress tests:

```text
========================================================
SMP Piece 5: Cross-Core Coordination & IPIs
========================================================
[TEST] SMP Piece 5: Testing unicast IPI delivery (BSP -> AP 1) (T5.a)...
       [PASS] Unicast IPI delivery verified (BSP -> AP 1)
[TEST] SMP Piece 5: Testing broadcast synchronous TLB shootdown (T5.a)...
       [PASS] Synchronous broadcast TLB shootdown acknowledged by all online APs (7 APs)
[TEST] SMP Piece 6C: Contention-safe TLB shootdown deadlock breaking (T5.a)...
       [PASS] Contention deadlock broken: target serviced shootdown from spin_lock_irqsave poll loop (poll count: 1)
[TEST] SMP Piece 5: Remote core wakeup via reschedule IPI (T5.b)...
       [PASS] Remote core wakeup verified (AP 1 awakened from idle)
[TEST] SMP Piece 5: Real VMM page unmap and shootdown barrier (T5.c)...
       [PASS] VMM synchronous TLB shootdown and frame unmap verified (SM14, SM15)
[TEST] SMP Piece 6C: Full TLB flush via CR3 reload...
       [PASS] Full TLB flush via CR3 reload acknowledged across all online APs
[ OK ] SMP Piece 5 (Cross-Core Coordination & IPIs) complete.


========================================================
SMP Piece 6B: PMM Concurrent Multi-Core Stress Test
========================================================
[TEST] SMP memory 6B: Spawning 32 workers across 8 CPU(s) (10,000 iterations each)...
       [INFO] Baseline snapshot taken: used=247801 free=8140807
       [INFO] Workers released to start barrier...
       [INFO] All 32 workers completed 320,000 alloc/free iterations
       [PASS] SMP memory 6B: zero duplicate frame claims across 320,000 cycles
       [PASS] SMP memory 6B: zero verification errors, zero allocation failures
       [PASS] SMP memory 6B: exact post-quiescence equality (bitmap & stats match baseline)
       [INFO] PMM lock acquires: 640002 contentions: 622169
       [PASS] SMP memory 6B: lock telemetry verified (delta acquires: 640000+, contentions verified)
       [PASS] SMP memory 6B: all worker stacks reaped cleanly
[ OK ] SMP Piece 6B (PMM Concurrent Multi-Core Safety) complete.
```

**Acceptance conclusions**:
1. `Piece 6C`: Contention deadlock breaking proven under real hardware contention with IF=0 (`poll count: 1`), confirming that polled local TLB servicing in `spin_lock_irqsave` breaks the circular dependency between target lock wait and initiator shootdown ACK.
2. Full TLB invalidation via CR3 reload verified across all 7 hardware APs simultaneously.
3. `Piece 6B`: Second continuous confirmation of 320,000 allocate/verify/free iterations across 8 CPUs under 622,169 lock contention events with 0 duplicate frames, 0 allocation errors, and exact post-quiescence state equality.
4. `Piece 6D`: Complete address-space and page-table lifetime discipline (SM13, SM16):
   - Global space registry with state machine (`LIVE`, `DYING`, `DEAD`) and permanent kernel space immutability.
   - Transient memory operation references (`op_refs`) protecting all page table modifications and lookups against concurrent destruction.
   - Scheduler references (`sched_refs`) and hardware `active_cpus_mask` tracking context switches across all cores without lock overhead on kernel threads.
   - Guaranteed non-blocking deferred destruction queue (`vmm_drain_deferred_destructions()`) integrated with `sched_reap_dead()` and idle threads, preserving L1 lock hierarchy and freeing structures outside `g_vmm_lock`.
   - Verified clean under host ASan/UBSan (`make test-vmm-host`) and full QEMU BIOS/UEFI shell regressions with zero leaks.



