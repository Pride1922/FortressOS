# One-command Dell benchmark capture — 2026-10-09

The user requested automation of the manual per-boot benchmark sequence.
The shell has no script-file execution mode. /bin/benchrun is a small
freestanding Ring 3 runner using the existing spawn/fd-action/wait/filesystem
ABI; no kernel or scheduling changes are required.

Usage: benchrun A1|B1|B2|A2 [--shutdown]. --smoke selects one worker, three
repetitions and three iterations for disposable QEMU verification only.
Default Dell mode runs the exact existing sequence: benchmark hash, sysinfo,
locks-before, pipes plain/wait-only/phase/phase/wait-only/plain with n=8/r=7,
spawn_wait plain n=8/r=5, locks-after, sync and optional system poweroff.
All benchmark commands include -c. Progress is printed between commands.

Each invocation creates a new /mnt/policy-RUN directory and refuses any
existing directory. Captures retain their policy-NN names. Stdout and stderr
share each capture; the runner waits for every child and stops on launch,
wait/status, read, write diagnostic, incomplete benchmark summary or sync
failure. Failure retains partial results and does not automatically power off.
A completion record is written before the final sync; success is reported only
if that sync succeeds. Offline validators remain the definitive complete-cohort
and filesystem checks. Run labels describe the user's intended boot and do
not detect/attest the kernel variant.

Automatic shutdown uses SYS_REBOOT mode 2, the same public power ABI as the
shell. The initial guest test caught accidental use of the similarly named
socket SYS_SHUTDOWN; that runner image is retained but not delivered.
Corrected bundle: build/benchrun-ab-20261009. Both variants retain
the previous scheduler policy code and share the new initramfs/runner. Frozen
older policy images do not contain benchrun. The runner's sleeping parent
adds a process relative to manual shell launches; this is common to both new
variants, and prior manual timings should not be treated as an exact matched
baseline for the automated trial.

## Verification

Strict build and make test-benchrun-host PASS. Host adapters exercise actual
runner code, eleven child launches and fd actions, default n/reps/flags,
existing-directory rejection, invalid labels, split capture reads, child launch/
exit, incomplete capture and sync failures, with no shutdown on failure.
Corrected UEFI USB A/B tests PASS. They run smoke A1 and B1 captures,
reject a duplicate A1 directory, use automatic shutdown, independently read
all output files with debugfs and validate each of seven workloads, verify
phase files exceed 4096 bytes, audit clean ext2 and unchanged original images.
No full-size or physical acceptance is claimed by the smoke test.

## Physical use

Flash new A, boot Persistent Storage and enter benchrun A1 --shutdown.
Export the contents of /mnt/policy-A1 to build/resched-policy-auto-dell-results/A1.
Repeat B1, B2 (same B stick reboot) and A2 (reflash A), using the corresponding
label. Export before reflashing destroys previous persistent captures.
There is no need to rerun the previous experiment solely for automation;
use this command for the next agreed matched experiment.
Evidence: build/benchrun-final-usb-{A,B}-20261009. Both contain raw guest logs, independently extracted captures and result.txt; frozen originals remain unchanged.

## Automated Dell ABBA exports — 2026-10-09

All 28 workload captures independently validate: correct benchmark hash,
eight CPUs/workers, seven timed pipe reps or five spawn reps, complete barrier,
checksum and profile records and exact reconstructed min/median/max. Warmups
excluded: 168 timed pipe cohorts and 20 spawn cohorts. A1/B1/A2 also have the
correct default-mode completion record. B2/complete.txt was not exported;
its seven workload captures and locks-after file are present and valid, but
that export alone cannot confirm the final runner completion/sync record.
No rerun is needed to calculate the workload comparison; a retained B2
completion record would close that metadata gap. Raw files are unchanged.

| Measure | A all-request service | B urgent-only service |
| --- | ---: | ---: |
| Plain pipe pooled median (28 cohorts each) | 13.1135 ms | 13.4705 ms |
| Plain pipe pooled mean | 13.7518 ms | 13.4708 ms |
| Plain pipe observed maximum | 17.612 ms | 14.901 ms |
| Wait-only pipe pooled median | 13.1555 ms | 13.5105 ms |
| Phase pipe pooled median | 13.2635 ms | 13.5795 ms |
| Plain spawn_wait median (10 cohorts each) | 14.526 ms | 14.638 ms |
| Wait-only critical reader blocks, mean | 1.214 | 42.5 |

B pipe median is 2.72% higher, mean 2.04% lower, and observed maximum 15.39%
lower. Spawn median is 0.77% higher, a small difference in this sample.
This reproduces the qualitative prior manual result: urgent-only service
reduces slow pipe samples while typical pipe elapsed remains slightly higher.
B critical-reader blocked time averages 5.745 ms versus A 1.505 ms. Aggregate
ready time spans many more waits in B and is not a per-wakeup latency estimate.
Phase reader read elapsed grows 7.009 -> 10.544 ms, while spawn call falls
2.715 -> 0.605 ms. Writer write elapsed grows 8.339 -> 10.488 ms. Frequent
reader blocking remains the next attribution lead; these are elapsed-time
observations, not measured switch counts or proof of causal CPU overhead.

Per-file plain medians A1 13.050/13.129 ms; B1 13.474/13.626 ms;
B2 13.339/13.241 ms; A2 13.088/13.188 ms. No new scheduling changes were made
while analyzing these exports. Keep automation for future experiments and
instrument pipe transfer sizes, block/handoff reasons and CPU placement before
further tuning. The added sleeping runner parent is shared by both variants;
manual and automatic trial absolute timings are not a single matched dataset.

Evidence and raw hashes: build/resched-policy-auto-dell-analysis.json,
build/resched-policy-auto-dell-summary.json,
build/resched-policy-auto-dell-attribution.json,
build/summarize_resched_policy_auto_dell.py and
build/attribute_resched_policy_auto_dell.py.