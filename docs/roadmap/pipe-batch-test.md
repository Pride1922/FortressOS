# Fixed-payload writer batching experiment

The previous Dell attribution found repeated reader waits without transfer
fragmentation or CPU-placement changes. This experiment changes only user
benchmark writer batching; it does not tune pipe wakeups or scheduling.

One image and one boot compare `pipes` (1 KiB writes) and `pipes4k` (4 KiB
writes). Both transfer identical 500 KiB deterministic payloads per worker,
with identical expected checksum, 8-byte header, 1 KiB reader requests,
eight pinned readers and neighboring pinned writers. The kernel policy is
the same urgent-only policy throughout the boot. The final incomplete batch
is supported; full runs use exactly 500 units, divisible by four. Writes
remain within PIPE_BUF. Short writes still use the existing retry loop.

`benchrun A1 --batch-test --shutdown` creates `/mnt/batch-A1/` and runs
1k/4k/4k/1k plain captures, then that same order with wait/pipe profiling.
Each capture has a warmup and seven timed repetitions. The existing plain
spawn check and tool hash/sysinfo/lockstat captures are retained. File names,
workload names and every worker's `pipe_batch=1|4` record the size directly;
completion metadata records the experiment, same-kernel order and run label.
There is no kernel A/B image distinction to remember. Plain results measure
effect; wait-only results attribute cost, with observer overhead qualified.

Host sanitizer tests verify identical byte streams including partial writes
and 1–5 unit tail boundaries, both with and without the phase trailer, and
runner command order/options. Existing profile parser handles ceil(iterations
/ batch) writer compute phases. Independent guest audit verifies all saved
cohorts/checksums, 512008 total bytes and 501 versus 126 positive writes
including the header, 1024 versus 4096 maximum writes, sync/shutdown, immutable
original image and clean offline filesystem on disposable USB copies.

The initial writer used a local 4 KiB buffer and faulted below the small guest
user stack; the failures were followed by TLB ACK timeouts. Those failed QEMU
artifacts and the initial image are retained under `build/pipe-batch-*` and
must not be flashed. The corrected writer uses static process-local storage
as the original writer did; no stack/mapping contract changes were made.

Bundle: `build/pipe-batch-verified-dell-20261009/fortress-dell-smpbench.img`.
Export every file from `/mnt/batch-A1/` into the pre-created
`build/pipe-batch-dell-results/` directory. Physical effect remains unmeasured.

Corrected-image verification (2026-10-09): strict build and host sanitizer
targets PASS. Actual full-size SMP=8 BIOS and UEFI disposable USB captures
PASS: eight interleaved pipe workloads plus spawn check, exact checksums,
payload/write counts/sizes, pinned CPU masks, independent persisted-file
parsing, completion/sync/shutdown and clean offline fsck. Frozen image hash:
`dd71497ca86eb533420c5ca800d0696dcd520b1832d735ff6c02be7e82550469`.
Evidence: `build/pipe-batch-verified-bios-20261009` and
`build/pipe-batch-verified-uefi-20261009`. QEMU timings are not hardware effect
measurements. No kernel/scheduler policy changes were made for this test.

## Dell result: reject 4 KiB batching

All 14 exports are present, including matching benchmark hash, eight online
CPUs and the exact experiment/run completion marker. Eight pipe captures
independently validate barrier/cohort records, checksums, size labels, exact
payload/write counts and pinned CPU placement; the spawn check also validates.
Warmups excluded: 56 timed pipe cohorts (14 per size/mode), plus five spawn
cohorts. Raw exports remain unchanged. This is one interleaved physical boot,
not a repeated-boot confidence estimate.

| Measurement | 1 KiB | 4 KiB |
| --- | ---: | ---: |
| Plain pooled median | 13.747 ms | 17.7855 ms |
| Plain pooled mean | 13.8275 ms | 17.7916 ms |
| Wait-only pooled median | 14.2785 ms | 17.899 ms |
| Wait-only critical reader blocks, mean | 60.57 | 4.07 |
| Critical reader blocked time, mean | 5.217 ms | 7.510 ms |
| Shared pipe writer wait attempts, mean | 0 | 86.43 |
| Shared pipe full-fill events, mean | 0 | 81 |
| Successive transfer CPU changes, mean | 892 | 241.86 |

Plain 4 KiB median regresses 29.4%; wait-only median regresses 25.4%. Both
plain 4 KiB captures regress against both 1 KiB captures; observed plain
ranges are 13.203–14.775 ms for 1 KiB and 16.863–19.517 ms for 4 KiB.
Fewer reader blocks, writes and transfer CPU changes are not a throughput
win. Profiles show substantial writer backpressure and longer reader blocked
time. Writer counters count predicate wait attempts, not measured actual
scheduler blocks; transfer CPU changes are not context switches. These
observations suggest batching redistributes waiting, but do not isolate the
precise scheduling/lock cost or prove that profile behavior explains the
entire plain timing difference.

Do not adopt 4 KiB writer batching as a performance fix. Keep the established
1 KiB default. No scheduling or wakeup change is justified solely by these
counters. Evidence: `build/pipe-batch-dell-analysis.json`, retained analysis
script `build/analyze_pipe_batch_dell.py`, and raw
`build/pipe-batch-dell-results/` exports with SHA-256 hashes in the analysis.
