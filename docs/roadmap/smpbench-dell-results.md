# Dell SMP environment decision — 2026-10-09

The user exported a completed physical session to `build/dell-results`.
Sixteen benchmark invocations have complete summaries with `ok=1 short=0`;
all pass the existing worker-completeness/readiness-barrier validator. Six
profile invocations also pass phase, parent and pipe-writer validation (30
timed cohorts). The guest benchmark SHA-256 matches the frozen bundle:
`53c0a4bb5aa8cd92c2ab8f018f3c0089c6568cb0de5e18a33fe57545d7842744`.
Sysinfo reports eight online CPUs, 100 Hz preemption, invariant TSC calibrated
at 1.90 GHz and 31.1 GiB managed RAM. The selected USB data PARTUUID matches.
These exported logs do not identify the precise CPU model, firmware mode or
power/thermal conditions; those remain unrecorded for this session.

## Observations

All times below are the maximum worker duration per cohort. The four medians
are consecutive plain/profile/profile/plain invocations, each with five timed
repetitions and one excluded warmup. They are not old/new kernel trials.

| Workload, eight workers | Plain 1 | Profile 1 | Profile 2 | Plain 2 | Largest/smallest timed duration across all four |
| --- | --- | --- | --- | --- | --- |
| signals | 4.357 ms | 4.344 ms | 4.273 ms | 4.219 ms | 1.079x |
| spawn_wait | 37.536 ms | 38.473 ms | 36.327 ms | 37.403 ms | 1.164x |
| pipes | 19.073 ms | 15.305 ms | 15.448 ms | 15.235 ms | 1.297x |

CPU reference median is 175.132 ms. Single-worker references (all eight CPUs
still online) report signals 2.646 ms and pipes 6.085 ms. The file named
`dell-smp-11-one-spanw_wait.txt` actually contains another **signals** run
(2.228 ms), so single-worker spawn_wait is missing. Interpret contents, not
filenames. All multi-worker spawn_wait logs contain the intended workload.

Mean phase shares across each workload's ten profiled critical workers:

- Signals: signal calls 96.2% of elapsed worker time.
- Spawn/wait: spawn calls 64.8%, wait calls 35.2%.
- Pipes: initial header read 41.8%, payload reads 38.0%, spawn 5.4%,
  checksum computation 7.4%. The header interval includes waiting for the
  newly spawned writer to run and publish its header, not just copying bytes.

Phase intervals include blocking, preemption and lock waits; they are not
active CPU costs. At these short durations the 10 ms sampled CPU ticks are
too coarse to determine CPU/elapsed gaps reliably.

Aggregate lock deltas between exported snapshots: VMM 3,590,086 acquisitions
and 1,868,354 contentions (52.0%); PMM 277,358 and 30,190 (10.9%); process
75,299 and 14,593 (19.4%). These cover all intervening work and idle time,
including the failed capture and USB output, not isolated workloads. Counter
maximums are cumulative maxima, not subtractable interval measurements.
Contention counts alone cannot establish which lock dominates elapsed time.

## Decision and limits

Use **Dell for optimization effect measurements**, with QEMU retained for
correctness, repeatable scenarios and frozen panic capture. The large
multi-second QEMU tails did not appear in this hardware session: previous
UEFI TCG plain medians at eight workers were approximately signals 1208.8 ms,
spawn_wait 1335.3 ms and pipes 1978.7 ms. Conversely CPU reference was faster
in QEMU (43.939 ms), so this is not a uniform machine-speed difference.
This supports treating TCG/host execution as a substantial confounder;
it does not identify the precise cause or show all kernel contention solved.

Historical QEMU matrix/control runs precede the final output formatter bound
fix and use serial capture, different firmware/storage conditions and fewer
plain repetitions. Comparisons above are contextual, not matched causal
speedups. The Dell binary identity itself is verified against its bundle.

Full Dell stdout was redirected to the USB after correcting the 4096-byte
`/tmp` limit. Parent collection takes roughly 196–413 ms in profiled cohorts,
outside worker timing but affecting cohort spacing. Keep capture mode and
power/thermal conditions consistent in future trials and confirm conclusions
with terminal/serial capture. One physical session does not measure run-to-run
variance. Profiling pairs are close for signals/spawn_wait; pipes has an order
effect, so these data do not establish zero profiling overhead.

## Next work

Step 4a is supported sufficiently to select the measurement environment.
Before choosing a kernel optimization, attribute the hardware spawn path
(ELF/file loading, allocations, mappings and publication) and pipe writer
startup/scheduling interval. Obtain workload-specific lock deltas or timed
subphases rather than assigning aggregate VMM contention to either workload.
Then perform step 4b interleaved old/new trials on Dell using the same capture
mode and actual worker counts. Recover the missing one-worker spawn_wait
reference when convenient; it does not block this environment decision.

Validated artifacts: `build/dell-results-analysis.json` (phase attribution)
and `build/dell-results-validation.json` (all complete summaries and limits).
Original exported logs are preserved unchanged. No kernel changes or new
performance improvement are claimed by this analysis.
