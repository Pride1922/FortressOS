# Benchmark per-phase profiling — 2026-10-09

Step 3 is implemented as opt-in `smpbench -p` / `--profile`. It preserves the
readiness barrier and uses the existing user-space syscall APIs. No kernel,
scheduler, locking, syscall transition or ABI contract was changed in this step.

## Measurements

Profile mode identifies itself as `rev=3 profile=1`. Normal runs retain rev=2.
For every measured worker it records raw TSC elapsed ticks, call/block counts
and the longest interval in these disjoint buckets:

| Bucket | Included interval |
| --- | --- |
| `setup` | Signal group/handler setup or pipe creation/sysinfo |
| `spawn` | Noop child or pipe peer spawn syscall |
| `wait` | Child wait syscall |
| `signal` | SIGINT send through return to the caller, including handler/sigreturn |
| `header` | Pipe peer's initial header read |
| `read` | Pipe payload read syscalls |
| `compute` | Integer CPU loop or pipe payload hashing |
| `close` | Pipe endpoint closes and, in profile mode, writer trailer collection/EOF |
| `write` | Writer payload writes; the reader's bucket is zero |
| `other` | Remaining worker duration, including control logic and profiler bookkeeping |

Fields `p_<bucket>`, `p_<bucket>_n` and `p_<bucket>_max` contain elapsed TSC ticks,
counts and longest ticks. `profile_hz` supplies the conversion rate.
`profile_cycles` is the full worker interval; `profile_valid` rejects reversed
clocks, accumulator overflow or an impossible partition. The validator checks
that phase totals plus `other` exactly equal the worker interval and reproduce
its reported microseconds. Counts and maxima must agree with the phase totals.

Profiled pipe writers measure payload generation and writes independently.
An 80-byte private trailer transports their totals, counts, maxima and validity
to the reader after the exact payload. The reader excludes it from the checksum
and rejects incomplete/malformed trailers or trailing bytes. Unprofiled pipes
retain their original stream. Writer time excludes its initial header, trailer
transmission and final close. Reader and writer intervals overlap: **do not add
them as if they were sequential costs**.

Parent rows divide each repetition into `launch` (pipe allocation, unlink/spawn),
`ready` (readiness wait), `release`, `join` and `collect`. Collection includes
reading/unlinking result files and printing the repetition and worker records.
The parent profile line and following barrier line are outside that collection
interval. Result formatting/writing is outside the worker interval, while join
still waits for each worker to finish publishing and exit.

All profiled elapsed intervals include any guest blocking, preemption, spin
time and host descheduling encountered inside them. They are **not CPU-cycle
hardware counters** and cannot distinguish those causes. Existing worker CPU
tick samples remain available at their coarse 100 Hz granularity; zero samples
for short runs do not imply zero CPU work. TSC start spread across workers still
requires comparable clocks across CPUs.

Profiling adds two timestamp reads per measured call/block, bookkeeping,
larger result files/UART output and a pipe trailer. Use it for attribution;
use unprofiled runs for effect measurement. Even unprofiled runs of this new
binary have the larger static buffers and disabled wrapper branches, so the
binary should be held constant in subsequent comparisons.

## Usage

```sh
smpbench -w signals -r 5 -c -p
smpbench -w spawn_wait -r 5 -c -p
smpbench -w pipes -r 5 -c -p
```

In WSL, `make test-smpbench-profile-host` runs the host checks.
`make test-smpbench-profile` runs the BIOS/UEFI x SMP=1/4/8 QEMU matrix with
profiling and panic capture armed. The capture runner also accepts `--profile`.
The analysis tool validates serial logs and writes all timed worker records,
parent durations and the slowest worker's phase attribution per repetition:

```sh
python3 scripts/analyze_smp_profile.py path/to/serial.log \
  --output build/new-profile-analysis.json
```

It excludes warmup, compares each repetition's longest worker, and prints the
fastest/slowest observed repetitions. Neither extreme is called the true cost.

## Verification and artifacts

- Strict freestanding `make -j4` PASS; bootable ISO and raw image generated.
  Compiler stack-usage output reports a 1488-byte `smpbench_main` frame;
  enlarged result buffers are static, not on the stack.
- Actual benchmark code with syscall adapters under ASan/UBSan PASS: barrier
  success and cleanup failures, profile opt-in, accumulation/maxima, reversed
  clock/overflow rejection and bounded field formatting. Actual pipe writer
  and trailer reader tests verify short writes/reads, exact payload bytes,
  counters/partition and truncated, bad-magic or excess-byte rejection.
- Synthetic parser tests PASS: valid evidence and rejection of inconsistent
  phase sums, maxima/counts, clock/status, missing parent rows and writer fields.
  They are evidence-validation tests, not guest behavior claims.
- Final profile binary: BIOS/UEFI x SMP=1/4/8, all four workloads, one warmup plus
  five timed repetitions per workload, PASS. Every worker/barrier and phase
  partition was validated. Each case used a fresh QEMU TCG boot, ISO only, no
  data disks; UEFI used paired OVMF 4M code and disposable variables.
- No panic was observed. The original debug exception remains unresolved.
- Normal (profiling disabled) BIOS/UEFI SMP=8 controls on the same binary,
  one warmup plus three timed repetitions of all four workloads, PASS.

Final profile evidence is under
`build/profile-final-20261009/{bios,uefi}-smp{1,4,8}`. Each case preserves its
ISO/ELF, hashes, exact argv, UART/QEMU logs and outcome manifest. The combined
validated attribution is `build/profile-final-20261009/analysis.json` (24
workload records, 120 timed repetition cohorts). Initial exploration before
writer telemetry was added remains separately under `profile-smp8-20261009`
and `profile-matrix-20261009`; it is not the final binary's verification.

Exact final invocations:

```sh
make test-smpbench-profile-host
make -j4
python3 scripts/test_smpbench_barrier.py --profile --cpus 1 4 8 --reps 5 \
  --output build/profile-final-20261009
python3 scripts/test_smpbench_barrier.py --cpus 8 --reps 3 \
  --output build/profile-controls-final-20261009
```

The latter uses the same binary without profiling under BIOS/UEFI, for a normal
path regression control. Those fresh sessions are not interleaved trials and
cannot estimate profiling overhead or establish a performance improvement.
After the matrix, the formatter's newline/NUL reservation was tightened and
tested at the exact capacity boundary; host checks were rerun and the image
rebuilt. A further SMP=8 profile smoke for pipes/signals is retained under
`build/profile-format-final-20261009` for that formatting change.
No debugger attaches while any guest runs; panic/timeout capture freezes it
first. These are QEMU observations, not Dell acceptance.

## Findings and next decision

For the **slowest timed repetition's longest worker** at SMP=8:

| Workload | BIOS elapsed / dominant share | UEFI elapsed / dominant share |
| --- | --- | --- |
| `cpu_scale` | 53.2 ms / compute 99.6% | 47.0 ms / compute 99.8% |
| `spawn_wait` | 3874.2 ms / spawn 85.2% | 1206.7 ms / spawn 87.8% |
| `signals` | 1677.7 ms / signal 99.7% | 1585.1 ms / signal 99.8% |
| `pipes` | 1995.2 ms / reader reads 76.4% | 1757.5 ms / reader reads 80.3% |

This localizes the expensive intervals but does not identify a kernel lock or
allocator as their cause. In the slow BIOS spawn/wait case, sampled worker CPU
time was approximately 450 ms against 3874 ms elapsed; the longest spawn call
was 389 ms. The gap warrants investigating scheduling/host execution as well
as kernel costs, rather than interpreting all elapsed time as active CPU work.

The slow BIOS pipe reader accumulated 1524.8 ms across 500 payload reads, with
a longest read of only 5.95 ms. Its peer accumulated 1385.2 ms across 500 writes,
with a longest write of 30.55 ms. This shows distributed cost on both ends,
not a single read that explains the entire slow repetition. The corresponding
UEFI peer spent 1343.9 ms in writes. These overlapping observations do not by
themselves distinguish spin contention from blocking/host delay.

Parent collection for those SMP=8 repetitions took roughly 0.45–0.70 seconds.
That observer cost is outside worker timing but changes the time between
cohorts, so profiled throughput should not be compared directly with old runs.

Proceed to **step 4a: environment decision** before optimizing another lock.
Run the same binary with profiling off/on on Dell, retain all worker and parent
records, and determine whether the SMP slowdown and phase distribution persist
outside QEMU TCG. The existing data supports choosing spawn, signal delivery
and both pipe ends for investigation; it does not justify a new VMM/PMM fix.
