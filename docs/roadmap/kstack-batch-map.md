# Kernel-stack batch mapping — 2026-10-09

Implemented after the [Dell subphase results](smpbench-spawn-dell-results.md)
identified kernel-stack allocation as the largest added spawn cost under
concurrency. **Hardware performance comparison is pending.** No QEMU timing
is claimed as an optimization effect.

## Change and contracts

`kstack_alloc` reserves a slot, drops its lock, allocates all four data frames,
then calls `vmm_map_pages` once. The 4 KiB guard remains absent and the usable
16 KiB stack remains supervisor/writable/NX. A successful creation replaces
four single-page shootdown rounds with one four-page round. Release retains
the existing batch unmap. No stack is used or task published until the mapping
call completes its synchronous invalidation.

The new VMM API is intentionally bounded: 1–16 contiguous virtual pages
within a single 2 MiB leaf-PT region, distinct nonzero aligned physical frames,
canonical range and no HUGE/GLOBAL flags. All current stack slots fit this
constraint. Kernel user-permission requests, overflow and boundary crossings
are rejected. It does not replace the general single-page API.

Under `g_vmm_lock`, acquire an operation lifetime reference and preflight the
entire destination. Reserve/zero the at-most-three missing table frames
outside the lock, then repeat preflight after reacquiring it. A competing
mapping can reject the batch or reduce the reserved suffix requirement.
Reclaim unused spares outside the lock. Once preflight and reservation pass,
installation has no allocation/fallible step: publish the hierarchy and all
leaf entries with release stores under one lock. Lock-free readers can still
observe individual stores; this is not an atomic range snapshot API.

Validation/OOM rejects all leaves, with no partial hierarchy created by this
operation. Data frames remain caller-owned on failure. The stack caller frees
every reserved data frame and releases its bitmap slot. On success, release
the VMM lock before the existing synchronous range shootdown and retain the
operation pin until it completes. The existing kernel-root path targets all
online CPUs; private never-loaded roots keep their existing no-flush behavior.
No lock rank, IRQ/switch, CR3, boot initialization or storage contract changes.

## Actual validation

- `make test-kstack-batch-host`: actual VMM under ASan/UBSan with PMM allocation
  cut points 0/1/2, exact allocation-set/table counts, collision and competing
  mapper, supported permissions/guard, huge-prefix rejection, address/count/
  frame boundaries, inactive/active invalidation and operation-reference checks.
  Actual stack caller functions are extracted from `thread.c` and compiled
  with mocked PMM/VMM adapters: data-frame cuts 0/1/2/3, mapping rejection,
  unchanged failure outputs, complete cleanup, slot exhaustion and slot 63
  allocation/free pass. Host adapters do not prove real IRQ/IPI behavior.
- `make test-smpbench-profile-host`: actual benchmark/writer/formatter sanitizer
  adapters and evidence rejection tests pass.
- `python3 scripts/test_smp_vmm.py --cpus 1 4 8 --firmware bios uefi --timeout 120`:
  all six ISO-only 2 GiB QEMU cases pass 100 spawn/exit cycles, guard/lifecycle
  assertions, exact physical allocation set and table-frame equality, zero
  deferred destructions and full stack-slot reclamation. No data disk attached.
  Evidence retained in `build/smp-vmm-lifecycle-{bios,uefi}-{1,4,8}-2G.log` and
  accompanying argv/stderr artifacts.
- BIOS/UEFI SMP=8 profiled spawn_wait/pipes: one warmup plus two timed reps
  each passes readiness, phases, spawn partitions and writer startup, with
  panic capture armed and no panic observed. Evidence:
  `build/kstack-batch-{bios,uefi}-20261009`.
- Fresh control A BIOS SMP=8 profile smoke passes both workloads (warmup plus
  one timed rep); `build/kstack-control-A-20261009`.
- Both frozen A and B raw images boot under UEFI USB SMP=1, pass real profiled
  signals/pipes, persist and independently validate a >4096-byte `/mnt` log,
  cleanly shut down and pass offline `e2fsck -fn`. Disposable copies only,
  source images unchanged; `build/kstack-usb-{A,B}-20261009`.

Strict freestanding build and diff whitespace checks pass. No user syscall,
entry assembly or benchmark binary change was needed for this optimization.

## Matched Dell comparison bundle

`build/kstack-ab-ready-20261009` contains verified raw images and matching
ISO/symbols, common object snapshots, common initramfs and benchmark, isolated
thread source/object variants, source diff and hash manifests. The packager
never modifies shared `bin`/object outputs or writes a physical device.
`make prepare-kstack-comparison` creates another timestamped bundle.

- A control: four single-page mappings, four shootdown rounds.
- B candidate: one batch mapping, one four-page shootdown round.

Both preallocate all four data frames, use the same batch stack release and
contain the same VMM batch API and profiling. Only the compiled stack mapper
call path and its failure rollback differ; all other linked object bytes and
initramfs bytes are identical. A is a fresh controlled comparison, **not** the
previous image with per-page allocation interleaved with mapping. This isolates
batching from other workspace changes and from data-frame reservation policy.
B's relinked kernel hash was verified against the normal workspace build.
Raw/ISO embedded kernel/initramfs hashes and filesystem structures are checked.

Image SHA-256:

| Variant | Image hash |
| --- | --- |
| A | `4c0ada4685b73a5c045b583070d7b40ef00a91360b797b966716225cd34dca43` |
| B | `01780d78b12d3d98d7f017f55ba28c335aa1e327c4f1d27f3621d0c24df5f70a` |

Run A1 → B1 → B2 → A2 with AC power, same firmware/background work and
consistent thermal conditions. Use the bundle's same `commands.txt` on each
boot. It records metadata/lock counters, seven-repetition plain spawn_wait,
five-repetition profiled spawn_wait and plain/profile pipes. Every full log
goes to `/mnt`; sync and shut down before exporting. Preserve each export in
`build/kstack-dell-results/{A1,B1,B2,A2}` before reboot/reflash overwrites it.
Reboot the existing B image for B2, then reflash A for A2.

Use plain throughput/durations for the effect comparison, profiles for
attribution and aggregate TLB deltas as supporting evidence. Count every
repetition, keep both sessions per variant and report variability/order
effects; if effects are small or inconsistent, more interleaved boots are
needed. USB collection is outside worker timing but changes cohort spacing.
The subsequent Dell ABBA evidence is recorded below.

## Dell ABBA results — 2026-10-09

User exported all four boots to `build/kstack-dell-results/{A1,B1,B2,A2}`.
All 16 workload logs pass readiness/worker completeness checks, report eight
online CPUs, `ok=1`, `short=0`, and the expected common benchmark SHA-256.
All eight profile logs also pass phase, spawn and writer startup accounting.
Warmups are excluded: 48 plain and 40 profiled timed cohorts are retained.
Kernel variant provenance relies on the user's image/boot order and export
folder labels; the shared benchmark hash alone cannot identify A versus B.

Plain median duration, milliseconds (lower is better):

| Workload | A1 | B1 | B2 | A2 |
| --- | ---: | ---: | ---: | ---: |
| spawn_wait | 35.816 | 29.390 | 30.345 | 35.994 |
| pipes | 19.134 | 15.448 | 15.254 | 15.347 |

Pooling all 14 plain spawn cohorts per variant gives median 35.905 ms for A
and 29.5735 ms for B: **17.6% lower duration** (21.4% higher throughput for
the same fixed work). Both B boot medians beat both A boot medians. Profiled
pooled spawn medians also improve, 36.5345 to 30.544 ms (16.4%). This supports
retaining the batch stack mapper for this tested eight-worker workload; it
does not establish scaling at other CPU counts or a general workload gain.

For the slowest worker of each profiled spawn cohort, mean inclusive kernel
stack allocation time across ten cohorts per variant falls from 9.153 to
3.128 ms per 25 spawns: **65.8% lower**, approximately 366 to 125 microseconds
per spawn. ELF loading stays near 12.1–12.3 ms, and the worker spawn phase
falls from 23.582 to 17.838 ms. These are elapsed intervals including lock,
IPI and scheduling delays, not isolated CPU execution times. The remaining
ELF and wait costs explain why whole-workload improvement is smaller.

Pipes is inconclusive: the pooled plain median improves 19.131 to 15.370 ms,
but A2 already matches the B boots. Profiled pooled medians change only
19.4105 to 19.1205 ms (1.5%). Do not attribute a reliable pipe speedup to
stack batching from this four-boot experiment.

A1's before/after TLB snapshots differ by only six calls despite complete
benchmark logs, with its before snapshot already showing 29,220 remote
batches. Its baseline appears late/replaced and is excluded from interval
counter comparisons. The valid A2 interval has 28,958 remote batches;
B1/B2 have 9,569/9,567, approximately 67% fewer. This supports the expected
four-to-one stack-map shootdown reduction but is aggregate session evidence,
not a per-workload measurement. Cumulative `max_spin` is never subtracted.

Reproducible analysis and raw-file hashes:
`build/analyze_kstack_dell.py`, `build/kstack-dell-validation.json`, and
`build/kstack-dell-profile-analysis.json`. All repetitions are retained.
Next attribution target for spawn is ELF mapping/copying; for pipes, measure
reader wake-to-selection delay before changing scheduler policy. More ABBA
boots would be needed to resolve small or inconsistent effects.
