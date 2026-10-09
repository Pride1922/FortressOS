# Pipe transfer and handoff attribution

The automated Dell ABBA comparison reproduced roughly 42 critical-reader
blocks in urgent-only B versus roughly one in all-request A. This experiment
keeps those two policies and adds opt-in shared pipe counters to distinguish
transfer fragmentation, empty/full transitions and transfer CPU placement.
It is an attribution experiment, not another scheduling optimization.

## Implementation and interpretation

`SYS_SPAWN_PROFILE` actions 6/7/8 read/enable/disable a separate 184-byte
`pipe_io_profile_t` through a live descriptor belonging to the caller.
The existing spawn and wait layouts/actions are unchanged. The syscall checks
the exact size and writable user range before accessing the object, verifies
the descriptor and rejects ordinary file nodes. Output is staged outside the
lock. A pipe endpoint reference keeps the shared backing alive during access.

Counters are updated under the existing pipe lock, without clocks, allocation,
new locks, changes to wait predicates, or changes to wakeup/scheduling policy.
Plain runs never enable counters. Wait-only and phase pipe runs enable them
before spawning the writer and snapshot/disable after EOF, before closing the
reader. They emit `pi_*` fields in each worker record.

- Positive transfers: call/byte totals, maximum size and <1024 / =1024 /
  >1024-byte buckets for both reader and writer.
- Wait attempts: occasions where the pipe predicate calls `sched_wait_until`.
  A racing peer can satisfy the predicate before actual blocking. These are
  not actual block counts; the existing reader `wait_blocks` measures actual
  scheduler waits, including setup/header/read/close/waitpid phases.
- Buffer transitions: writes into an empty pipe, reads draining it, and writes
  filling it completely.
- CPU masks: logical CPU IDs observed during positive transfers. Consecutive
  direction and CPU changes describe pipe transfers, not context switches.
- Bytes include the 8-byte benchmark header and, in phase mode, the 104-byte
  private writer trailer. EOF contributes no positive transfer. The independent
  analyzer checks exact total bytes, size bucket sums and transition bounds.

Overflow or an unsupported logical CPU invalidates the profile. Enable resets
the aggregate; disable freezes it. Metadata/setup transfers are intentionally
included, so tiny-transfer counts need that qualification. Added lock-held
counter work has observer cost. Plain timings measure the A/B policy effect;
profiling timings attribute behavior and are compared separately.

## Verification — 2026-10-09

- Strict `make -j2 ... all` build PASS. Actual pipe/VFS/syscall host adapters
  under ASan/UBSan PASS: normal transfers, wrap/boundaries, blocking/closure,
  lifetime/rollback/SIGPIPE plus profile reset/disable/overflow, transfer
  histograms, transitions, CPU masks and exact-size/range/fd/non-pipe rejection.
- `make test-smpbench-profile-host test-benchrun-host` PASS: benchmark and
  automation adapters plus seven evidence parser tests, including inconsistent
  bytes/buckets/transitions, invalid masks and partial pipe profiles.
- Both frozen A/B images pass `scripts/test_benchrun_image.py` on separate
  disposable UEFI USB copies: two automated smoke runs, duplicate directory
  refusal, all seven workload files independently read from ext2, phase files
  above 4096 bytes, completion/sync/shutdown and clean offline fsck. Originals
  remain unchanged. This USB runner uses one CPU and smoke iteration counts.
- Frozen SMP=8 BIOS phase and UEFI wait-only runs pass for each A/B policy,
  three timed repetitions plus warmup. B BIOS and A UEFI additionally run
  spawn_wait and signals. All barriers/accounting pass; no panic observed;
  stopped all-CPU capture succeeds without breakpoints or memory mutation.
- Independent audit validates 128 warmup/timed SMP=8 pipe records and 48 timed
  USB pipe records: exact bytes including metadata, size buckets, transition
  limits, CPU mask bounds and absent pipe counters in plain runs. Evidence:
  `build/pipe-handoff-validation-20261009.json` and retained audit script.

QEMU establishes exercised correctness and counter consistency, not a Dell
performance result or a general race-freedom proof. Physical attribution is
pending the new captures.

## Dell procedure

Frozen matched bundle: `build/pipe-handoff-final-ab-20261009`. A services urgent
and fresh-work requests; B services urgent requests only. Both contain the
same pipe instrumentation, benchmark, automation and other kernel objects.
Only the existing `thread.o` policy differs. Embedded kernel/initramfs hashes,
source snapshots and image checks are retained in the bundle.

Boot Persistent Storage, AC connected, same firmware/thermal conditions:

1. Flash A; `benchrun A1 --shutdown`.
2. Export `/mnt/policy-A1/` before flashing B.
3. Flash B; `benchrun B1 --shutdown`, export `/mnt/policy-B1/`.
4. Reboot the same B stick; `benchrun B2 --shutdown`, export `/mnt/policy-B2/`.
5. Flash A; `benchrun A2 --shutdown`, export `/mnt/policy-A2/`.

Export each directory's complete contents, including `complete.txt`, to
`build/pipe-handoff-dell-results/A1`, `B1`, `B2`, or `A2`. Existing output
directories are refused. Do not use `--smoke` on the Dell. The host preparation
and tests never flash physical devices. No hardware improvement is claimed.
