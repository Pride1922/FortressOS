# Dell spawn subphase results — 2026-10-09

User-exported evidence: `build/dell-spawn-results`. All ten benchmark
invocations have complete worker/barrier records and `ok=1 short=0` summaries.
Six profiled invocations (30 timed cohorts, excluding warmups) pass spawn
partitions, ELF containment, call counts, pipe trailer and startup validation.
The guest tool hash matches the new frozen bundle:
`d0d8b19aa7892407e8d71006da7ce576043194436215e7ac4bc474978601bcc5`.
Eight CPUs are online; invariant TSC is calibrated near 1.90 GHz. Filename
typos in the lock snapshots do not prevent their identification by contents.
Original exported files are preserved unchanged.

## Spawn/wait

One-worker profiled median is 13.617 ms for 25 spawn/wait pairs. Eight-worker
plain medians are 34.922 and 33.618 ms; profiled medians are 35.463 and
35.630 ms. These are instrumentation modes of one build, not old/new fixes.

The table averages each timed cohort's critical worker. Eight-worker entries
cover ten profiled cohorts, one-worker entries five. Each worker makes 25
spawns. Phase values accumulate across those calls, not per individual spawn.

| Interval | One worker mean | Eight workers critical-worker mean |
| --- | --- | --- |
| Whole worker | 13.594 ms | 35.703 ms |
| Kernel spawn body | 12.979 ms | 22.582 ms |
| ELF loader | 11.774 ms | 12.144 ms |
| Kernel-stack allocation | 0.582 ms | 8.582 ms |
| Spawn-path reaping | 0.195 ms | 1.039 ms |
| TCB setup | 0.254 ms | 0.449 ms |
| Publication/context setup | 0.062 ms | 0.177 ms |

Kernel-stack allocation grows approximately 14.7x while ELF elapsed time
changes little. It accounts for 38.0% of kernel spawn-body time at eight
workers, versus 4.5% at one. This is the clearest measured extra cost under
concurrency, even though ELF remains the largest absolute spawn subphase
(53.8% at eight workers). ELF subcounters average copying/zeroing 7.319 ms,
mapping 3.991 ms, root creation 0.638 ms and leaf-frame allocation 0.129 ms.
Mapping includes internal table allocations; copying includes zeroing and
preemption, so these are not pure bandwidth or allocator CPU benchmarks.

Code inspection shows `kstack_alloc` maps its four usable pages through four
separate `vmm_map_page` calls. Each successful active kernel-root mapping
performs a global TLB shootdown. Stack release already uses a batch unmap.
This makes **batching the four stack mappings into one invalidation** the
first targeted optimization experiment, while preserving page ownership,
guard-page protection, rollback and shootdown completion before stack use.
The current stage timer does not split slot reservation, allocation, mapping
and shootdown waits, so the IPI mechanism is a supported hypothesis, not a
proven complete attribution of the 8.582 ms.

Aggregate snapshot deltas show 35,052 additional TLB calls, 35,052 remote
batches, 245,364 target CPUs and about 6.408 billion wait cycles. At this
TSC calibration the last value is roughly 3.38 seconds of summed caller
wait intervals across the session/CPUs, **not wall-clock runtime** and not
an isolated spawn-only measurement. It supports examining shootdown frequency
but must not be added to critical-worker durations.

## Pipes

Single-worker profiled median is 5.971 ms. Eight-worker plain medians are
18.624 and 19.252 ms; profile medians are 15.366 and 18.915 ms. The paired
spread prevents claiming profiling is cost-free.

Across ten profiled critical workers, mean total duration is 17.336 ms:
payload reads 7.510 ms, initial header read 6.499 ms, checksum computation
1.431 ms, setup 1.065 ms and writer spawn 0.773 ms. Writer spawn's kernel
body averages 0.763 ms, including only 0.081 ms in kernel-stack allocation.
This workload's critical path is different from repeated spawn/wait.

Single-worker writers wait about 2 microseconds from publication to first
selection and about 5 microseconds from selection to the sampled user entry.
At eight workers, five of ten critical peers wait 6.096–6.746 ms before
first selection. The other five wait 2–28 microseconds, yet several associated
reader header calls still take 6–7 ms. Selection-to-entry remains about
5–6 microseconds. Thus expensive context/CR3 transition is not supported as
the dominant observed startup cost. Queued writer delay and reader/header
completion delay both occur; these overlapping intervals are not additive.

The next scheduler investigation should measure reader wake-to-selection
alongside writer queue-to-selection. These traces do not justify changing
the time quantum, pinning policy or wakeup semantics without that evidence.

## Decision and next experiment

Prioritize a bounded, failure-safe **kernel-stack batch mapping** experiment
for spawn/wait, then interleave the current frozen baseline and modified
image on Dell with the same capture method. Verify stack guards, allocation
failure cleanup, mappings and global invalidation in host/QEMU tests first.
Retain this build as the baseline; no optimization or speedup is implemented
or claimed by this analysis. Pipe scheduling attribution remains a separate
follow-up and should not be inferred from aggregate VMM contention.

USB collection remains outside worker timing but changes cohort spacing;
power, model and firmware details were not captured in the exported metadata.
One physical session establishes phase evidence, not cross-session variance
or an optimization effect. Single-worker means are with all CPUs online.
Validated analysis is saved in `build/dell-spawn-analysis-20261009.json`,
with complete-run summaries in `build/dell-spawn-validation-20261009.json`.
