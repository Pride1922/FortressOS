# Pipe scheduling investigation — 2026-10-09

The ELF copy Dell ABBA comparison improved plain spawn duration by 51.6%
but worsened plain pipe medians by 22.1%. Existing phase instrumentation
reversed the pipe direction; A2's phase export was empty. We must measure
observer effects and scheduling before choosing a wakeup/quantum change.

Existing profiled critical pipe workers show reader header/read mean times
of 6.49/9.04 ms in A1, 5.54/6.34 ms in B1 and 2.80/10.80 ms in B2.
Their corresponding writer queue-to-first-selection measurements range from
2.5 to 45.5 microseconds. Thus initial writer selection does not explain
these millisecond reader intervals. Reader runnable delay is a hypothesis;
blocked producer/data delay, contention and phase/tick alignment remain
possible. Missing A2 phase data and unprofiled waits prevent causal claims.

## Implemented measurements

No scheduling, wakeup, quantum, affinity or pipe transfer policy changes.
`wait_trace_t` is bounded per-TCB state, disabled by default and never
inherited. Existing `sched_wait_until` stamps actual BLOCKED publication;
the wake paths stamp READY publication under the owning scheduler lock;
all four task-selection paths stamp selection before CR3/context switch;
the original sleep continuation stamps resume with IRQs still disabled.
No added locks, allocation, output or blocking in these hooks. Disabled
tasks perform no added TSC reads. Extra checks/TCB footprint still exist
and can affect plain timings, so fresh matched A/B kernels are necessary.

Serialized TSC intervals are:

- blocked: BLOCKED publication to wake/READY publication;
- ready: wake/READY publication to first scheduler selection;
- resume: selection to original wait continuation.

They include elapsed scheduling/host delay and do not measure pure CPU work.
Blocked duration includes outgoing handoff and wake delivery work; ready
duration includes queueing and handoff work, not just a scheduler algorithm.
Selection precedes incoming CR3/context exchange. Aggregate counters cover
all scheduler waits in the reader worker's timed operation (including header,
data reads, trailer/EOF and waitpid), not just pipe data readiness or writer
backpressure. Existing writer birth telemetry remains available in phase mode.
Reversed clocks, overflow, missing/duplicate transitions invalidate evidence.

`SYS_SPAWN_PROFILE` keeps existing actions 0..2 and 160-byte spawn structure.
Additional self-only actions 3/4/5 READ/ENABLE/DISABLE use the independent
72-byte `wait_profile_t`; exact size and writable user range are validated
before state changes. ENABLE resets only own wait counters; DISABLE snapshots.
The task cannot execute a snapshot syscall while sleeping/queued. Counter
writes while blocked/queued use the owner's scheduler lock; running resume
and enable/snapshot are self-owned.

`smpbench --wait-profile` records these clocks without per-call phase TSC
reads or a profiled writer trailer. `-p` records both waits and existing
phases/startup. Plain mode has neither. Worker records add `wait_diag`,
`wait_valid`, equal blocks/wakes/selections/resumes, elapsed interval totals,
maximum ready interval, worker cycle total and frequency. The bounded 4096B
formatter checks append capacity. Clocks/counters are disabled before output.
Both diagnostic modes can affect timing; plain remains the effect measure.

`scripts/analyze_wait_profile.py` validates all three modes, excludes warmup,
preserves every worker and raw-file hash, and reports critical worker waits.
The phase analyzer also exposes critical worker scheduler waits. Parser gates
check transition counts, nonnegative/bounded intervals, exact worker-clock
conversion, and wait totals within the complete worker interval. Historical
logs without wait fields remain readable.

## Evidence

- `make test-wait-profile-host`: actual benchmark adapters and formatter,
  phase/wait-only/disabled modes, pointer/action/size requests; six parser
  rejection tests; actual wait state machine exact intervals, missing wake,
  reverse-clock and overflow cases under ASan/UBSan. No real IRQ/SMP claim.
- Strict freestanding build PASS.
- Six BIOS/UEFI × 1/4/8 CPU ISO-only lifecycle cases PASS with exact physical
  allocation-set/table equality, 100 spawn/exit cycles, zero deferred reaping.
- B BIOS/UEFI SMP=8 phases: pipes/spawn_wait/signals, one warmup and two
  timed reps each; accounting PASS, panic capture armed, no panic observed.
  `build/pipe-wait-{bios,uefi}-20261009`.
- B UEFI SMP=8 and BIOS SMP=4 wait-only pipes: warmup plus two timed reps,
  transitions/interval bounds and benchmark checksum/barrier PASS.
  `build/pipe-wait-only-{uefi8,bios4}-20261009`.
- A BIOS SMP=8 wait-only pipes: warmup plus one timed rep PASS;
  `build/pipe-wait-control-A-20261009`.
- A UEFI USB SMP=1 phase capture persists >4096 bytes, independent debugfs
  validation and offline e2fsck PASS; `build/pipe-wait-usb-A-20261009`.
  Disposable copies only, frozen source image unchanged.
- B UEFI USB SMP=1 wait-only signals/pipes and warmup plus nine timed pipe
  reps persist >4096 bytes, independently validate all transitions with
  debugfs and pass offline e2fsck; `build/pipe-wait-usb-B-wait9-20261009`.
  The earlier `build/pipe-wait-usb-B-20261009` attempt used only five timed
  reps: its shorter wait-only capture did not exceed 4096 bytes and the
  length gate rejected it. That failed evidence is retained; it is not a
  runtime write failure or a passing >4096-byte test.

QEMU results establish the diagnostic path, not the cause of the Dell
regression. Parallel TCG runs/host delays make their durations unsuitable
for the hardware effect comparison.

## Dell matched observer comparison

Bundle `build/pipe-wait-ab-20261009`: slow ELF initialization A versus fast
ELF initialization B, both with identical wait instrumentation, batch kernel
stacks, benchmark/initramfs and all other linked objects. Exact embedded
raw/ISO bytes, fresh linked B/workspace equality and hashes verified. The
images use existing `fortress-elf-copy-{A,B}.img` filenames. No physical
device is flashed by preparation. `make prepare-pipe-wait-comparison` creates
another fresh bundle.

| Variant | Image SHA-256 |
| --- | --- |
| A | `20d0f3b42d7ebd606b76035cdb3feb5df58a337f6faa34d9cd52dd2fa7ebae58` |
| B | `97f8bbe76b1b0ed7a44e8b281cb63e20b037ea5649a8e112f133745eff1c1d2a` |

Boot order A1 -> B1 -> B2 -> A2, Persistent Storage RW, same AC power,
firmware/background/thermal conditions. Per boot use `commands.txt`:
plain -> wait-only -> phase -> phase -> wait-only -> plain; each has eight
workers, seven timed reps and one warmup. This compares observer modes within
each boot as well as ELF variants across boots. Preserve every repetition.
Sync/shutdown, export `pipe-wait-*.txt` into separate
`build/pipe-wait-dell-results/{A1,B1,B2,A2}` folders before overwriting logs.
B2 reboots the same B stick; A2 reflashes A. USB collection remains outside
worker timing but affects cohort spacing. Hardware cause/fix remains pending.

Interpretation after collection: dominant ready intervals support looking at
wake-to-selection service; dominant blocked intervals point to producer/data
availability. Small wait totals with expensive read phases point toward
active syscall work such as copying, validation and wake scans. None alone
proves a particular fix; compare observer modes and retain all repetitions.

## Dell results

All 24 workload exports validate: each has eight workers, seven timed cohorts
plus warmup, correct barrier/worker counts, `ok=1`, `short=0`, and the frozen
benchmark SHA-256 `83f22c90e007d59307319ec606c1c4029de13a650dd84036d03e8833a8ad4775`.
All 16 diagnostic logs pass transition/clock/interval validation; all eight
phase logs additionally pass spawn/writer accounting. There are 168 timed
cohorts (56 per mode), with every repetition and worker retained. Export
filename variations are accepted by content; no exports are empty. Image
variant provenance relies on the user's boot/export labels.

Per-command plain median durations in milliseconds:

| Boot | plain-1 | plain-2 |
| --- | ---: | ---: |
| A1 | 13.525 | 13.447 |
| B1 | 13.344 | 13.216 |
| B2 | 13.270 | 13.229 |
| A2 | 13.675 | 13.624 |

Pooled medians across all 28 timed cohorts per variant/mode:

| Mode | A ms | B ms |
| --- | ---: | ---: |
| Plain | 13.5675 | 13.2445 |
| Wait-only | 13.597 | 13.266 |
| Full phases | 13.724 | 13.450 |

The earlier 22.1% plain pipe regression **does not reproduce in this matched
comparison**: B's median is 2.38% lower, with both B boots below both A boots.
Within each variant wait-only changes the pooled median by +0.16–0.22%,
and full phases by +1.15–1.55%. The earlier observer-dependent reversal is
also absent here. This does not retroactively invalidate the earlier dataset:
the fresh kernels include diagnostic TCB/code footprint changes, the worker
binary is different, and this boot sequence runs only pipes rather than
preceding them with spawn workloads. Tick alignment, warmup/order and other
environment effects remain possible; their individual causes are unproven.

Mean critical-reader wait-only attribution (28 timed cohorts per variant):

| Interval | A ms | B ms |
| --- | ---: | ---: |
| Blocked until wake | 4.198 | 3.103 |
| Runnable until selection | 2.092 | 3.495 |
| Selection to sleep continuation | 0.00212 | 0.00212 |

These are means on each repetition's slowest worker; do not subtract them
from pooled medians. The largest ready interval accounts for 97.6% of all
critical-reader ready time in A and 97.3% in B. Critical readers block only
1–4 times in A and 1–2 in B. Thus one wake-to-selection delay dominates
their measured runnable wait, rather than hundreds of long per-read sleeps.
Both waiting for wake and waiting for selection contribute milliseconds;
the context-switch/resume tail itself is only microseconds.

Full phases corroborate ready delay (A mean 2.640 ms, B 3.347 ms). Critical
reader header/read means are A 5.038/5.752 ms and B 4.665/6.848 ms. In this
new dataset writer startup sometimes also reaches 6.263 ms, unlike the
earlier subset; producer selection can contribute too. Aggregate reader
waits cannot identify whether the dominant delay occurred in header/data
read/trailer/waitpid, or distinguish IPI delivery from service/queueing.

The evidence supports investigating wake-to-selection service as a real
pipe cost, but **does not establish it as the cause of the original
regression**. Prefer a targeted scheduling experiment over a broad quantum
reduction. Before implementing it, audit reschedule IPI handling and safe
post-EOI/return scheduling boundaries, then preserve lock/IRQ/CR3 contracts.
No scheduling policy was changed during this analysis.

Read-only code audit finds `smp_ipi_resched_handler` only increments an IPI
counter. It neither sets a pending-reschedule flag nor calls scheduling;
the hardware interrupt dispatcher acknowledges EOI and returns. Timer
scheduling yields idle tasks immediately or normal tasks at quantum expiry;
blocking and explicit yields are other selection opportunities. Therefore
the current reschedule IPI can wake an idle HLT CPU but does not promptly
preempt a busy CPU for a newly runnable peer. This is consistent with the
measured delays, not a proof that all of them are attributable to this path.
A proposed follow-up is a coalesced per-CPU reschedule request serviced at
an audited safe boundary after EOI; it needs explicit IRQ/context-switch
contract review before implementation. Simply yielding inside the IPI
handler would violate the established ordinary hardware-handler contract.

Counters are supporting session evidence only: A2 has 3,663 remote batches
versus 2,745–2,755 in the other boots, despite complete matching workload
exports, so extra activity/retries cannot be ruled out. Do not normalize
these aggregate snapshots into causal per-workload deltas. Cumulative
max-spin is not subtracted.

Reproduction: `scripts/analyze_wait_profile.py` on the 24 workload logs,
`build/summarize_pipe_wait_dell.py`, `build/pipe-wait-dell-analysis.json`, and
`build/pipe-wait-dell-summary.json` (metadata/raw hashes and phase records).
