# Memory investigation

Started 2026-10-09 at commit `26cd041`. SMP performance tuning is paused.
Preserve VMM lifetime pins, atomic page-table entry access, deferred teardown,
acknowledged invalidation and scheduler context handoff. Preserve the unrelated
permissions-plan edits. Normal images use journaled EXT4; ext2 requires explicit
legacy selection. No allocator redesign or reclamation-policy change is part
of this initial investigation.

## Sequence and acceptance

1. Establish baseline: record commit, dirty status, hashes of kernel/ISO/image
   and the permissions plan. Restore the PMM boot host runner without pretending
   to execute its discarded process-lifecycle sections.
2. Reproduce defects: execute actual heap code under ASan/UBSan, with explicitly
   synthetic PMM/VMM and checked single-threaded locks. Preserve failures for
   resize arithmetic overflow and shrink live-block accounting.
3. Correct only demonstrated errors: reject oversized resize before mutation;
   account the synthetic shrink remainder; independently audit live blocks.
   Require payload preservation, growth/relocation/coalescing and kernel boot
   assertions. Add coherent heap snapshots for diagnostic comparisons.
4. Verify reclamation and OOM: exercise every cut in a five-page heap expansion,
   frame-budget exhaustion and the expansion cap. Separate retained tables from
   data-frame leaks. Make PMM audit count actual bitmap state under rank 4,
   reconcile counters and verify reservations, with logging after unlock.
5. Characterize bounded pressure: deterministic mixed-size heap operations and
   an exhausted PMM with alternating free holes. Record largest payload/run,
   committed backing, free capacity and failure/scan counters. This establishes
   specific mechanisms, not workload performance or a need to redesign.
6. Check journaled EXT4 independently: exclusive disposable production USB
   image, BIOS/UEFI, four CPUs; warm the reusable node and transaction paths,
   perform ten write/read/truncate/close/sync cycles, compare exact bitmap,
   coherent heap metrics, tables, deferred count and mapping fingerprint.
   Follow with Ring 3 persistence/reboot, independent Linux bytes/fsck and
   journal-feature checks. No physical or ext2-to-EXT4 performance inference.

## Automation

Run from PowerShell:

```powershell
wsl -d Ubuntu-24.04 -- make test-memory
```

The runner retains a unique `build/memory-*/result.json`, commands, return
codes, hashes and per-stage logs, including failed runs. Host fixtures run
first, then normal journaled-EXT4 build, VMM BIOS/UEFI at 1/4/8 CPUs, boot
ceiling checks at 256 MiB with 1/4 CPUs, and disposable EXT4 audits. Existing
memory runners retain detailed UART logs under `build/smp-*`; EXT4 logs,
QEMU argv, copies and Linux audits live inside the campaign directory.

For a quick host-only run:

```powershell
wsl -d Ubuntu-24.04 -- python3 scripts/test_memory_investigation.py --host-only
```

## Evidence limits and next decisions

Host heap adapters do not model hardware page walks, TLB acknowledgments or
SMP exclusion. Actual VMM host coverage and QEMU lifecycle regressions supply
separate evidence. The EXT4 memory comparison runs synchronous kernel VFS calls
before shell startup; Ring 3 persistence and process-lifetime tests are separate.
Warm baselines include intentional heap/table/cache retention. Matching bitmap
state does not prove all ownership relationships.

After this bounded correctness gate, choose the next investigation from
measured failure causes: large allocation requirements, retained heap backing
under system-wide pressure, filesystem node-cache growth, or allocation latency
under representative workloads. Do not optimize based on synthetic timing.
Dell testing is needed only for a specific unresolved hardware question.

## Retention follow-up

The host namespace churn and BIOS/UEFI QEMU reproduction are complete. Run
`make test-ext4-memory-churn-host` for the sanitizer/independent Linux checks,
or `make test-memory-churn` for the four-boot production USB campaign. Both
retain unique evidence directories; neither writes the shared image during
testing or changes node/allocator policy.

The reproduced limit affects both creation and uncached existing-file opens.
Before changing node ownership, discuss the unpinned VFS lookup/callback
lifetime contract. Separately investigate retained heap backing with actual
system-wide PMM pressure in bounded low-memory QEMU. Keep these correctness
and capacity investigations separate from allocation-latency measurements.

The user approved node-lifetime/reclamation implementation after the reproduced
limit. Owned VFS references and bounded journaled EXT4 reclamation are now
implemented; the raw-pointer compatibility API remains mount-lifetime stable.
Use `wsl -d Ubuntu-24.04 -- make test-memory-nodes` to build first, then run
the host churn/pin gate, focused API errors, mounted lifecycle/recovery smoke
and BIOS/UEFI QEMU sequentially on disposable copies. This ordering prevents
a build changing the source image while a test verifies its immutable hash.
Recorded results and the legacy ext2 verification gap are in the report.

## Real PMM pressure

Run `wsl -d Ubuntu-24.04 -- make test-memory-pressure`. The target builds
first, then runs disposable ISO-only BIOS/UEFI boots at 256/512 MiB plus an
unmodified BIOS control. No data disk is attached. Each pressure boot exhausts
PMM before and after a freed 4 MiB heap burst, checks existing heap reuse and
expansion OOM, returns every test-owned frame, and compares exact PMM bitmaps
and heap/table accounting. The baseline is after the normal heap self-tests,
before AP/thread startup. Retained backing is capacity evidence, not a leak
or allocator-latency result. Do not change trimming policy on this synthetic
workload alone; next measure a representative process/allocation burst.

## Representative process bursts and bounded PMM pressure

Run `wsl -d Ubuntu-24.04 -- make test-memory-burst`. The target builds first,
then executes disposable ISO-only BIOS/UEFI boots at 256/512 MiB plus an unmodified
BIOS 256 MiB control. No data disk is attached. The diagnostic runs after AP and
scheduler initialization:
1. Warmed baseline: 4 process spawn/exit cycles warm up stack slots and TCB memory.
2. Burst workload: 32 process spawn/exit cycles across CPUs interleaved with kernel
   heap churn. Draining via `sched_reap_dead()` and `vmm_drain_deferred_destructions()`
   restores live heap used to baseline, with 0 retained heap delta and 0 table delta.
3. Bounded PMM pressure: tests real page consumer process execution under 64-frame
   headroom, heap reuse from retained capacity, and graceful `NULL`/ENOMEM rejection
   under 2-frame headroom.
4. Post-pressure recovery: releases held frames, drains to quiescence, and verifies
   exact PMM bitmap equality, heap stats equality, zero deferred destructions, and
   zero table leaks.
Evidence: `build/memory-burst-20261009T183501817513Z/result.json` (5/5 PASS).
Host coverage: `make test-vmm-host` under ASan/UBSan includes `test_burst_memory_accounting()`.

## Concurrent process cohorts and fragmented PMM headroom

Run `wsl -d Ubuntu-24.04 -- make test-memory-cohort`. The target builds first,
then executes disposable ISO-only BIOS/UEFI boots at 256/512 MiB plus an unmodified
BIOS 256 MiB control without storage. The diagnostic runs after scheduler startup:
1. Warmed baseline: 2 process spawn/exit cycles, full drain to quiescence, snapshot.
2. Concurrent cohorts: sizes 2, 4, 8 executed in 2 passes each. Preemption is disabled
   during staging so all $N$ processes coexist concurrently (verified by stack slot
   bitmask population). Processes are waited on in mixed order. Post-reap quiescence
   verifies exact live heap used bytes restoration. Repeated pass confirms heap
   committed capacity remains stable (`repeat_stable=PASS`, zero growth on repeat).
3. Fragmented PMM: intrusive linked list holds memory down to 128 frames, remaining
   frames allocated into an array, and even frames freed to create 64 isolated
   single-page holes (zero runs $\ge 2$). Proves contiguous allocations fail
   (`pmm_alloc_pages(2) == 0`, `pmm_alloc_pages(4) == 0`) while single-page allocations
   and real Ring 3 process execution succeed cleanly. Tight exhaustion ($\le 2$ frames)
   fails cleanly (`NULL`/ENOMEM). Full release restores contiguous allocations
   (`pmm_alloc_pages(4) != 0`) and post-pressure process creation.
4. Post-pressure recovery: full drain restores exact post-cohort heap stats, table
   frame counts, zero deferred destructions, PMM free pages, and exact quiescent
   PMM bitmap equality.
Evidence: `build/memory-cohort-20261009T190329723217Z/result.json` (5/5 PASS).
Host coverage: `make test-vmm-host` under ASan/UBSan includes `test_concurrent_cohort_and_fragmented_pmm()`.
Evidence boundary qualification: Overlapping process ownership in memory on single-core
QEMU demonstrates concurrent resource coexistence, not simultaneous multicore execution.
Alternating array indices in the fragmentation test do not alone prove physical isolation,
but zero-run multi-page allocation failure (`pmm_alloc_pages(2) == 0`) definitively proves it.

## Process launch allocation failure rollback

Run `wsl -d Ubuntu-24.04 -- make test-memory-rollback`. The target builds first,
then executes disposable ISO-only BIOS/UEFI boots at 256/512 MiB plus an unmodified
BIOS 256 MiB control without attached storage. The diagnostic exercises deterministic
fault injection across 16 distinct cuts spanning every fallible stage of `process_spawn_on_cpu`,
`process_spawn_with_actions`, `load_elf_into_space`, and `kstack_alloc`:
1. PML4 space allocation failure (`SPAWN_FAULT_VMM_USER_PML4`).
2. ELF segment PMM frame allocation failure on page 0 (`SPAWN_FAULT_ELF_SEGMENT_PMM_PAGE0`).
3. ELF segment PMM frame allocation failure on page 1 after page 0 is mapped (`SPAWN_FAULT_ELF_SEGMENT_PMM_PAGE1`).
4. ELF segment page mapping failure on page 0 (`SPAWN_FAULT_ELF_SEGMENT_MAP_PAGE0`).
5. ELF segment page mapping failure on page 1 after page 0 is mapped (`SPAWN_FAULT_ELF_SEGMENT_MAP_PAGE1`).
6. Signal restorer stub PMM frame allocation failure (`SPAWN_FAULT_SIGRESTORER_PMM`).
7. Signal restorer page mapping failure (`SPAWN_FAULT_SIGRESTORER_MAP`).
8. User stack PMM frame allocation failure (`SPAWN_FAULT_USER_STACK_PMM`).
9. User stack page mapping failure (`SPAWN_FAULT_USER_STACK_MAP`).
10. Kernel stack partial PMM frame allocation failure mid-loop after 2 frames (`SPAWN_FAULT_KSTACK_PMM_PARTIAL`).
11. Kernel stack batch mapping failure after 4 frames allocated (`SPAWN_FAULT_KSTACK_MAP_BATCH`).
12. Kernel stack slot bitmap exhaustion failure (`SPAWN_FAULT_KSTACK_SLOT_EXHAUST`).
13. TCB allocation failure (`SPAWN_FAULT_TCB_KMALLOC`).
14. Process file descriptor initialization failure (`SPAWN_FAULT_FD_INIT`).
15. Spawn action failure after partial progress: action 0 opens `/etc/motd` to fd 3, action 1 fails with ENOENT (`SPAWN_FAULT_SPAWN_ACTIONS_PARTIAL`).
16. Scheduler address-space reference acquisition failure (`SPAWN_FAULT_SCHED_REF`).

Verification across all cuts:
- Exact hit tracking verified: `spawn_get_fault_hits() == 1` asserted on production path for each cut.
- Clean failure result (`NULL`/ENOMEM) and zero partially runnable process published.
- Aborted PID tracked (`spawn_get_last_aborted_pid()`), verified dead and not waitable (`!process_is_alive(pid) && !process_wait(pid, ...)`).
- Partial construction rollback verified: multi-page ELF mappings, partial kstack allocations, and open file descriptors cleanly unwound.
- Zero leaked stack slots (`sched_get_active_stack_slots_mask() == initial_stack_mask`).
- Zero deferred destructions (`vmm_get_deferred_count() == 0`).
- Exact baseline heap used bytes, allocated blocks, table frames, and PMM free pages restored after every single cut.
- Subsequent normal Ring 3 process execution and reaping succeeds with exit code 42.
- Full quiescent post-pressure recovery restores exact PMM bitmap equality (`memcmp(before, after) == 0`).
Evidence: `build/memory-rollback-20261009T194049474942Z/result.json` (5/5 PASS).
Host coverage: `make test-vmm-host` under ASan/UBSan includes `test_process_launch_rollback()`.
Host vs. QEMU boundary: `tests/vmm_space_host.c` models VMM frame/table tracking with stubs; production ELF loading, kstack allocation, TCB allocation, descriptors, and PID aborts are verified in QEMU.

## Read-only memory observability

Run `wsl -d Ubuntu-24.04 -- make test-memory-observability`. The target builds first,
then executes disposable ISO-only BIOS/UEFI boots at 256/512 MiB plus an unmodified
BIOS 256 MiB control without attached storage:
1. ABI architecture: `SYS_SYSINFO` (syscall 37, 72 bytes) preserved without mutation.
   Added non-breaking `SYS_MEMINFO = 56` with `sysinfo_mem_t` (96 bytes), verified by
   static assertions.
2. User tool: `/bin/sysinfo` preserves default output format; `/bin/sysinfo -m` provides
   structured memory observability.
3. Metric verification:
   - PMM total, used, free, and allocatable (<1G) frames.
   - Heap live used bytes (including 32B block metadata), reusable free bytes, committed backing bytes, largest free chunk payload, and free block count.
   - VMM page-table frames (kernel + user hierarchy).
   - Deferred teardown queued address spaces (not frames or reclaimable bytes).
4. Snapshot consistency: Subsystem locks are acquired sequentially without global rank nesting.
   Individually coherent counters disclosed as non-globally atomic.
5. Acceptance: Sane metric scaling across RAM geometries, process lifecycle observation
   (`/bin/ps`), and shell recovery verified.
Evidence: `build/memory-observability-20261009T200055706676Z/result.json` (5/5 PASS).
Host coverage: `make test-s9-sysinfo-host` verifies ABI layout, size bounds, invalid pointer rejection, and invariants.

## Next priorities and evidence gates

Continue in the following order. Completion claims above describe the tested
fixtures, not exhaustive production coverage or all normal workloads.

1. Completed: audited launch rollback coverage, confirmed injected failures
   reach production paths exactly once (`hits == 1`), distinguished stage rejection
   from internal partial-allocation rollback (multi-page ELF segment, partial kstack,
   spawn action partial progress), and verified exact accounting per cut. Verified
   host model boundary vs QEMU production integration.
2. Completed: read-only memory observability via non-breaking `SYS_MEMINFO = 56`
   and `/bin/sysinfo -m`. Verified PMM frames, heap bytes and largest payload,
   VMM page-table frames, and queued address spaces. Verified snapshot consistency
   without global lock nesting. Verified host unit tests and 5/5 QEMU acceptance.
3. Completed: audited consumer attribution against code and qualified evidence
   boundaries. PMM bitmap prevents duplicate allocation but does not record owner
   identity; HHDM aliases physical frames into kernel virtual space. Dynamic stack
   multiplier popcount64(slots)*4 overcounts transiently during allocation/teardown
   windows. Reserved bitmap entries include non-RAM physical address holes. DMA sizing
   and quarantine behavior depend on hardware probe, capability registers, and attached
   devices. Disclosed that remainder calculations from sequential snapshots reflect
   in-flight allocations, tearing, or domain mismatches, not leaks, and risk underflow.
   Noted that length-negotiated ABI extension is a proposed future design. No new
   attribution counters or pressure policies introduced.
4. Completed: diagnostic headroom verification and semantic boundaries.
   Verified ceiling expansion (`pmm_allocatable_frames` expands from boot ceiling to
   all free frames after `pmm_unlock_high_memory()`; linked to `tests/pmm_boot_host.c`
   for >1 GiB and 30 GiB verification). Qualified `heap_largest_payload` as the single
   largest existing free block payload (excluding 32B tags) on realizable 16-byte
   aligned boundaries for valid, nonzero, overflow-safe requests at the observed instant.
   Noted coalescing is immediate upon `kfree()`, not a future reclaim pass.
   Linked heap reuse evidence (`tests/heap_memory_host.c` lines 224-231, `src/mm/memory_boot_test.c`)
   and PMM fragmentation limits (`src/mm/memory_boot_test.c` lines 1350-1370).
   Verified via `tests/sysinfo_host.c` and 5/5 QEMU acceptance (`make test-memory-observability`).
   No new allocator feature (caches, trimming) is justified.
5. Completed: measured SMP profiling of kernel stack allocation & free lifecycle at
   1, 4, and 8 CPUs. Bounded, repeatable workload executed on pinned workers with
   ordered TSC tick accounting (`lfence; rdtsc; lfence`), phased coordinator gates (ready/start/done/term),
   clean isolation of setup stack allocations, repeated 3-trial matrices comparing
   profiled vs. matching untracked gated controls (`smp_memory_test=kstack_control`), same-scope sub-interval decomposition,
   two-layer lifecycle reconciliation, explicit non-overlap verification, and exact post-quiescence resource cleanup
   (heap, tables, PMM bitmap equality).
   
   **Superseded Earlier Claims Marked:**
   - Earlier claims that "VMM operations dominate 80–86% of lifecycle time" and "synchronous TLB shootdowns explain the majority of lifecycle cost" are explicitly marked **SUPERSEDED** by full-lifecycle partition accounting: at 8 CPUs, shootdowns account for 28.2–29.0% (median 28.9%) of total stack lifecycle time, and VMM lock wait/hold accounts for only ~5.5% combined (wait 3.3%, hold 2.3%).
   - Earlier hypothesis that "stack-touch memory writes explain the lifecycle residual" is **SUPERSEDED**: stack memory writes execute strictly outside measured intervals (between $t_{\text{alloc\_end}}$ and $t_{\text{free\_start}}$).
   
   **Two-Layer Reconciliation Model & Findings:**
   - **Layer 1: Continuous Non-Overlapping Lifecycle Partition**:
     Complete, contiguous timestamp coverage where every sub-interval begins exactly where the previous sub-interval ended:
     - Allocation: `slot_alloc_wait` + `slot_alloc_hold` + `alloc_prep` + `pmm_alloc` + `map_prep` + `vmm_map_pre_lock` + `vmm_map_wait` + `vmm_map_hold` + `vmm_map_post_prep` + `vmm_map_put_op_wait` + `vmm_map_put_op_hold` + `map_dispatch` + `map_ack_poll` + `map_service` + `alloc_tail`.
     - Free: `unmap_pre_lock` + `unmap_wait` + `unmap_hold` + `unmap_post_prep` + `unmap_put_op_wait` + `unmap_put_op_hold` + `unmap_dispatch` + `unmap_ack_poll` + `unmap_service` + `free_mid` + `pmm_free` + `free_tail` + `slot_free_wait` + `slot_free_hold`.
     - Non-overlap and monotonic containment strictly verified per run ($t_{\text{alloc}} \ge \text{alloc\_partition}$, $t_{\text{free}} \ge \text{free\_partition}$).
     - Component Distributions (median % across 3 runs from `build/smp-memory-profile-20261010T100909874884Z/result.json`):
       - **1 CPU**: PMM = 25.1%, VMM lock hold = 20.4%, VMM misc = 8.5% (pre-lock 1.5%, post-prep 4.0%, put-op wait 1.9%, put-op hold 1.0%), Wrappers = 10.0%, KStack lock wait/hold = 7.0%, VMM lock wait = 3.3%, TLB shootdown = 0.0%, Unmeasured Residual = 25.7%.
         - Outer-vs-Inner Breakdown: Invocation gap = **7.3%**, Internal sub-interval gap = **18.6%**.
       - **4 CPUs**: TLB shootdown = 39.6% (dispatch 27.8%, ack poll 58.7%, service 13.5%), PMM = 13.1%, VMM misc = 11.8% (pre-lock 1.2%, post-prep 4.4%, put-op wait 5.4%, put-op hold 0.5%), VMM lock hold = 7.6%, VMM lock wait = 5.2%, Wrappers = 3.3%, KStack lock wait/hold = 2.3%, Unmeasured Residual = **16.0%** (range: 15.8%–19.5%).
         - Outer-vs-Inner Breakdown: Invocation gap = **3.2%**, Internal sub-interval gap = **12.6%**.
       - **8 CPUs**: TLB shootdown = 26.3% (dispatch 24.1%, ack poll 46.9%, service 29.0%), VMM misc = 17.0% (pre-lock 0.5%, post-prep 10.5%, put-op wait 5.7%, put-op hold 0.3%), PMM = 13.1%, VMM lock wait = 4.9%, VMM lock hold = 2.9%, Wrappers = 1.5%, KStack lock wait/hold = 1.1%, Unmeasured Residual = **31.9%** (range: 29.1%–35.8%).
         - Outer-vs-Inner Breakdown: Invocation gap = **1.5%**, Internal sub-interval gap = **30.4%**.
       - Page-table traversal explains 78–83% of VMM lock hold time (`vmm_map_pt` + `unmap_pt`).
       - Secondary contention on `g_vmm_lock` during `vmm_space_put_op` (`vmm_put_op_wait`) accounts for 5.4%–5.9% of lifecycle duration across 4 and 8 CPUs.
   - **Layer 2: Nested Interrupt Overlay (Remote IPI Servicing)**:
     - Tracked independently per CPU via ordered TSC reads in `smp_ipi_tlb_handler` (`g_ipi_tlb_service_tsc`) and snapshotted at coordinator start/done gates.
     - Never added into the Layer 1 partition sum. Remote IPI servicing accounts for **3.2%–4.1% of lifecycle duration at 8 CPUs** (5,908–6,055 IPIs per run, 61.2M–64.8M ticks) and **3.7%–5.6% at 4 CPUs** (1,909–1,983 IPIs, 10.2M–10.7M ticks).
   - **Instrumentation Overhead on Matching Gated Workload Intervals**:
     - Both profiled runs and untracked controls execute the identical 50-iteration worker loop (alloc, touch top/bot, free).
     - With unconditional servicing retained in the polling loop (servicing active on every spin, timestamp clocking gated only when peer requests exist), gated overhead is **+4.46% at 8 CPUs** (profiled gated median 150.5M vs control 144.1M ticks). At 4 CPUs, gated TSC variance across repetitions shows -10.56% (control median 73.7M vs profiled 65.9M ticks, wall overhead +5.66%), explicitly demonstrating that three repetitions under host TCG scheduling exhibit concurrency variance; single-point estimates (such as earlier +0.74%) do not prove absence of perturbation without broader statistical distributions.
   - **Auditing Residual & Three Explicit Hypotheses**:
     - *Empirical Outer vs. Inner Audit*: Measuring the exact entry/exit timestamps of `kstack_alloc_tracked` and `kstack_free_tracked` (`inner_tsc`) proves that outer invocation/return overhead accounts for only **0.9%–2.1% (median 1.5%) of lifecycle time at 8 CPUs**, and **1.6%–3.4% (median 3.2%) at 4 CPUs**. The remaining unmeasured residual (median **30.4% at 8 CPUs**, **12.6% at 4 CPUs**) resides *inside* the execution boundaries between the constituent sub-intervals (e.g. gaps between PMM allocation, page-table mapping, op-reference release, and slot unlinking), rather than function call setup/teardown.
     - *Hypothesis 1 (Measurement Perturbation & TCG Variance)*: Three repetitions under QEMU MTTCG are insufficient to establish a single precise percentage for diagnostic perturbation. Gated overhead varies from -10% to +15% depending on host scheduling of vCPU threads and lock competition.
     - *Hypothesis 2 (Host Scheduling Skew under MTTCG)*: QEMU Multi-Threaded TCG executes vCPUs on host OS threads. Spin durations in ACK polling (47%–59% of shootdown time) and peer service times are influenced by host thread preemption, meaning TCG measurements reflect host scheduling stretch rather than bare-metal hardware bus latency.
     - *Hypothesis 3 (Internal Sub-interval Residual Attribution)*: The internal gap between tracked sub-intervals (30.4% at 8 CPUs) is hypothesized to arise from compiler register saving/spilling across C function boundaries, loop overhead inside batch allocators, and asynchronous interrupt handling occurring during uninstrumented code windows between timed blocks. It cannot be assumed that compiler function entry/exit explains the residual.
   - Evidence: `build/smp-memory-profile-20261010T100909874884Z/result.json` (18/18 PASS). Exact post-quiescence resource equality (heap, tables, PMM free pages, deferred list 0, and exact PMM bitmap equality) verified on every run.
   - Conclusion: Secondary VMM lock wait is a confirmed measured contributor. Synchronous shootdowns represent a substantial concurrent component (25%–40%), but do NOT dominate kernel stack cost. Neither per-CPU PMM page caches nor heap trimming address the measured scaling behavior. Keep allocator policies unchanged. Defer Dell Latitude 5590 physical tests until a targeted physical shootdown latency question is formulated.

Deferred proposals:

- Per-CPU page caches: SMP tuning remains paused. Measure PMM lock wait/hold time,
  allocation frequency and refill opportunities before proposing a change. VMM
  contention alone does not identify PMM as its cause. A future design must cover
  cached-frame ownership/accounting, cross-CPU frees, pressure drainage, boot and
  IRQ/migration safety, and contiguous allocation without stranded capacity.
- Explicit `heap_trim()`: retain as a possible bounded primitive, not an approved
  implementation. Capacity benefit is demonstrated, but representative demand
  and safe tail metadata, unmapping, TLB invalidation and frame-release ordering
  must justify and constrain it. Manual invocation does not remove these risks.
- Broader TLB shootdown batching: defer until measured shootdown frequency/cost
  warrants reopening SMP work. Preserve lifetime pins and invalidation-before-
  reclamation guarantees; batching across calls is not just per-call batching.

Use actual-code host sanitizer tests and disposable QEMU first, with one simple
command and retained machine-readable evidence. Keep correctness findings separate
from capacity and performance hypotheses. No allocator redesign is authorized by
this plan. Normal images remain journaled EXT4; do not transfer ext2 benchmark
conclusions to EXT4. Dell testing is reserved for a specific hardware evidence gap.
Leave unrelated permissions-plan edits untouched. Subsequent handoff prompts
should follow this order and carry forward these evidence boundaries.



