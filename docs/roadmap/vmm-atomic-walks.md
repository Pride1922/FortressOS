# VMM atomic walks and shorter mapping critical sections (2026-10-08)

User-authorized completion of the interrupted VMM contention work. Existing
permissions-plan and benchmark changes in the working tree were preserved.

## Implemented behavior

- Runtime table installation and leaf map/unmap use release atomic stores.
  Intermediate permission upgrades use atomic fetch-or to preserve hardware
  accessed/dirty bits. Readers take one acquire snapshot per level.
- Intermediate tables remain linked until whole-address-space teardown.
  Generic readers acquire/release existing op_refs, with no lock across the walk.
  Own-task user-range validation uses immutable cached space metadata pinned by
  the task's scheduler reference, eliminating VMM acquisitions on the syscall
  buffer-validation path. Retirement is still acquire-observed and rejected.
- Mapping counts missing tables under the VMM lock, allocates and zeroes that
  suffix outside the lock, and rechecks before installation. Unused speculative
  frames are returned outside the lock. Partial allocation failure publishes no
  hierarchy. Root allocation/zeroing also occurs outside the VMM lock.
- Map and unmap synchronously invalidate after dropping the VMM lock when
  the address space may have cached translations. Never-loaded private roots
  skip invalidation; a sticky `ever_active` bit is set before scheduler or raw
  fixture CR3 entry. Shared kernel mappings always invalidate.
  User-space op_refs remain held through acknowledgement. Higher-half mappings
  broadcast because their table hierarchy is shared by all address spaces.
- Parked APs poll the existing lock-free TLB acknowledgement service until the
  scheduler release gate. This is necessary for the BSP's AP-idle-stack mapping
  shootdowns while AP interrupts remain disabled.
- Teardown validates and detaches eligible spaces under the registry lock,
  then invalidates and frees their exclusively owned frames outside it.
  Invalid structures retain registry/deferred ownership. Frame counters use
  atomics because independent reclamation can now overlap.
- `vmm_unmap_pages` validates up to 16 contiguous non-global leaves before
  changing any entry, removes them under one lock acquisition, and retains
  its operation reference through one synchronous range invalidation. The
  mailbox carries a bounded page count; each CPU uses INVLPG on those pages
  before acknowledging, retaining unrelated translations. Kernel stack
  reclamation uses it for four pages, freeing frames and releasing the stack
  slot only after ACK completion. Missing/global leaves fail without mutation.
- The existing self-signal wake fast path was retained. Pipes now use separate
  reader and writer channels, broadcasting within the affected direction and
  both directions on endpoint closure. Arbitrary wake-one was rejected: it
  can select the wrong/ineligible peer, and killing or stopping the selected
  peer can strand others before resource consumption.
- `lockstat` includes cumulative TLB calls, remote batches, target CPU counts,
  ACK spin iterations and ACK-wait TSC cycles. The benchmark runner captures
  workload deltas, all per-repetition/per-worker records, ISO hashes and serial
  logs, checkpoints completed workloads, retains failures, and rejects log
  overwrites. Max spin is a lifetime maximum and is not subtracted.
- Workers record coarse guest scheduled ticks through `SYS_PROCINFO`'s self
  selector (the existing ABI structure and syscall number remain unchanged).
  Direct PID lookup avoids enumeration shifts while other processes exit.

## Verification

- `make -j4`: full kernel, ISO and raw-image build passed; later edits rebuilt
  the kernel/ISO. The final raw image is regenerated at completion.
- `make test-vmm-host`: ASan/UBSan with real pthread serialization of VMM
  metadata, four concurrent map/unmap/walk workers, exact frame reclamation,
  permission/cross-page/overflow boundaries, zero VMM acquisitions for 1,000
  own-task validations, DYING rejection, all three table-allocation OOM cuts,
  and a deterministic competing-mapper installation/unused-spare cleanup case.
  PMM allocation and shootdown shims assert the VMM lock is not held; user-root
  shootdowns assert an operation reference remains held.
- `make test-pipe-host`: pipe and SIGPIPE host fixtures passed.
- `python3 scripts/test_pipe.py`: BIOS/UEFI, one CPU, disposable ISO/no data
  disks; blocking/EOF/EPIPE/CLOEXEC/reclamation and Ring 3 ABI cases passed.
- `SMP=4 python3 scripts/test_s8_process.py --signals`: BIOS/UEFI, four CPUs,
  disposable ISO/no data disks; actual Ring 3 signal ABI passed.
- `python3 scripts/test_smpbench_qemu.py`: BIOS at one/four/eight CPUs; CPU,
  spawn/wait, signals and pipes each reported `ok=1 short=0`. This is functional
  benchmark evidence, not a controlled before/after performance comparison.
- `python3 scripts/test_smp_vmm.py`: BIOS/UEFI at one/four/eight CPUs, disposable
  ISO/paired OVMF/no data disks. Six cases passed after the reaper quiescence
  repair. The final revision additionally requires exact PMM allocation-set
  equality, with the same six-case matrix repeated.

## Failures investigated

New mapping shootdowns initially blocked SMP boot because parked APs did not
service TLB mailboxes. The AP polling change fixes that dependency. The VMM
runner also used the obsolete `fortress> ` prompt and timed out after successful
lifecycle completion; it now accepts the current `fortress:/ $ ` form.

The lifecycle fixture's fixed ten final yields did not wait for remote reapers:
an AP can detach a dead task under its scheduler lock and still be freeing its
stack after the BSP sees empty queues/table counts. An observed UEFI/SMP=8
snapshot had four outstanding frames, one extra stack slot and 1,792 extra heap
bytes; the subsequent exact bitmap snapshot already matched the baseline.
The fixture now waits up to five BSP-tick seconds for stack/heap/deferred
resources to return, then retains exact counter and allocation-set assertions.
Representative failed logs are retained in `build/vmm-atomic-reaper-race.log`,
`build/vmm-atomic-initial-accounting-failure.log` and
`build/vmm-atomic-uefi-accounting-failure.log`.

## Boundaries

Atomic entry access alone does not protect table lifetime. The scheduler/op
references remain mandatory; generic arbitrary-root APIs still take short
registry locks. Buffer validation does not pin data frames or make a later
user copy atomic with concurrent unmap. Pipes and process/signal metadata still
use their own locks. No physical hardware, latency improvement, or general
lock-free VMM claim is made. Batch unmap supports non-global leaves only;
ordinary unmap remains available for global entries. Validation and registry
updates still serialize, while PMM reclamation and ACK waits run outside the
registry lock. No batched map API or allocator-cache redesign is included.

## Follow-up performance investigation

The instrumented first-pass binary is preserved in
`build/smp-perf-baseline/fortress.iso` and `.elf`. It broadcasts on every map,
including private ELF construction. The first SMP=8, seven-repetition run
hit a BSP debug exception at `spin_debug_assert_unheld` with TF unexpectedly
set, followed by an AP shootdown ACK timeout. Its log remains in
`build/smpbench-baseline-smp8-run1.log`; the cause is not established and is
not claimed fixed by these optimizations. Later five-repetition baseline
runs completed; one older self-accounting sample missed its process during
enumeration, motivating direct PID lookup. `--allow-missing-cpu` exists only
to retain throughput from that saved older instrumented binary; current
builds require valid CPU samples.

Before stack batching, three sequential fresh SMP=8 sessions per binary
(one warmup + five timed repetitions per workload) gave these medians of
session medians. These are exploratory TCG measurements, not interleaved
controlled trials or evidence of a throughput win:

| Workload | Baseline median us | Optimized median us | Baseline TLB calls | Optimized TLB calls |
| --- | ---: | ---: | ---: | ---: |
| cpu_scale | 49,264 | 42,243 | 903 | 501 |
| spawn_wait | 299,724 | 648,277 | 21,310 | 11,310 |
| signals | 585,695 | 683,219 | 907 | 506 |
| pipes | 485,559 | 840,800 | 1,725 | 938 |

Evidence: `build/smp-perf-baseline/results-final.json` and
`build/smp-perf-optimized.json`. The final direct-PID accounting revision also
completed all four workloads at 1/4/8 CPUs (`build/smp-perf-final.json`) and
  an SMP=8 seven-repetition run (`build/smp-perf-stress-final.json`) without
reproducing the exception. Stack batching is measured separately below.

Lower contention and fewer broadcasts have not established faster workloads.
Counters include worker launch, warmup, result-file I/O and reclamation around
the command, rather than only the worker's timed loop. Worker clocks measure
wall time, including descheduling; guest APIC tick accounting is coarse and
cannot identify host vCPU descheduling. BSP tick/TSC disagreement is retained
in the raw output rather than silently treating either as precise CPU time.

Workers start as they are spawned, without a common start barrier. The existing
summary uses the slowest worker interval per repetition, not the span from
the first worker's start to the last worker's end. Parent launch/wait elapsed
time is now reported separately. The minimum is not the "true" workload cost,
and total work divided by the longest independently started worker interval
is not a measurement of simultaneous whole-system throughput.

QEMU MTTCG uses one host thread per vCPU and retains synchronization around
some emulated operations; see its [execution and synchronization design](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html).
Host scheduling/emulation overhead is a plausible additional contributor,
not a proven explanation for the observed regressions. A next performance
investigation should profile the remaining kernel-stack mapping/shootdown
path and guest/host scheduling, with a synchronized benchmark and interleaved
binary runs, before changing the allocator or claiming hardware scaling.

## Final range-invalidation acceptance

The final build (`make -j4`, including ISO and raw image) passed host VMM
tests and the six BIOS/UEFI lifecycle cases at 1/4/8 CPUs with exact frame-set
equality. Pipe BIOS/UEFI at one CPU and signal BIOS/UEFI at four CPUs passed.
The range implementation passed all workloads at 1/4 CPUs with five timed
repetitions (`build/smp-perf-range.json`). That runner was interrupted by a
host SIGHUP during the SMP=8 session after cpu_scale; completed records were
retained. A fresh SMP=8 session then completed all four workloads with three
timed repetitions and valid CPU samples (`build/smp-perf-range-completion.json`):
cpu_scale 1.79e9, spawn_wait 3.37e2, signals 1.19e4, pipes 4.87e3 units/sec,
all `ok=1 short=0`. These mixed repetition counts are acceptance evidence,
not a controlled speedup comparison.

An intermediate batch implementation used whole-CR3 reloads. It was replaced
with bounded INVLPG ranges to preserve unrelated translations. Its exploratory
results remain in `build/smp-perf-batch.json`; they do not describe the final
implementation. Performance improvements remain unproven despite the reduced
number of lock acquisitions and broadcast rounds.
