# Benchmark readiness barrier — 2026-10-09

Step 2 adds a user-space barrier to `smpbench`, using the existing pipe and
process APIs. No kernel synchronization, scheduling, syscall ABI or boot
contract was changed.

## Behavior and measurement boundary

Each repetition creates a ready pipe and a release pipe, then spawns all workers.
Each loaded worker parses its arguments, closes the inherited unused pipe ends,
writes its unique ID to the ready pipe, closes that writer, and waits for a
single release byte. The parent validates one unique ID per worker before
writing the complete cohort's release bytes in one small atomic pipe write.
Workers close the gate before entering measured work, so workload descendants
do not inherit barrier descriptors. Four descriptors suffice for the cohort,
including SMP=8; there is no per-worker descriptor multiplication.

On spawn/readiness/release failure the parent closes all barrier ends, kills and
waits for every successfully spawned worker, and marks the repetition failed.
SIGPIPE is ignored in the benchmark parent so broken release is handled as an
error. Wait status now rejects signal termination as success. Worker result
files are removed before spawning to prevent stale results from being accepted.
The existing process exit path closes any descriptors left by a dying worker.

Output identifies `rev=2 barrier=pipe metric=max_worker_us`. Each repetition adds
`setup_us` (pipe creation, spawn and readiness wait) and `barrier_ok`.
`elapsed_us` now starts immediately before release and ends after all workers
have exited; it includes release, scheduling, result-file writes and exit/wait.
Each cohort also reports `start_spread_us`, calculated from the earliest and
latest worker `time_us_start` timestamps.

The summary retains its previous denominator: maximum worker measured duration,
with one warmup excluded. It is not cohort wall time. Start spread uses the
cross-CPU TSC timestamps already collected by the tool and requires a comparable
timebase across CPUs. The barrier does not force simultaneous execution, and
post-release sysinfo/process accounting calls still precede each worker's start
timestamp. Workload setup (signal registration, pipe peer spawn) remains inside
the worker measurement as before. Per-phase attribution is step 3.

Treat rev=2 results separately from earlier results. Command-wide lockstat
deltas include the barrier's allocations and IPC; setup being outside worker
timing does not remove it from those counters. These runs were correctness and
measurement checks, not interleaved performance trials.

## Validation

`make -j4` passed with the existing strict freestanding compiler flags and
generated the bootable ISO/raw image. ASan/UBSan tests execute the actual
benchmark code with syscall adapters and cover successful readiness/release,
spawn failure, readiness EOF, duplicate ID, release EPIPE, second-pipe
allocation failure, successful worker gate and worker gate EOF. They check
descriptor cleanup, cancellation and wait ownership. These adapters do not
establish scheduler behavior.

Real QEMU TCG Ring 3 runs passed under BIOS and UEFI at SMP=1, 4 and 8. Each
session ran all four workloads (`cpu_scale`, `spawn_wait`, `signals`, `pipes`),
one warmup plus three timed repetitions. The validator checked complete unique
worker cohorts, `barrier_ok=1`, checksums/summary success, timestamp order and
independently recomputed start spread. No panic was observed in these runs;
the original debug exception remains unresolved.

Retained evidence:

| Firmware / CPUs | Directory under `build/` |
| --- | --- |
| BIOS / 8 | `barrier-bios8-20261009` |
| UEFI / 8 | `barrier-uefi8-20261009` |
| BIOS / 1, 4 and UEFI / 1, 4 | `barrier-matrix-20261009/{bios,uefi}-smp{1,4}` |

Each contains copied ISO/ELF, hashes, exact QEMU argv, serial logs and outcome
manifests. Panic/timeout capture was armed throughout, with no debugger client
attached during execution. UEFI uses paired OVMF 4M code/variables with a
disposable variables copy. The argv gate excludes data disks; only the ISO and,
for UEFI, those two pflash devices are permitted. Guest command waits are bounded
by the capture runner; user-space pipe waits themselves have no deadline API.
No hardware acceptance is claimed.

Exact invocations:

```sh
python3 scripts/test_smpbench_barrier_host.py
python3 scripts/capture_smp_panic.py --iso bin/fortress.iso --elf bin/fortress.elf \
  --output build/barrier-bios8-20261009 --cpus 8 --runs 1 --reps 3 \
  --dump-ram --require-barrier
python3 scripts/capture_smp_panic.py --iso bin/fortress.iso --elf bin/fortress.elf \
  --output build/barrier-uefi8-20261009 --firmware uefi --cpus 8 --runs 1 --reps 3 \
  --dump-ram --require-barrier
python3 scripts/test_smpbench_barrier.py --cpus 1 4 \
  --output build/barrier-matrix-20261009
```

Repeat the full six-case matrix with `make test-smpbench-barrier`; run the host
adapters alone with `make test-smpbench-barrier-host`. Ordinary `smpbench`
commands now use the barrier automatically. The older benchmark runner accepts
saved rev=1 binaries and validates barrier evidence whenever rev=2 is detected.

## What remains uncertain

Readiness did not remove the large timing spread. In the UEFI SMP=8 session,
timed pipes ranged from 25,729 to 875,522 microseconds; in BIOS SMP=8 they ranged
from 605,998 to 2,281,161 microseconds. Those are isolated diagnostic sessions,
not evidence that firmware or the barrier caused an improvement/regression.
Scheduling/host timing and costs inside the workloads remain candidates.
Proceed to per-phase profiling before choosing a new optimization or comparing
QEMU performance to Dell hardware.
